#include "citius/Poller.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <stdexcept>
#include <sys/event.h>
#include <sys/time.h>

namespace citius {

namespace {

constexpr std::size_t kMaxEventsPerWait = 256;

// No kqueue cada filtro (leitura, escrita) e registrado a parte. Registramos sempre os dois e
// ligamos/desligamos (EV_ENABLE/EV_DISABLE): assim add() e modify() sao a mesma operacao e nao
// precisamos lembrar o que cada fd tinha antes (EV_DELETE de um filtro ausente daria ENOENT).
bool apply(int kq, int fd, Interest interest) noexcept {
    const bool want_read = interest != Interest::WRITE;
    const bool want_write = interest != Interest::READ;
    std::array<struct kevent, 2> changes;
    EV_SET(&changes[0], static_cast<std::uintptr_t>(fd), EVFILT_READ, EV_ADD | (want_read ? EV_ENABLE : EV_DISABLE), 0,
           0, nullptr);
    EV_SET(&changes[1], static_cast<std::uintptr_t>(fd), EVFILT_WRITE, EV_ADD | (want_write ? EV_ENABLE : EV_DISABLE),
           0, 0, nullptr);
    return fd >= 0 && kevent(kq, changes.data(), static_cast<int>(changes.size()), nullptr, 0, nullptr) == 0;
}

} // namespace

Poller::Poller() : m_handle(kqueue()) {
    if (!m_handle.valid()) {
        throw std::runtime_error("Falha ao criar o kqueue.");
    }
}

bool Poller::add(int fd, Interest interest) noexcept {
    return apply(m_handle.get(), fd, interest);
}

bool Poller::modify(int fd, Interest interest) noexcept {
    return apply(m_handle.get(), fd, interest);
}

bool Poller::remove(int fd) noexcept {
    std::array<struct kevent, 2> changes;
    EV_SET(&changes[0], static_cast<std::uintptr_t>(fd), EVFILT_READ, EV_DELETE, 0, 0, nullptr);
    EV_SET(&changes[1], static_cast<std::uintptr_t>(fd), EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
    return fd >= 0 &&
           kevent(m_handle.get(), changes.data(), static_cast<int>(changes.size()), nullptr, 0, nullptr) == 0;
}

std::vector<PollEvent> Poller::wait(std::chrono::milliseconds timeout) {
    std::array<struct kevent, kMaxEventsPerWait> raw;
    const auto secs = std::chrono::duration_cast<std::chrono::seconds>(timeout);
    const timespec ts{
        .tv_sec = static_cast<time_t>(secs.count()),
        .tv_nsec = static_cast<long>(std::chrono::duration_cast<std::chrono::nanoseconds>(timeout - secs).count())};

    const int n = kevent(m_handle.get(), nullptr, 0, raw.data(), static_cast<int>(raw.size()), &ts);
    if (n < 0) {
        if (errno == EINTR) return {};
        throw std::runtime_error("Falha no kevent.");
    }

    std::vector<PollEvent> events;
    events.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        const struct kevent& ev = raw[static_cast<std::size_t>(i)];
        PollEvent e;
        e.fd = static_cast<int>(ev.ident);
        e.readable = ev.filter == EVFILT_READ;
        e.writable = ev.filter == EVFILT_WRITE;
        // EV_EOF: o par fechou (no filtro de leitura ainda pode haver dados; o recv devolve 0 no fim)
        e.closed = (ev.flags & (EV_EOF | EV_ERROR)) != 0;
        events.push_back(e);
    }

    // o mesmo fd pode vir em dois eventos (um por filtro): junta, para o chamador ver um so
    std::ranges::sort(events, {}, &PollEvent::fd);
    std::vector<PollEvent> merged;
    merged.reserve(events.size());
    for (const PollEvent& e : events) {
        if (!merged.empty() && merged.back().fd == e.fd) {
            merged.back().readable |= e.readable;
            merged.back().writable |= e.writable;
            merged.back().closed |= e.closed;
        } else {
            merged.push_back(e);
        }
    }
    return merged;
}

} // namespace citius
