#include "citius/net/Socket.hpp"
#include <array>
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <type_traits>
#include <unistd.h>
#include <utility>

using citius::Socket;

namespace {

// fcntl(F_GETFD) falha com EBADF quando o fd nao esta aberto
bool fd_aberto(int fd) {
    return fcntl(fd, F_GETFD) != -1;
}

int novo_fd() {
    return ::socket(AF_INET, SOCK_STREAM, 0);
}

} // namespace

static_assert(!std::is_copy_constructible_v<Socket>);
static_assert(!std::is_copy_assignable_v<Socket>);
static_assert(std::is_nothrow_move_constructible_v<Socket>);

TEST(SocketTest, PadraoNaoTemFd) {
    Socket socket;

    EXPECT_FALSE(socket.valid());
    EXPECT_EQ(socket.get(), -1);
}

TEST(SocketTest, DestrutorFechaOFd) {
    int fd = novo_fd();
    ASSERT_GE(fd, 0);
    {
        Socket socket{fd};
        EXPECT_TRUE(socket.valid());
        EXPECT_EQ(socket.get(), fd);
        EXPECT_TRUE(fd_aberto(fd));
    }
    EXPECT_FALSE(fd_aberto(fd));
}

TEST(SocketTest, MoveTransfereAPosse) {
    int fd = novo_fd();
    ASSERT_GE(fd, 0);

    Socket origem{fd};
    Socket destino{std::move(origem)};

    EXPECT_FALSE(origem.valid());
    EXPECT_EQ(destino.get(), fd);
    EXPECT_TRUE(fd_aberto(fd));
}

TEST(SocketTest, AtribuicaoPorMoveFechaOFdAnterior) {
    int fd_a = novo_fd();
    int fd_b = novo_fd();
    ASSERT_GE(fd_a, 0);
    ASSERT_GE(fd_b, 0);

    Socket a{fd_a};
    Socket b{fd_b};
    a = std::move(b);

    EXPECT_FALSE(fd_aberto(fd_a));
    EXPECT_EQ(a.get(), fd_b);
    EXPECT_FALSE(b.valid());
    EXPECT_TRUE(fd_aberto(fd_b));
}

TEST(SocketTest, ReleaseEntregaOFdSemFechar) {
    int fd = novo_fd();
    ASSERT_GE(fd, 0);

    int solto;
    {
        Socket socket{fd};
        solto = socket.release();
        EXPECT_FALSE(socket.valid());
    }
    EXPECT_EQ(solto, fd);
    EXPECT_TRUE(fd_aberto(fd));
    ::close(fd);
}

TEST(SocketTest, ResetFechaOFdAtual) {
    int fd = novo_fd();
    ASSERT_GE(fd, 0);

    Socket socket{fd};
    socket.reset();

    EXPECT_FALSE(socket.valid());
    EXPECT_FALSE(fd_aberto(fd));
}

namespace {

int opcao(int fd, int nivel, int nome) {
    int valor = 0;
    socklen_t tamanho = sizeof(valor);
    EXPECT_EQ(getsockopt(fd, nivel, nome, &valor, &tamanho), 0);
    return valor;
}

} // namespace

TEST(SocketTest, SetReuseAddrLigaAOpcao) {
    Socket socket{novo_fd()};
    ASSERT_TRUE(socket.valid());
    ASSERT_EQ(opcao(socket.get(), SOL_SOCKET, SO_REUSEADDR), 0);

    EXPECT_TRUE(socket.set_reuse_addr());
    EXPECT_NE(opcao(socket.get(), SOL_SOCKET, SO_REUSEADDR), 0);
}

TEST(SocketTest, SetNoSigpipeLigaAOpcaoNoMacOS) {
    Socket socket{novo_fd()};
    ASSERT_TRUE(socket.valid());

    EXPECT_TRUE(socket.set_no_sigpipe());
#ifdef SO_NOSIGPIPE
    EXPECT_NE(opcao(socket.get(), SOL_SOCKET, SO_NOSIGPIPE), 0);
#endif
}

TEST(SocketTest, OpcoesFalhamEmSocketInvalido) {
    Socket socket;

    EXPECT_FALSE(socket.set_reuse_addr());
#ifdef SO_NOSIGPIPE
    EXPECT_FALSE(socket.set_no_sigpipe());
#endif
}

