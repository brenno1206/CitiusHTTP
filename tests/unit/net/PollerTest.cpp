#include "citius/net/Poller.hpp"
#include "citius/net/Socket.hpp"
#include <chrono>
#include <gtest/gtest.h>
#include <string_view>
#include <sys/socket.h>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <vector>

using citius::Interest;
using citius::Poller;
using citius::PollEvent;
using citius::Socket;
using namespace std::chrono_literals;

static_assert(!std::is_copy_constructible_v<Poller>);
static_assert(!std::is_copy_assignable_v<Poller>);
static_assert(std::is_nothrow_move_constructible_v<Poller>);

namespace {

// Dois sockets conectados entre si, sem rede: o que um escreve o outro le.
struct SocketPair {
    Socket a;
    Socket b;

    SocketPair() {
        int fds[2];
        EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
        a.reset(fds[0]);
        b.reset(fds[1]);
    }
};

} // namespace

TEST(PollerTest, SemNadaPronto_RetornaVazioSemBloquear) {
    Poller poller;
    SocketPair pair;
    ASSERT_TRUE(poller.add(pair.a.get(), Interest::READ));

    EXPECT_TRUE(poller.wait(0ms).empty());
}

TEST(PollerTest, EsperaOPrazoQuandoNaoHaEventos) {
    Poller poller;
    SocketPair pair;
    ASSERT_TRUE(poller.add(pair.a.get(), Interest::READ));

    const auto start = std::chrono::steady_clock::now();
    EXPECT_TRUE(poller.wait(50ms).empty());
    EXPECT_GE(std::chrono::steady_clock::now() - start, 40ms); // folga p/ arredondamento do relogio
}

TEST(PollerTest, AvisaQuandoHaDadosParaLer) {
    Poller poller;
    SocketPair pair;
    ASSERT_TRUE(poller.add(pair.a.get(), Interest::READ));
    ASSERT_TRUE(pair.b.send_all("oi"));

    std::vector<PollEvent> events = poller.wait(1000ms);

    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].fd, pair.a.get());
    EXPECT_TRUE(events[0].readable);
    EXPECT_FALSE(events[0].writable);
    EXPECT_FALSE(events[0].closed);
}

TEST(PollerTest, AvisaQuandoPodeEscrever) {
    Poller poller;
    SocketPair pair;
    ASSERT_TRUE(poller.add(pair.a.get(), Interest::WRITE));

    std::vector<PollEvent> events = poller.wait(1000ms);

    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].fd, pair.a.get());
    EXPECT_TRUE(events[0].writable);
    EXPECT_FALSE(events[0].readable);
}

TEST(PollerTest, LeituraEEscritaNoMesmoFdVemNumEventoSo) {
    Poller poller;
    SocketPair pair;
    ASSERT_TRUE(poller.add(pair.a.get(), Interest::READ_WRITE));
    ASSERT_TRUE(pair.b.send_all("oi"));

    std::vector<PollEvent> events = poller.wait(1000ms);

    ASSERT_EQ(events.size(), 1u); // o kqueue gera dois eventos crus (um por filtro); o Poller junta
    EXPECT_TRUE(events[0].readable);
    EXPECT_TRUE(events[0].writable);
}

TEST(PollerTest, SinalizaQuandoOParFechaAConexao) {
    Poller poller;
    SocketPair pair;
    ASSERT_TRUE(poller.add(pair.a.get(), Interest::READ));
    pair.b.reset(); // fecha a outra ponta

    std::vector<PollEvent> events = poller.wait(1000ms);

    ASSERT_EQ(events.size(), 1u);
    EXPECT_TRUE(events[0].readable); // o proximo recv devolve 0 (fim da conexao)
    EXPECT_TRUE(events[0].closed);
}

TEST(PollerTest, ModifyTrocaOInteresse) {
    Poller poller;
    SocketPair pair;
    ASSERT_TRUE(poller.add(pair.a.get(), Interest::READ));
    ASSERT_TRUE(pair.b.send_all("oi"));
    ASSERT_TRUE(poller.modify(pair.a.get(), Interest::WRITE));

    std::vector<PollEvent> events = poller.wait(1000ms);

    ASSERT_EQ(events.size(), 1u);
    EXPECT_TRUE(events[0].writable);
    EXPECT_FALSE(events[0].readable); // ha dados, mas ninguem pediu para ser avisado deles
}

TEST(PollerTest, RemoveParaDeAvisar) {
    Poller poller;
    SocketPair pair;
    ASSERT_TRUE(poller.add(pair.a.get(), Interest::READ));
    ASSERT_TRUE(poller.remove(pair.a.get()));
    ASSERT_TRUE(pair.b.send_all("oi"));

    EXPECT_TRUE(poller.wait(50ms).empty());
}

TEST(PollerTest, AvisaOnivelEnquantoOsDadosNaoForemLidos) {
    // level-triggered: sem ler, o aviso se repete (e o que o Server vai assumir)
    Poller poller;
    SocketPair pair;
    ASSERT_TRUE(poller.add(pair.a.get(), Interest::READ));
    ASSERT_TRUE(pair.b.send_all("oi"));

    EXPECT_EQ(poller.wait(1000ms).size(), 1u);
    EXPECT_EQ(poller.wait(1000ms).size(), 1u);
}

TEST(PollerTest, VariosFdsRecebemEventosSeparados) {
    Poller poller;
    SocketPair p1;
    SocketPair p2;
    ASSERT_TRUE(poller.add(p1.a.get(), Interest::READ));
    ASSERT_TRUE(poller.add(p2.a.get(), Interest::READ));
    ASSERT_TRUE(p1.b.send_all("1"));
    ASSERT_TRUE(p2.b.send_all("2"));

    EXPECT_EQ(poller.wait(1000ms).size(), 2u);
}

TEST(PollerTest, AddDeFdInvalidoFalha) {
    Poller poller;

    EXPECT_FALSE(poller.add(-1, Interest::READ));
    EXPECT_FALSE(poller.modify(-1, Interest::READ));
}

TEST(PollerTest, MoverTransfereOPoller) {
    Poller original;
    SocketPair pair;
    ASSERT_TRUE(original.add(pair.a.get(), Interest::READ));
    ASSERT_TRUE(pair.b.send_all("oi"));

    Poller movido = std::move(original);

    EXPECT_EQ(movido.wait(1000ms).size(), 1u);
}
