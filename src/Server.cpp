#include "citius/Server.hpp"
#include <sys/socket.h> // Para socket(), bind(), listen()
#include <netinet/in.h> // Para struct sockaddr_in, htons(), htonl()
#include <unistd.h>     // Para close()
#include <stdexcept>    // Para lançar std::runtime_error
#include <format>
#include <iostream>

Server::Server(uint16_t port) {
    start(port);
}
Server::~Server() {
    if(m_server_fd >= 0) {
        close(m_server_fd);
    }
}
void Server::start(uint16_t port) {

    m_server_fd = socket(AF_INET, SOCK_STREAM, 0);

    if(m_server_fd < 0) {
        throw std::runtime_error("Erro na criacao do socket\n");
    }

    sockaddr_in address{
    .sin_family = AF_INET,  // IPv4 - ip classico
    .sin_port = htons(port), // escolhe a porta
    .sin_addr = { .s_addr = INADDR_ANY } // aceita todos os enderecos de ip da maquina
    };
    auto bindResponse = bind(m_server_fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address));

    if(bindResponse < 0) {
        throw std::runtime_error("Erro no bind.\n");
    }

    if(listen(m_server_fd, 10) < 0) {
        throw std::runtime_error("Falha no listen.\n");
    }

    std::cout << std::format("Servidor escutando na porta {}...\n", port);


}