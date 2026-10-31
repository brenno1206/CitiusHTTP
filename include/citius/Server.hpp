#pragma once

#include "citius/Socket.hpp"
#include <cstdint>

namespace citius {

/// Servidor HTTP: escuta numa porta TCP e atende um cliente por vez (por enquanto).
class Server {
public:
    /// Abre o socket de escuta em @p port (IPv4, todas as interfaces).
    /// @throws std::runtime_error se nao conseguir criar, associar ou escutar (ex.: porta ocupada).
    explicit Server(std::uint16_t port);

    /// Aceita e atende clientes em laco; so retorna se o processo for encerrado.
    void run();

private:
    void start(std::uint16_t port);

    Socket m_server_socket; ///< socket de escuta; fecha sozinho no destrutor
};

} // namespace citius
