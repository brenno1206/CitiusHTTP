#include "citius/Server.hpp"
#include "citius/HttpParser.hpp"
#include "citius/HttpResponse.hpp"
#include "citius/RequestReader.hpp"
#include <arpa/inet.h>  // ntohs()
#include <cerrno>
#include <format>
#include <iostream>
#include <netinet/in.h> // Para struct sockaddr_in, htons(), htonl()
#include <optional>
#include <stdexcept>    // Para lançar std::runtime_error
#include <string>
#include <sys/socket.h> // Para socket(), bind(), listen()

namespace citius {

namespace {

// Teto para linha de requisicao + cabecalhos; acima disso respondemos 431. 8 KB e um limite comum (ex.: nginx)
constexpr std::size_t kMaxRequestHeadBytes = 8192;

std::string_view method_name(HttpMethod method) {
    switch (method) {
        case HttpMethod::GET: return "GET";
        case HttpMethod::POST: return "POST";
        case HttpMethod::PUT: return "PUT";
        case HttpMethod::DELETE: return "DELETE";
        case HttpMethod::UNKNOWN: break;
    }
    return "?";
}

// Decide a resposta para o que foi lido do cliente; std::nullopt = nao ha a quem responder.
std::optional<HttpResponse> build_response(const ReadResult& head) {
    switch (head.status) {
        case ReadStatus::TOO_LARGE:
            std::cout << "(cabecalhos grandes demais) -> 431\n";
            return HttpResponse::make_error(StatusCode::HEADERS_TOO_LARGE);
        case ReadStatus::CLOSED:
        case ReadStatus::ERROR:
            std::cout << "(conexao encerrada antes de completar a requisicao)\n";
            return std::nullopt;
        case ReadStatus::COMPLETE:
            break;
    }

    ParseResult parsed = HttpParser::parse(head.data);
    if (!parsed.ok()) {
        StatusCode status = to_status(*parsed.error);
        std::cout << std::format("(requisicao invalida) -> {}\n", static_cast<int>(status));
        return HttpResponse::make_error(status);
    }

    // TODO(Fases 2 e 3): servir arquivos de www/ e rotear; por enquanto a resposta e fixa
    HttpResponse response{StatusCode::OK};
    response.set_body("Hello Citius!");
    // fechamos a conexao apos cada resposta; a RFC pede que o servidor avise
    (void)response.set_header("Connection", "close");

    std::cout << std::format("{} {} -> 200\n", method_name(parsed.request->method), parsed.request->path);
    return response;
}

} // namespace

Server::Server(std::uint16_t port) {
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

    std::cout << std::format("Servidor escutando na porta {}...\n", m_port);
}

void Server::serve_one() {
    int fd;
    do {
        // nullptr, nullptr: por enquanto nao queremos saber o endereco do cliente
        fd = accept(m_server_socket.get(), nullptr, nullptr);
    } while(fd < 0 && errno == EINTR);  // um sinal interrompeu a espera: tenta de novo

    Socket client{fd};
    if(!client.valid()) {
        std::cerr << "Falha ao aceitar conexao.\n";
        return;
    }

    // SO_NOSIGPIPE e por socket: precisa ser definido em cada cliente aceito
    if(!client.set_no_sigpipe()) {
        std::cerr << "Falha ao definir SO_NOSIGPIPE no cliente.\n";
        return;
    }

    std::optional<HttpResponse> response = build_response(read_request_head(client, kMaxRequestHeadBytes));
    if(response && !client.send_all(response->serialize())) {
        std::cerr << "Falha ao enviar resposta.\n";
    }
    // sem close(): o destrutor de 'client' fecha o fd ao sair da funcao
}

void Server::run() {
    while(true) {
        serve_one();
    }
}

} // namespace citius