TEST(SocketTest, SendAllEnviaTudoMesmoPayloadGrande) {
    int fds[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    Socket emissor{fds[0]};
    Socket receptor{fds[1]};
    ASSERT_TRUE(emissor.set_no_sigpipe());

    // maior que o buffer do kernel: obriga send() a enviar em partes
    const std::string payload(4 * 1024 * 1024, 'x');
    std::string recebido;
    std::thread leitor([&] {
        std::array<char, 65536> buffer;
        ssize_t n;
        while ((n = receptor.recv_some(buffer)) > 0) {
            recebido.append(buffer.data(), static_cast<std::size_t>(n));
        }
    });

    EXPECT_TRUE(emissor.send_all(payload));
    emissor.reset(); // fecha para o leitor ver o fim
    leitor.join();

    EXPECT_EQ(recebido.size(), payload.size());
    EXPECT_EQ(recebido, payload);
}

TEST(SocketTest, SendAllFalhaSemMatarOProcessoQuandoOParFechou) {
    int fds[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    Socket emissor{fds[0]};
    Socket receptor{fds[1]};
    ASSERT_TRUE(emissor.set_no_sigpipe());
    receptor.reset();

    EXPECT_FALSE(emissor.send_all("ola")); // sem SIGPIPE ele derrubaria o teste
}

TEST(SocketTest, RecvSomeDevolveZeroQuandoOParFechou) {
    int fds[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    Socket emissor{fds[0]};
    Socket receptor{fds[1]};
    emissor.reset();

    std::array<char, 16> buffer;
    EXPECT_EQ(receptor.recv_some(buffer), 0);
}

TEST(SocketTest, RecvSomeDevolveErroEmSocketInvalido) {
    Socket socket;
    std::array<char, 16> buffer;

    EXPECT_EQ(socket.recv_some(buffer), -1);
    EXPECT_FALSE(socket.send_all("x"));
}

TEST(SocketTest, RecvTimeoutFazRecvSomeFalharComEagain) {
    int fds[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    Socket emissor{fds[0]}; // fica aberto e mudo
    Socket receptor{fds[1]};
    ASSERT_TRUE(receptor.set_recv_timeout(std::chrono::milliseconds(50)));

    std::array<char, 16> buffer;
    auto inicio = std::chrono::steady_clock::now();
    EXPECT_EQ(receptor.recv_some(buffer), -1);
    EXPECT_TRUE(errno == EAGAIN || errno == EWOULDBLOCK);
    EXPECT_GE(std::chrono::steady_clock::now() - inicio, std::chrono::milliseconds(40));
}

TEST(SocketTest, SetRecvTimeoutFalhaEmSocketInvalido) {
    Socket socket;

    EXPECT_FALSE(socket.set_recv_timeout(std::chrono::milliseconds(50)));
}

TEST(SocketTest, SetNonblockingFazRecvSomeRetornarEagainSemEsperar) {
    int fds[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    Socket emissor{fds[0]}; // aberto e mudo: um recv bloqueante ficaria parado
    Socket receptor{fds[1]};
    ASSERT_TRUE(receptor.set_nonblocking());

    std::array<char, 16> buffer;
    auto inicio = std::chrono::steady_clock::now();
    EXPECT_EQ(receptor.recv_some(buffer), -1);
    EXPECT_TRUE(errno == EAGAIN || errno == EWOULDBLOCK);
    EXPECT_LT(std::chrono::steady_clock::now() - inicio, std::chrono::milliseconds(40));
}

TEST(SocketTest, SetNonblockingFalhaEmSocketInvalido) {
    Socket socket;

    EXPECT_FALSE(socket.set_nonblocking());
}

TEST(SocketTest, SendSomeEnviaOsBytesEDevolveQuantosForam) {
    int fds[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    Socket emissor{fds[0]};
    Socket receptor{fds[1]};
    ASSERT_TRUE(emissor.set_no_sigpipe());

    EXPECT_EQ(emissor.send_some("ola"), 3);

    std::array<char, 16> buffer;
    ASSERT_EQ(receptor.recv_some(buffer), 3);
    EXPECT_EQ(std::string(buffer.data(), 3), "ola");
}

TEST(SocketTest, SendSomeNaoBloqueanteEnviaParteEDepoisDaEagain) {
    int fds[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    Socket emissor{fds[0]};
    Socket receptor{fds[1]};
    ASSERT_TRUE(emissor.set_no_sigpipe());
    ASSERT_TRUE(emissor.set_nonblocking());

    // ninguem le: o buffer do kernel enche e o send_some tem de parar sozinho, sem bloquear
    const std::string bloco(64 * 1024, 'x');
    std::size_t total = 0;
    ssize_t n;
    while ((n = emissor.send_some(bloco)) > 0) {
        total += static_cast<std::size_t>(n);
        ASSERT_LT(total, 64u * 1024 * 1024) << "o buffer nunca encheu?";
    }
    EXPECT_GT(total, 0u);
    EXPECT_EQ(n, -1);
    EXPECT_TRUE(errno == EAGAIN || errno == EWOULDBLOCK);

    // o receptor esvazia o buffer: volta a caber
    std::array<char, 65536> buffer;
    ASSERT_GT(receptor.recv_some(buffer), 0);
    EXPECT_GT(emissor.send_some("x"), 0);
}

TEST(SocketTest, SendSomeFalhaSemMatarOProcessoQuandoOParFechou) {
    int fds[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    Socket emissor{fds[0]};
    Socket receptor{fds[1]};
    ASSERT_TRUE(emissor.set_no_sigpipe());
    receptor.reset();

    EXPECT_EQ(emissor.send_some("ola"), -1);
}

TEST(SocketTest, SendSomeDevolveErroEmSocketInvalido) {
    Socket socket;

    EXPECT_EQ(socket.send_some("x"), -1);
}

TEST(SocketTest, ShutdownWriteEnviaOFimMasAindaRecebe) {
    int fds[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    Socket a{fds[0]};
    Socket b{fds[1]};

    ASSERT_TRUE(a.send_all("resposta"));
    ASSERT_TRUE(a.shutdown_write());

    // b le os dados e depois ve o fim da conexao (FIN), mas a ainda esta aberto para receber
    std::array<char, 16> buffer;
    ASSERT_EQ(b.recv_some(buffer), 8);
    EXPECT_EQ(b.recv_some(buffer), 0);
    ASSERT_TRUE(b.send_all("ok"));
    EXPECT_EQ(a.recv_some(buffer), 2);
}

TEST(SocketTest, ShutdownWriteFalhaEmSocketInvalido) {
    Socket socket;

    EXPECT_FALSE(socket.shutdown_write());
}
