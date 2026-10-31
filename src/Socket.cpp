#include "citius/Socket.hpp"
#include <unistd.h> // close()
#include <utility>

namespace citius {

Socket::~Socket() {
    reset();
}

Socket::Socket(Socket&& other) noexcept : m_fd(other.release()) {}

Socket& Socket::operator=(Socket&& other) noexcept {
    // se for auto-atribuicao, release() esvazia other (== *this) e reset() nao fecha nada
    if (this != &other) {
        reset(other.release());
    }
    return *this;
}

int Socket::release() noexcept {
    return std::exchange(m_fd, -1);
}

void Socket::reset(int new_fd) noexcept {
    if (m_fd >= 0) {
        ::close(m_fd);
    }
    m_fd = new_fd;
}

} // namespace citius
