#pragma once
#include <span>
#include <string_view>
#include <sys/types.h> // ssize_t

namespace citius {

// Dono unico de um file descriptor: fecha no destrutor. Move-only.
class Socket {
public:
    Socket() noexcept = default; // Construtor default: fd invalido.
    explicit Socket(int fd) noexcept : m_fd(fd) {} // Construtor a partir de um fd existente.
    ~Socket();

    Socket(const Socket&) = delete; // Desabilita copia.
    Socket& operator=(const Socket&) = delete; // Desabilita copia.
    Socket(Socket&& other) noexcept; // transfere a posse do fd de other para this.
    Socket& operator=(Socket&& other) noexcept; // transfere a posse do fd de other para this.

    // nodiscard: indica que o valor retornado nao deve ser ignorado.
    [[nodiscard]] int get() const noexcept { return m_fd; }
    [[nodiscard]] bool valid() const noexcept { return m_fd >= 0; }

    // Abre mao da posse: devolve o fd sem fechar. Quem recebe passa a ser o dono.
    [[nodiscard]] int release() noexcept;

    // Fecha o fd atual (se houver) e passa a guardar new_fd.
    void reset(int new_fd = -1) noexcept;

    // Permite reusar a porta logo apos reiniciar (evita "Address already in use").
    [[nodiscard]] bool set_reuse_addr() noexcept;

    // Impede que send() em conexao fechada mate o processo com SIGPIPE.
    // So tem efeito no macOS (SO_NOSIGPIPE); no Linux usa-se MSG_NOSIGNAL no send.
    [[nodiscard]] bool set_no_sigpipe() noexcept;

    /// Envia todos os bytes de @p data, repetindo send() em envios parciais e em EINTR.
    /// @return false se a conexao falhou (ex.: o cliente fechou) antes de enviar tudo.
    [[nodiscard]] bool send_all(std::string_view data) noexcept;

    /// Le ate buffer.size() bytes com um unico recv() (repete apenas em EINTR).
    /// @return bytes lidos; 0 se o par fechou a conexao; -1 em erro.
    [[nodiscard]] ssize_t recv_some(std::span<char> buffer) noexcept;

private:
    int m_fd = -1;
};

} // namespace citius
