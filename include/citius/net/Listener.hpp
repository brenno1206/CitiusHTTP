#pragma once
#include "citius/net/Socket.hpp"
#include <cstdint>

namespace citius {

/// O que aconteceu numa chamada a Listener::accept().
enum class AcceptStatus {
    ACCEPTED,  ///< AcceptResult::socket tem o cliente
    EMPTY,     ///< nenhum cliente esperando agora (EAGAIN): espere o Poller avisar
    RETRY,     ///< um sinal interrompeu, ou o cliente desistiu antes do accept: tente de novo na hora
    EXHAUSTED, ///< faltam descritores ou memoria (EMFILE, ENFILE, ENOBUFS, ENOMEM): espere antes de tentar de novo
    FAILED     ///< qualquer outro erro
};

struct AcceptResult {
    Socket socket; ///< so valido com ACCEPTED
    AcceptStatus status = AcceptStatus::FAILED;
};

/// Socket de escuta TCP (IPv4, todas as interfaces): socket() + SO_REUSEADDR + bind() + listen(), nao bloqueante.
///
/// Nao bloqueante porque o aviso "ha cliente" do Poller pode ser falso (o cliente desiste entre o aviso e o
/// accept), e um accept bloqueante nesse caso travaria o event loop inteiro ate o proximo cliente.
/// Move-only (o Socket fecha o fd no destrutor).
class Listener {
public:
    /// Abre o socket de escuta em @p port. Com 0 o sistema escolhe uma porta livre (use port()).
    /// @throws std::runtime_error se nao conseguir criar, associar ou escutar (ex.: porta ocupada); a mensagem
    ///         diz qual passo falhou e o motivo do sistema.
    explicit Listener(std::uint16_t port);

    /// @return a porta em que realmente escuta (a escolhida pelo sistema, se foi pedida a 0).
    [[nodiscard]] std::uint16_t port() const noexcept {
        return m_port;
    }

    /// @return o fd, para registrar no Poller. Nao feche nem guarde alem da vida do Listener.
    [[nodiscard]] int fd() const noexcept {
        return m_socket.get();
    }

    /// Tira um cliente da fila de espera, sem bloquear. O socket devolvido ainda precisa ser configurado
    /// (set_nonblocking, set_no_sigpipe): o Listener nao faz isso, a Connection faz.
    [[nodiscard]] AcceptResult accept() noexcept;

private:
    Socket m_socket;
    std::uint16_t m_port = 0;
};

} // namespace citius
