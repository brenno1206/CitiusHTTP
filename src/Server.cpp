#include "citius/Server.hpp"
#include "citius/HttpParser.hpp"
#include "citius/HttpResponse.hpp"
#include "citius/Log.hpp"
#include "citius/RequestReader.hpp"
#include "citius/StaticFiles.hpp"
#include <arpa/inet.h>  // ntohs()
#include "citius/ThreadPool.hpp"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <exception>
#include <format>
#include <memory>
#include <netinet/in.h> // Para struct sockaddr_in, htons(), htonl()
#include <optional>
#include <poll.h>
#include <stdexcept>    // Para lançar std::runtime_error
#include <string>
#include <sys/socket.h> // Para socket(), bind(), listen()
#include <thread>
#include <utility>      // std::move

namespace citius {

namespace {

// Teto para linha de requisicao + cabecalhos; acima disso respondemos 431. 8 KB e um limite comum (ex.: nginx)
constexpr std::size_t kMaxRequestHeadBytes = 8192;

// De quanto em quanto tempo o laco de accept acorda para ver se pediram para parar
constexpr int kAcceptPollMs = 100;

// Pausa depois de um erro persistente de accept/poll (ex.: sem fds), para nao girar em laco apertado
constexpr auto kAcceptErrorPause = std::chrono::milliseconds(10);

// Um handler com bug nao pode derrubar o servidor nem deixar o cliente sem resposta.
HttpResponse route_safely(const Router& router, const HttpRequest& request) {
    try {
        return router.route(request);
    } catch (const std::exception& e) {
        log_error(std::format("Handler lancou excecao: {}", e.what()));
    } catch (...) {
        log_error("Handler lancou excecao desconhecida.");
    }
    return HttpResponse::make_error(StatusCode::INTERNAL_SERVER_ERROR);
}

// Decide a resposta para o que foi lido do cliente; std::nullopt = nao ha a quem responder.
std::optional<HttpResponse> build_response(const ReadResult& head, const Router& router) {
    switch (head.status) {
        case ReadStatus::TOO_LARGE:
            log_info("(cabecalhos grandes demais) -> 431");
            return HttpResponse::make_error(StatusCode::HEADERS_TOO_LARGE);
        case ReadStatus::TIMEOUT: {
            log_info("(cliente parado) -> 408");
            HttpResponse timeout = HttpResponse::make_error(StatusCode::REQUEST_TIMEOUT);
            (void)timeout.set_header("Connection", "close");
            return timeout;
        }
        case ReadStatus::CLOSED:
        case ReadStatus::ERROR:
            log_info("(conexao encerrada antes de completar a requisicao)");
            return std::nullopt;
        case ReadStatus::COMPLETE:
            break;
    }

    ParseResult parsed = HttpParser::parse(head.data);
    if (!parsed.ok()) {
        StatusCode status = to_status(*parsed.error);
        log_info(std::format("(requisicao invalida) -> {}", static_cast<int>(status)));
        return HttpResponse::make_error(status);
    }

    HttpResponse response = route_safely(router, *parsed.request);
    // fechamos a conexao apos cada resposta; a RFC pede que o servidor avise
    (void)response.set_header("Connection", "close");

    log_info(std::format("{} {} -> {}", method_name(parsed.request->method), parsed.request->path,
                         static_cast<int>(response.status())));
    return response;
}

} // namespace

Server::Server(std::uint16_t port, std::filesystem::path root, std::size_t threads, std::size_t max_queued)
    : m_root(std::move(root)),
      m_threads(threads != 0 ? threads : std::max<std::size_t>(2, std::thread::hardware_concurrency())),
      m_max_queued(max_queued) {
    if (max_queued == 0) {
        throw std::invalid_argument("Server: max_queued deve ser maior que zero.");
    }
    // captura a raiz por valor: o fallback nao pode depender do endereco do Server (que e movivel)
    m_router.set_fallback([root = m_root](const HttpRequest& request) { return serve_static(root, request); });
    start(port);
}

void Server::start(std::uint16_t port) {
    m_server_socket.reset(socket(AF_INET, SOCK_STREAM, 0));

    if(!m_server_socket.valid()) {
        throw std::runtime_error("Erro na criacao do socket\n");
    }

    // precisa vir antes do bind, senao reiniciar o servidor falha com "Address already in use"
    if(!m_server_socket.set_reuse_addr()) {
        throw std::runtime_error("Falha ao definir SO_REUSEADDR.\n");
    }

    sockaddr_in address{
        .sin_family = AF_INET,                // IPv4
        .sin_port = htons(port),              // htons: converte para a ordem de bytes da rede (big-endian)
        .sin_addr = { .s_addr = INADDR_ANY }  // aceita conexoes em qualquer interface da maquina
    };
    if(bind(m_server_socket.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0) {
        throw std::runtime_error("Erro no bind.\n");
    }

    // 10 = tamanho da fila de conexoes aguardando accept()
    if(listen(m_server_socket.get(), 10) < 0) {
        throw std::runtime_error("Falha no listen.\n");
    }

    // com porta 0 so agora o sistema sabe qual porta escolheu
    sockaddr_in bound{};
    socklen_t bound_size = sizeof(bound);
    if(getsockname(m_server_socket.get(), reinterpret_cast<sockaddr*>(&bound), &bound_size) < 0) {
        throw std::runtime_error("Falha no getsockname.\n");
    }
    m_port = ntohs(bound.sin_port);

    log_info(std::format("Servidor escutando na porta {}...", m_port));
}

Socket Server::accept_client() {
    while (!m_stop.load()) {
        // poll com timeout em vez de accept bloqueante: assim request_stop() e notado em ate
        // kAcceptPollMs, sem depender de um sinal interromper o accept (corrida entre checar e bloquear)
        pollfd pfd{.fd = m_server_socket.get(), .events = POLLIN, .revents = 0};
        int ready = poll(&pfd, 1, kAcceptPollMs);
        if (ready == 0 || (ready < 0 && errno == EINTR)) {
            continue;
        }
        if (ready < 0) {
            log_error("Falha no poll do socket de escuta.");
            return Socket{};
        }

        // nullptr, nullptr: por enquanto nao queremos saber o endereco do cliente
        int fd = accept(m_server_socket.get(), nullptr, nullptr);
        if (fd < 0) {
            if (errno == EINTR || errno == ECONNABORTED) {
                continue; // sinal, ou o cliente desistiu antes do accept: tenta o proximo
            }
            log_error("Falha ao aceitar conexao.");
            return Socket{};
        }

        Socket client{fd};
        // SO_NOSIGPIPE e por socket: precisa ser definido em cada cliente aceito
        if (!client.set_no_sigpipe()) {
            log_error("Falha ao definir SO_NOSIGPIPE no cliente.");
            continue;
        }
        return client;
    }
    return Socket{};
}

void Server::set_read_timeout(std::chrono::milliseconds timeout) noexcept {
    if (timeout > std::chrono::milliseconds::zero()) {
        m_read_timeout_ms.store(timeout.count());
    }
}

void Server::handle_client(Socket& client) const {
    // por socket (como o SO_NOSIGPIPE); sem isso um cliente mudo prenderia a thread para sempre
    if (!client.set_recv_timeout(std::chrono::milliseconds(m_read_timeout_ms.load()))) {
        log_error("Falha ao definir o timeout de leitura no cliente.");
    }
    std::optional<HttpResponse> response = build_response(read_request_head(client, kMaxRequestHeadBytes), m_router);
    if (response && !client.send_all(response->serialize())) {
        log_error("Falha ao enviar resposta.");
    }
}

void Server::serve_one() {
    Socket client = accept_client();
    if (client.valid()) {
        handle_client(client);
    }
    // sem close(): o destrutor de 'client' fecha o fd ao sair da funcao
}

void Server::run() {
    // o pool e local a run(): ao sair do escopo o destrutor deixa os clientes ja enfileirados
    // terminarem e faz join, que e exatamente o encerramento gracioso
    ThreadPool pool{m_threads, m_max_queued};

    while (!m_stop.load()) {
        Socket client = accept_client();
        if (!client.valid()) {
            if (!m_stop.load()) {
                std::this_thread::sleep_for(kAcceptErrorPause);
            }
            continue;
        }

        // std::function exige tarefa copiavel e Socket e move-only: o shared_ptr carrega o cliente.
        // Se o submit falhar, a tarefa e descartada mas nossa copia do ponteiro continua valida
        auto shared = std::make_shared<Socket>(std::move(client));
        if (!pool.submit([this, shared] { handle_client(*shared); })) {
            log_info("(fila cheia) -> 503");
            HttpResponse busy = HttpResponse::make_error(StatusCode::SERVICE_UNAVAILABLE);
            (void)busy.set_header("Retry-After", "1");
            (void)busy.set_header("Connection", "close");
            if (!shared->send_all(busy.serialize())) {
                log_error("Falha ao enviar resposta.");
            }
        }
    }
    log_info("Encerrando: esperando os clientes em andamento...");
}

} // namespace citius
