#pragma once

#include "citius/Socket.hpp"
#include <cstdint>

namespace citius {

class Server {
private:
Socket m_server_socket;  // dono do fd de escuta; fecha sozinho
void start(std::uint16_t port);

public:
    explicit Server(std::uint16_t port);
    void run();
};

} // namespace citius
