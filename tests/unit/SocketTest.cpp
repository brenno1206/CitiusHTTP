#include "citius/Socket.hpp"
#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/socket.h>
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
