#pragma once
#include "citius/net/Socket.hpp"
#include <chrono>
#include <vector>

namespace citius {

/// O que se quer saber sobre um fd.
enum class Interest {
    READ,      ///< ha dados para ler (ou o par fechou)
    WRITE,     ///< cabe escrever sem bloquear
    READ_WRITE ///< os dois
};

/// Um fd que ficou pronto. Um fd aparece no maximo uma vez por wait().
struct PollEvent {
    int fd = -1;
    bool readable = false; ///< recv() nao bloqueia (inclusive quando devolveria 0 = conexao fechada)
    bool writable = false; ///< send() nao bloqueia
    bool closed = false;   ///< o par fechou ou houve erro; ainda pode haver dados a ler antes do recv devolver 0
};

/// "Quem esta pronto?": espera varios fds de uma vez, em vez de uma thread bloqueada por conexao.
/// Usa kqueue no macOS e epoll no Linux (a escolha e feita pelo CMake; o resto do codigo nao ve a diferenca).
///
/// Dispara por nivel (level-triggered): enquanto houver dados nao lidos, todo wait() avisa de novo.
/// Nao toma posse dos fds registrados: quem os cria os fecha (o SO ja os tira do Poller ao fechar,
/// mas chame remove() antes para deixar a intencao clara). Move-only. Nao e thread-safe.
class Poller {
public:
    /// @throws std::runtime_error se o SO nao criar o objeto (ex.: limite de fds do processo).
    Poller();

    Poller(Poller&&) noexcept = default;
    Poller& operator=(Poller&&) noexcept = default;
    Poller(const Poller&) = delete;
    Poller& operator=(const Poller&) = delete;
    ~Poller() = default; // m_handle fecha o fd do kqueue/epoll

    /// Passa a vigiar @p fd.
    /// @return false se o SO recusar (fd invalido, ou ja registrado no Linux).
    [[nodiscard]] bool add(int fd, Interest interest) noexcept;

    /// Troca o que se quer saber sobre um fd ja registrado.
    /// @return false se o SO recusar (fd invalido ou nao registrado).
    [[nodiscard]] bool modify(int fd, Interest interest) noexcept;

    /// Para de vigiar @p fd. @return false se o SO recusar (ex.: nunca foi registrado).
    [[nodiscard]] bool remove(int fd) noexcept;

    /// Espera ate @p timeout por fds prontos; zero consulta sem bloquear.
    /// @return os fds prontos (vazio se o prazo acabou ou um sinal interrompeu a espera: chame de novo).
    /// @throws std::runtime_error em falha do SO que nao seja interrupcao por sinal (bug ou fd do Poller fechado).
    [[nodiscard]] std::vector<PollEvent> wait(std::chrono::milliseconds timeout);

private:
    Socket m_handle; ///< dono do fd do kqueue/epoll (Socket aqui so significa "dono de um fd": fecha no destrutor)
};

} // namespace citius
