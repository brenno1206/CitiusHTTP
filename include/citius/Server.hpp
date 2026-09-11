#include <cstdint>

class Server {
private:
int m_server_fd = -1;  // SOCKET
void start(uint16_t port);

public:
    explicit Server(uint16_t port);
    ~Server();
    
};