#include "citius/net/Listener.hpp"
#include <arpa/inet.h> // IWYU pragma: keep (htons, ntohs)
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <format>
#include <netinet/in.h>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/types.h> // IWYU pragma: keep (socklen_t no macOS)

namespace citius {

namespace {

// Uma falha de inicializacao vira excecao (ver ADR 0002) com o passo e o motivo do sistema:
// "bind: Address already in use" ajuda mais que "Erro no bind".
[[noreturn]] void fail(const char* step) {
    throw std::runtime_error(std::format("Falha no {}: {}", step, std::strerror(errno)));
}

} // namespace

Listener::Listener(std::uint16_t port) {
    m_socket.reset(::socket(AF_INET, SOCK_STREAM, 0));
    if (!m_socket.valid()) fail("socket");

    // precisa vir antes do bind, senao reiniciar o servidor falha com "Address already in use"
    if (!m_socket.set_reuse_addr()) fail("SO_REUSEADDR");

    sockaddr_in address{
        .sin_family = AF_INET,              // IPv4
        .sin_port = htons(port),            // htons: ordem de bytes da rede (big-endian)
        .sin_addr = {.s_addr = INADDR_ANY}, // aceita conexoes em qualquer interface da maquina
        .sin_zero = {}                      // preenchimento: o Linux o declara e -Wmissing-field-initializers reclama
    };
    if (::bind(m_socket.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0) fail("bind");

    // fila de conexoes aguardando accept(). Com 10, uma rajada de ~30 clientes perdia conexoes no macOS
    // (o cliente via a conexao cair sem resposta); SOMAXCONN pede o maximo que o SO permite
    if (::listen(m_socket.get(), SOMAXCONN) < 0) fail("listen");

    if (!m_socket.set_nonblocking()) fail("O_NONBLOCK");

    // com porta 0 so agora o sistema sabe qual porta escolheu
    sockaddr_in bound{};
    socklen_t bound_size = sizeof(bound); // NOLINT(misc-include-cleaner): socklen_t vem de <sys/socket.h>
    if (::getsockname(m_socket.get(), reinterpret_cast<sockaddr*>(&bound), &bound_size) < 0) fail("getsockname");
    m_port = ntohs(bound.sin_port);
}

AcceptResult Listener::accept() noexcept {
    // nullptr, nullptr: por enquanto nao queremos saber o endereco do cliente
    const int fd = ::accept(m_socket.get(), nullptr, nullptr);
    if (fd >= 0) return {Socket{fd}, AcceptStatus::ACCEPTED};

    switch (errno) {
    case EAGAIN: // EWOULDBLOCK tem o mesmo valor nos sistemas que usamos
        return {{}, AcceptStatus::EMPTY};
    case EINTR:
    case ECONNABORTED:
        return {{}, AcceptStatus::RETRY};
    case EMFILE:
    case ENFILE:
    case ENOBUFS:
    case ENOMEM:
        return {{}, AcceptStatus::EXHAUSTED};
    default:
        return {{}, AcceptStatus::FAILED};
    }
}

} // namespace citius
