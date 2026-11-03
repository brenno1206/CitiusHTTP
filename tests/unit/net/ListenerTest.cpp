#include "citius/net/Listener.hpp"
#include "citius/net/Socket.hpp"
#include <arpa/inet.h>
#include <array>
#include <chrono>
#include <cstdint>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <type_traits>
#include <utility>

using citius::AcceptResult;
using citius::AcceptStatus;
using citius::Listener;
using citius::Socket;

static_assert(!std::is_copy_constructible_v<Listener>);
static_assert(!std::is_copy_assignable_v<Listener>);
static_assert(std::is_nothrow_move_constructible_v<Listener>);

namespace {

// Conecta em 127.0.0.1:port. O connect() de um cliente termina assim que o kernel enfileira a conexao,
// sem o servidor ter chamado accept(): por isso da para testar tudo numa thread so.
Socket conectar(std::uint16_t port) {
    Socket socket{::socket(AF_INET, SOCK_STREAM, 0)};
    EXPECT_TRUE(socket.valid());
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    EXPECT_EQ(::connect(socket.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)), 0);
    return socket;
}

// A conexao pode levar um instante para aparecer na fila do accept (loopback e quase imediato, mas nao garantido).
AcceptResult aceitar_com_espera(Listener& listener) {
    AcceptResult result;
    for (int i = 0; i < 200; ++i) {
        result = listener.accept();
        if (result.status != AcceptStatus::EMPTY) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return result;
}

} // namespace

TEST(ListenerTest, PortaZeroEscolheUmaPortaLivre) {
    Listener listener{0};

    EXPECT_GT(listener.port(), 0);
    EXPECT_GE(listener.fd(), 0);
}

TEST(ListenerTest, PortaOcupadaLancaExcecaoComOMotivo) {
    Listener primeiro{0};

    try {
        Listener segundo{primeiro.port()};
        FAIL() << "deveria ter lancado";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("bind"), std::string::npos) << e.what();
    }
}

TEST(ListenerTest, SocketDeEscutaEhNaoBloqueante) {
    Listener listener{0};

    EXPECT_NE(::fcntl(listener.fd(), F_GETFL) & O_NONBLOCK, 0);
}

TEST(ListenerTest, AcceptSemClienteDevolveEmptySemBloquear) {
    Listener listener{0};

    const auto inicio = std::chrono::steady_clock::now();
    AcceptResult result = listener.accept();

    EXPECT_EQ(result.status, AcceptStatus::EMPTY);
    EXPECT_FALSE(result.socket.valid());
    EXPECT_LT(std::chrono::steady_clock::now() - inicio, std::chrono::milliseconds(100));
}

TEST(ListenerTest, AcceptDevolveOClienteQueConectou) {
    Listener listener{0};
    Socket cliente = conectar(listener.port());

    AcceptResult result = aceitar_com_espera(listener);

    ASSERT_EQ(result.status, AcceptStatus::ACCEPTED);
    ASSERT_TRUE(result.socket.valid());

    // os dois lados conversam
    ASSERT_TRUE(cliente.send_all("oi"));
    std::array<char, 16> buffer;
    ssize_t n = -1;
    for (int i = 0; i < 200 && n < 0; ++i) {
        n = result.socket.recv_some(buffer);
        if (n < 0) std::this_thread::sleep_for(std::chrono::milliseconds(5)); // socket aceito pode herdar O_NONBLOCK
    }
    ASSERT_EQ(n, 2);
    EXPECT_EQ(std::string(buffer.data(), 2), "oi");
}

TEST(ListenerTest, AceitaVariosClientesEDepoisFicaVazio) {
    Listener listener{0};
    Socket c1 = conectar(listener.port());
    Socket c2 = conectar(listener.port());
    Socket c3 = conectar(listener.port());

    for (int i = 0; i < 3; ++i) {
        AcceptResult result = aceitar_com_espera(listener);
        EXPECT_EQ(result.status, AcceptStatus::ACCEPTED) << "cliente " << i;
    }

    EXPECT_EQ(listener.accept().status, AcceptStatus::EMPTY);
}

TEST(ListenerTest, ReusaAPortaLogoDepoisDeFechar) {
    std::uint16_t porta = 0;
    {
        Listener listener{0};
        porta = listener.port();
        Socket cliente = conectar(porta);
        AcceptResult aceito = aceitar_com_espera(listener);
        ASSERT_EQ(aceito.status, AcceptStatus::ACCEPTED);
        aceito.socket.reset(); // quem fecha primeiro (o servidor) fica com a conexao em TIME_WAIT
    }

    // sem SO_REUSEADDR isto falharia com "Address already in use" por ate alguns minutos
    EXPECT_NO_THROW((Listener{porta}));
}

TEST(ListenerTest, MoverTransfereOSocketDeEscuta) {
    Listener original{0};
    const std::uint16_t porta = original.port();

    Listener movido = std::move(original);

    EXPECT_EQ(movido.port(), porta);
    Socket cliente = conectar(porta);
    EXPECT_EQ(aceitar_com_espera(movido).status, AcceptStatus::ACCEPTED);
}
