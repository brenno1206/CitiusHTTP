#include "citius/Socket.hpp"
#include <cerrno>
#include <sys/socket.h> // setsockopt(), send(), recv()
#include <unistd.h>     // close()
#include <utility>

namespace citius {

Socket::~Socket() {
    reset();
}

Socket::Socket(Socket&& other) noexcept : m_fd(other.release()) {}

Socket& Socket::operator=(Socket&& other) noexcept {
    // sem este teste, "a = std::move(a)" fecharia o proprio fd
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

bool Socket::send_all(std::string_view data) noexcept {
#ifdef MSG_NOSIGNAL
    constexpr int kFlags = MSG_NOSIGNAL;
#else
    constexpr int kFlags = 0; // macOS: SO_NOSIGPIPE (set_no_sigpipe) faz esse papel
#endif
    while (!data.empty()) {
        ssize_t sent = ::send(m_fd, data.data(), data.size(), kFlags);
        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        data.remove_prefix(static_cast<std::size_t>(sent));
    }
    return true;
}

ssize_t Socket::recv_some(std::span<char> buffer) noexcept {
    while (true) {
        ssize_t received = ::recv(m_fd, buffer.data(), buffer.size(), 0);
        if (received < 0 && errno == EINTR) {
            continue;
        }
        return received;
    }
}

} // namespace citius
