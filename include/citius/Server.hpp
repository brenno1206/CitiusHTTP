#pragma once

#include <cstdint>

namespace citius {

class Server {
private:
int m_server_fd = -1;  // SOCKET
void start(std::uint16_t port);

public:
    explicit Server(std::uint16_t port);
    ~Server();
    void run();
};

} // namespace citius
