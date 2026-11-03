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
#include <string_view>
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

HttpResponse close_with(StatusCode status) {
    HttpResponse response = HttpResponse::make_error(status);
    (void)response.set_header("Connection", "close");
    return response;
}

// O que fazer depois de ler uma requisicao.
struct Outcome {
    std::optional<HttpResponse> response; ///< std::nullopt = nao ha a quem responder
    bool keep_alive = false;              ///< true = esperar outra requisicao na mesma conexao
    std::string surplus;                  ///< bytes ja lidos que pertencem a proxima requisicao
};

// Decide a resposta para o que foi lido do cliente. Le o corpo do socket quando a cabeca anuncia um
// Content-Length. Todo erro fecha a conexao: depois dele nao sabemos mais onde a proxima requisicao
// comeca, entao reaproveitar o fluxo seria arriscado.
Outcome build_response(const ReadResult& head, Socket& client, const Router& router, bool may_keep_alive) {
    switch (head.status) {
        case ReadStatus::TOO_LARGE:
            log_info("(cabecalhos grandes demais) -> 431");
            return {close_with(StatusCode::HEADERS_TOO_LARGE), false, {}};
        case ReadStatus::TIMEOUT:
            log_info("(cliente parado) -> 408");
            return {close_with(StatusCode::REQUEST_TIMEOUT), false, {}};
        case ReadStatus::CLOSED:
            // fim normal de uma conexao keep-alive: o cliente terminou e fechou
            if (!head.data.empty()) log_info("(conexao encerrada antes de completar a requisicao)");
            return {};
        case ReadStatus::ERROR:
            log_info("(erro de leitura)");
            return {};
        case ReadStatus::COMPLETE:
            break;
    }

    // so a cabeca vai ao parser; o que veio depois dela e o comeco do corpo (ou da proxima requisicao)
    const std::string_view data{head.data};
    ParseResult parsed = HttpParser::parse(data.substr(0, head.head_size));
    if (!parsed.ok()) {
        StatusCode status = to_status(*parsed.error);
        log_info(std::format("(requisicao invalida) -> {}", static_cast<int>(status)));
        return {close_with(status), false, {}};
    }

    std::string surplus(data.substr(head.head_size));
    if (parsed.request->content_length > 0) {
        ReadResult body = read_request_body(client, surplus, parsed.request->content_length);
        if (body.status == ReadStatus::TIMEOUT) {
            log_info("(corpo incompleto, cliente parado) -> 408");
            return {close_with(StatusCode::REQUEST_TIMEOUT), false, {}};
        }
        if (body.status != ReadStatus::COMPLETE) {
            log_info("(conexao encerrada antes de completar o corpo)");
            return {};
        }
        parsed.request->body = std::move(body.data);
        surplus = std::move(body.surplus);
    }

    const bool keep_alive = may_keep_alive && parsed.request->keep_alive();
    HttpResponse response = route_safely(router, *parsed.request);
    // a RFC pede que o servidor avise quando vai fechar; "keep-alive" explicito tambem serve ao HTTP/1.0
    (void)response.set_header("Connection", keep_alive ? "keep-alive" : "close");

    log_info(std::format("{} {} -> {}", method_name(parsed.request->method), parsed.request->path,
                         static_cast<int>(response.status())));
    return {std::move(response), keep_alive, std::move(surplus)};
}

// Espera chegar algo na conexao (a proxima requisicao, ou o fim dela). Faz poll em fatias curtas,
// como accept_client, para notar o pedido de parada sem esperar o prazo todo.
// @return true se ha dados para ler; false se estourou o prazo, pediram para parar ou deu erro.
bool wait_for_next_request(const Socket& client, std::chrono::milliseconds idle, const std::atomic<bool>& stop) {
    const auto deadline = std::chrono::steady_clock::now() + idle;
    while (!stop.load()) {
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (left <= std::chrono::milliseconds::zero()) return false;

        pollfd pfd{.fd = client.get(), .events = POLLIN, .revents = 0};
        int ready = poll(&pfd, 1, static_cast<int>(std::min<std::chrono::milliseconds::rep>(left.count(), kAcceptPollMs)));
        if (ready > 0) return true; // dados, ou POLLHUP/POLLERR: o proximo recv descobre qual
        if (ready < 0 && errno != EINTR) return false;
    }
    return false;
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

    // fila de conexoes aguardando accept(). Com 10, uma rajada de ~30 clientes perdia conexoes no macOS
    // (o cliente via a conexao cair sem resposta); SOMAXCONN pede o maximo que o SO permite
    if(listen(m_server_socket.get(), SOMAXCONN) < 0) {
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

void Server::set_idle_timeout(std::chrono::milliseconds timeout) noexcept {
    if (timeout > std::chrono::milliseconds::zero()) {
        m_idle_timeout_ms.store(timeout.count());
    }
}

void Server::handle_client(Socket& client) const {
    // por socket (como o SO_NOSIGPIPE); sem isso um cliente mudo prenderia a thread para sempre
    if (!client.set_recv_timeout(std::chrono::milliseconds(m_read_timeout_ms.load()))) {
        log_error("Falha ao definir o timeout de leitura no cliente.");
    }

    std::string carry; // bytes ja lidos que pertencem a proxima requisicao (requisicoes encadeadas)
    for (std::size_t served = 0; served < kMaxRequestsPerConnection; ++served) {
        // a primeira requisicao segue o timeout de leitura (silencio = 408). Nas seguintes, esperar
        // calado e normal: nao ha requisicao a responder, so fechamos quando o prazo de ociosidade acaba
        if (served > 0 && carry.empty() &&
            !wait_for_next_request(client, std::chrono::milliseconds(m_idle_timeout_ms.load()), m_stop)) {
            return;
        }

        const bool last = served + 1 == kMaxRequestsPerConnection || m_stop.load();
        Outcome outcome = build_response(read_request_head(client, kMaxRequestHeadBytes, carry), client, m_router, !last);
        if (outcome.response && !client.send_all(outcome.response->serialize())) {
            log_error("Falha ao enviar resposta.");
            return;
        }
        if (!outcome.keep_alive) return;
        carry = std::move(outcome.surplus);
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
