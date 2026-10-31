#include "citius/Server.hpp"
#include <sys/socket.h> // Para socket(), bind(), listen()
#include <netinet/in.h> // Para struct sockaddr_in, htons(), htonl()
#include <unistd.h>     // Para close()
#include <stdexcept>    // Para lançar std::runtime_error
#include <format>
#include <iostream>
#include <array>
#include <string>

namespace citius {

Server::Server(std::uint16_t port) {
    start(port);
}
void Server::start(std::uint16_t port) {

    m_server_socket.reset(socket(AF_INET, SOCK_STREAM, 0));

    if(!m_server_socket.valid()) {
        throw std::runtime_error("Erro na criacao do socket\n");
    }

    sockaddr_in address{
    .sin_family = AF_INET,  // IPv4 - ip classico
    .sin_port = htons(port), // escolhe a porta
    .sin_addr = { .s_addr = INADDR_ANY } // aceita todos os enderecos de ip da maquina
    };
    // bind associa o socket a um endereco e porta
    auto bindResponse = bind(m_server_socket.get(),reinterpret_cast<const sockaddr*>(&address), sizeof(address));

    if(bindResponse < 0) {
        throw std::runtime_error("Erro no bind.\n");
    }

    // listen coloca o socket em modo de escuta
    // 10 é o tamanho da fila de conexoes pendentes
    if(listen(m_server_socket.get(), 10) < 0) {
        throw std::runtime_error("Falha no listen.\n");
    }

    std::cout << std::format("Servidor escutando na porta {}...\n", port);
}

void Server::run(){
    while(true) {
        // Os 'nullptr' significam que, por enquanto, nao queremos saber o IP do cliente
        Socket client{accept(m_server_socket.get(), nullptr, nullptr)};

        if(!client.valid()) {
            std::cerr <<  "Falha ao aceitar conexao.\n";
            continue;
        }

        std::cout << "\n--- Novo Cliente Conectado! ---\n";
        
        // cria uma array de 4KB na stack
        std::array<char, 4096> buffer{}; // o {} garante que ele seja preenchido com zeros

        // recv pede: o socket do cliente, onde salvar os dados, o tamanho máximo, e flags
        ssize_t bytes_lidos = recv(client.get(), buffer.data(), buffer.size(), 0);
        
        if (bytes_lidos > 0) {
            std::cout << std::format("Requisicao recebida:\n{}\n", buffer.data());
        }

        std::string response = "HTTP/1.1 200 OK\r\nContent-Length: 13\r\n\r\nHello Citius!";

        // send pede: o socket do cliente, o ponteiro para os dados brutos da string, o tamanho, e flags
        send(client.get(), response.c_str(), response.size(), 0);

        // sem close(): o destrutor de 'client' fecha o fd ao fim de cada volta do laço
        std::cout << "Conexao encerrada.\n";
    }
}

} // namespace citius
