#include "citius/net/Poller.hpp"
#include <array>
#include <cerrno>
#include <stdexcept>
#include <sys/epoll.h>

namespace citius {

namespace {

constexpr std::size_t kMaxEventsPerWait = 256;

epoll_event make_event(int fd, Interest interest) noexcept {
    epoll_event ev{};
    // EPOLLHUP e EPOLLERR sao sempre reportados, mesmo sem pedir
    ev.events = (interest != Interest::WRITE ? EPOLLIN : 0u) | (interest != Interest::READ ? EPOLLOUT : 0u);
    ev.data.fd = fd;
    return ev;
}

} // namespace

Poller::Poller() : m_handle(epoll_create1(EPOLL_CLOEXEC)) {
    if (!m_handle.valid()) {
        throw std::runtime_error("Falha ao criar o epoll.");
    }
}

bool Poller::add(int fd, Interest interest) noexcept {
    epoll_event ev = make_event(fd, interest);
    return fd >= 0 && epoll_ctl(m_handle.get(), EPOLL_CTL_ADD, fd, &ev) == 0;
}

bool Poller::modify(int fd, Interest interest) noexcept {
    epoll_event ev = make_event(fd, interest);
    return fd >= 0 && epoll_ctl(m_handle.get(), EPOLL_CTL_MOD, fd, &ev) == 0;
}

bool Poller::remove(int fd) noexcept {
    // kernels antigos (< 2.6.9) exigem um ponteiro nao nulo mesmo no DEL; passar um custa nada
    epoll_event ev{};
    return fd >= 0 && epoll_ctl(m_handle.get(), EPOLL_CTL_DEL, fd, &ev) == 0;
}

std::vector<PollEvent> Poller::wait(std::chrono::milliseconds timeout) {
    std::array<epoll_event, kMaxEventsPerWait> raw;
    const int n =
        epoll_wait(m_handle.get(), raw.data(), static_cast<int>(raw.size()), static_cast<int>(timeout.count()));
    if (n < 0) {
        if (errno == EINTR) return {};
        throw std::runtime_error("Falha no epoll_wait.");
    }

    std::vector<PollEvent> events;
    events.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        const epoll_event& ev = raw[static_cast<std::size_t>(i)];
        PollEvent e;
        e.fd = ev.data.fd;
        e.closed = (ev.events & (EPOLLHUP | EPOLLERR)) != 0;
        // no HUP o recv devolve 0: tratamos como "legivel" para o chamador descobrir o fim da conexao
        e.readable = (ev.events & (EPOLLIN | EPOLLHUP | EPOLLERR)) != 0;
        e.writable = (ev.events & EPOLLOUT) != 0;
        events.push_back(e);
    }
    return events; // o epoll ja junta leitura e escrita do mesmo fd num evento so
}

} // namespace citius
