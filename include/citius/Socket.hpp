#pragma once

namespace citius {

// Dono unico de um file descriptor: fecha no destrutor. Move-only.
class Socket {
public:
    Socket() noexcept = default;
    explicit Socket(int fd) noexcept : m_fd(fd) {}
    ~Socket();

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;

    [[nodiscard]] int get() const noexcept { return m_fd; }
    [[nodiscard]] bool valid() const noexcept { return m_fd >= 0; }

    // Abre mao da posse: devolve o fd sem fechar. Quem recebe passa a ser o dono.
    [[nodiscard]] int release() noexcept;

    // Fecha o fd atual (se houver) e passa a guardar new_fd.
    void reset(int new_fd = -1) noexcept;

private:
    int m_fd = -1;
};

} // namespace citius
