#include "citius/net/Socket.hpp"
#include <cerrno>
#include <fcntl.h>      // fcntl(), O_NONBLOCK
#include <sys/socket.h> // setsockopt(), send(), recv()
#include <sys/time.h>   // struct timeval
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

bool Socket::set_recv_timeout(std::chrono::milliseconds timeout) noexcept {
    // timeval zerado desliga o timeout no SO_RCVTIMEO; por isso recusamos prazo <= 0
    if (timeout <= std::chrono::milliseconds::zero()) {
        return false;
    }
    auto const secs = std::chrono::duration_cast<std::chrono::seconds>(timeout);
    auto const usecs = std::chrono::duration_cast<std::chrono::microseconds>(timeout - secs);
    timeval tv{.tv_sec = static_cast<time_t>(secs.count()), .tv_usec = static_cast<suseconds_t>(usecs.count())};
    return ::setsockopt(m_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0;
}

bool Socket::set_nonblocking() noexcept {
    const int flags = ::fcntl(m_fd, F_GETFL, 0);
    return flags >= 0 && ::fcntl(m_fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

bool Socket::send_all(std::string_view data) noexcept {
    while (!data.empty()) {
        ssize_t const sent = send_some(data);
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

ssize_t Socket::send_some(std::string_view data) noexcept {
#ifdef MSG_NOSIGNAL
    constexpr int kFlags = MSG_NOSIGNAL;
#else
    constexpr int kFlags = 0; // macOS: SO_NOSIGPIPE (set_no_sigpipe) faz esse papel
#endif
    // sem laco de EINTR aqui: send_all trata o EINTR e quem usa send_some direto decide o que fazer
    return ::send(m_fd, data.data(), data.size(), kFlags);
}

ssize_t Socket::recv_some(std::span<char> buffer) noexcept {
    while (true) {
        ssize_t const received = ::recv(m_fd, buffer.data(), buffer.size(), 0);
        if (received < 0 && errno == EINTR) {
            continue;
        }
        return received;
    }
}

} // namespace citius
