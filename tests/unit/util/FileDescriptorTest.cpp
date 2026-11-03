#include "citius/util/FileDescriptor.hpp"
#include <fcntl.h>
#include <gtest/gtest.h>
#include <type_traits>
#include <unistd.h>
#include <utility>

using citius::FileDescriptor;

namespace {

// fcntl(F_GETFD) falha com EBADF quando o fd nao esta aberto
bool fd_aberto(int fd) {
    return fcntl(fd, F_GETFD) != -1;
}

int novo_fd() {
    return ::open("/dev/null", O_RDONLY);
}

} // namespace

static_assert(!std::is_copy_constructible_v<FileDescriptor>);
static_assert(!std::is_copy_assignable_v<FileDescriptor>);
static_assert(std::is_nothrow_move_constructible_v<FileDescriptor>);
static_assert(std::is_nothrow_move_assignable_v<FileDescriptor>);

TEST(FileDescriptorTest, PadraoNaoTemFd) {
    FileDescriptor fd;

    EXPECT_FALSE(fd.valid());
    EXPECT_EQ(fd.get(), -1);
}

TEST(FileDescriptorTest, DestrutorFechaOFd) {
    const int cru = novo_fd();
    ASSERT_GE(cru, 0);
    {
        FileDescriptor fd{cru};
        EXPECT_TRUE(fd.valid());
        EXPECT_EQ(fd.get(), cru);
        EXPECT_TRUE(fd_aberto(cru));
    }
    EXPECT_FALSE(fd_aberto(cru));
}

TEST(FileDescriptorTest, MoveTransfereAPosse) {
    const int cru = novo_fd();
    ASSERT_GE(cru, 0);

    FileDescriptor origem{cru};
    FileDescriptor destino{std::move(origem)};

    EXPECT_FALSE(origem.valid());
    EXPECT_EQ(destino.get(), cru);
    EXPECT_TRUE(fd_aberto(cru));
}

TEST(FileDescriptorTest, AtribuicaoPorMoveFechaOFdAnterior) {
    const int cru_a = novo_fd();
    const int cru_b = novo_fd();
    ASSERT_GE(cru_a, 0);
    ASSERT_GE(cru_b, 0);

    FileDescriptor a{cru_a};
    FileDescriptor b{cru_b};
    a = std::move(b);

    EXPECT_FALSE(fd_aberto(cru_a));
    EXPECT_EQ(a.get(), cru_b);
    EXPECT_FALSE(b.valid());
    EXPECT_TRUE(fd_aberto(cru_b));
}

TEST(FileDescriptorTest, AutoAtribuicaoPorMoveNaoFechaOFd) {
    const int cru = novo_fd();
    ASSERT_GE(cru, 0);
    FileDescriptor fd{cru};

    FileDescriptor& mesmo = fd; // evita o aviso do compilador de auto-move
    fd = std::move(mesmo);

    EXPECT_TRUE(fd.valid());
    EXPECT_TRUE(fd_aberto(cru));
}

TEST(FileDescriptorTest, ReleaseEntregaOFdSemFechar) {
    const int cru = novo_fd();
    ASSERT_GE(cru, 0);

    int solto;
    {
        FileDescriptor fd{cru};
        solto = fd.release();
        EXPECT_FALSE(fd.valid());
    }
    EXPECT_EQ(solto, cru);
    EXPECT_TRUE(fd_aberto(cru));
    ::close(cru);
}

TEST(FileDescriptorTest, ResetFechaOFdAtualEPassaAGuardarOutro) {
    const int cru_a = novo_fd();
    const int cru_b = novo_fd();
    ASSERT_GE(cru_a, 0);
    ASSERT_GE(cru_b, 0);

    FileDescriptor fd{cru_a};
    fd.reset(cru_b);

    EXPECT_FALSE(fd_aberto(cru_a));
    EXPECT_EQ(fd.get(), cru_b);

    fd.reset();
    EXPECT_FALSE(fd.valid());
    EXPECT_FALSE(fd_aberto(cru_b));
}
