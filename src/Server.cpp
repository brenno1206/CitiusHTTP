#include "citius/Server.hpp"
#include "citius/RequestReader.hpp"
#include <sys/socket.h> // Para socket(), bind(), listen()
#include <netinet/in.h> // Para struct sockaddr_in, htons(), htonl()
#include <stdexcept>    // Para lançar std::runtime_error
#include <format>
#include <iostream>
#include <string>

namespace citius {

namespace {
// Teto para linha de requisicao + cabecalhos; acima disso respondemos 431. 8 KB e um limite comum (ex.: nginx)
constexpr std::size_t kMaxRequestHeadBytes = 8192;
}

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

    std::cout << std::format("Servidor escutando na porta {}...\n", port);
}

void Server::run(){
    while(true) {
        // nullptr, nullptr: por enquanto nao queremos saber o endereco do cliente
        Socket client{accept(m_server_socket.get(), nullptr, nullptr)};

        if(!client.valid()) {
            std::cerr <<  "Falha ao aceitar conexao.\n";
            continue;
        }

        // SO_NOSIGPIPE e por socket: precisa ser definido em cada cliente aceito
        if(!client.set_no_sigpipe()) {
            std::cerr << "Falha ao definir SO_NOSIGPIPE no cliente.\n";
            continue;
        }

        std::cout << "\n--- Novo Cliente Conectado! ---\n";

        ReadResult request = read_request_head(client, kMaxRequestHeadBytes);

        if (request.status == ReadStatus::TOO_LARGE) {
            if (!client.send_all("HTTP/1.1 431 Request Header Fields Too Large\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")) {
                std::cerr << "Falha ao enviar resposta 431.\n";
            }
            continue;
        }
        if (request.status != ReadStatus::COMPLETE) {
            std::cout << "Conexao encerrada antes de completar a requisicao.\n";
            continue;
        }

        std::cout << std::format("Requisicao recebida:\n{}\n", request.data);

        // TODO(passo 6): passar request.data pelo HttpParser e montar a resposta certa
        const std::string response = "HTTP/1.1 200 OK\r\nContent-Length: 13\r\n\r\nHello Citius!";
        if (!client.send_all(response)) {
            std::cerr << "Falha ao enviar resposta.\n";
        }

        // sem close(): o destrutor de 'client' fecha o fd ao fim de cada volta do laço
        std::cout << "Conexao encerrada.\n";
    }
}

} // namespace citius
