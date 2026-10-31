#include "citius/Socket.hpp"
#include <sys/socket.h> // setsockopt()
#include <unistd.h> // close()
#include <utility>

namespace citius {

Socket::~Socket() {
    reset();
}

// transfere a posse do fd de other para this.
Socket::Socket(Socket&& other) noexcept : m_fd(other.release()) {}

// transfere a posse do fd de other para this.
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

// Fecha o fd atual (se houver) e passa a guardar new_fd.
// tem new_fd default = -1, que fecha o fd atual e deixa o Socket invalido.
void Socket::reset(int new_fd) noexcept {
    if (m_fd >= 0) {
        ::close(m_fd);
    }
    m_fd = new_fd;
}

bool Socket::set_reuse_addr() noexcept {
    int on = 1;
    return ::setsockopt(m_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) == 0;
}

bool Socket::set_no_sigpipe() noexcept {
#ifdef SO_NOSIGPIPE
    int on = 1;
    return ::setsockopt(m_fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on)) == 0;
#else
    return true; // Linux: protegido por MSG_NOSIGNAL em cada send
#endif
}

} // namespace citius
