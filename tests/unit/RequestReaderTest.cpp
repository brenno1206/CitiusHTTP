#include "citius/RequestReader.hpp"
#include "citius/Socket.hpp"
#include <chrono>
#include <gtest/gtest.h>
#include <string>
#include <vector>
#include <sys/socket.h>
#include <thread>
#include <utility>

using citius::ReadStatus;
using citius::Socket;
using citius::read_request_head;

namespace {

// Par de sockets conectados entre si (um "cano" local): o que 'cliente' envia, 'servidor' recebe.
struct Par {
    Socket cliente;
    Socket servidor;
};

Par novo_par() {
    int fds[2];
    EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    Par par{Socket{fds[0]}, Socket{fds[1]}};
    EXPECT_TRUE(par.cliente.set_no_sigpipe());
    EXPECT_TRUE(par.servidor.set_no_sigpipe());
    return par;
}

// Envia cada pedaco com uma pausa, simulando uma rede lenta que fragmenta a requisicao.
std::thread enviar_em_pedacos(Socket& socket, std::vector<std::string> pedacos) {
    return std::thread([&socket, pedacos = std::move(pedacos)] {
        for (const auto& pedaco : pedacos) {
            EXPECT_TRUE(socket.send_all(pedaco));
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    });
}

constexpr std::size_t kLimite = 1024;

} // namespace

TEST(RequestReaderTest, LeRequisicaoCompletaEmUmaVez) {
    auto par = novo_par();
    ASSERT_TRUE(par.cliente.send_all("GET / HTTP/1.1\r\nHost: x\r\n\r\n"));

    auto resultado = read_request_head(par.servidor, kLimite);

    EXPECT_EQ(resultado.status, ReadStatus::COMPLETE);
    EXPECT_EQ(resultado.data, "GET / HTTP/1.1\r\nHost: x\r\n\r\n");
}

TEST(RequestReaderTest, JuntaRequisicaoFragmentada) {
    auto par = novo_par();
    auto envio = enviar_em_pedacos(par.cliente, {"GET / HT", "TP/1.1\r\nHo", "st: x\r\n", "\r\n"});

    auto resultado = read_request_head(par.servidor, kLimite);
    envio.join();

    EXPECT_EQ(resultado.status, ReadStatus::COMPLETE);
    EXPECT_EQ(resultado.data, "GET / HTTP/1.1\r\nHost: x\r\n\r\n");
}

TEST(RequestReaderTest, EncontraDelimitadorPartidoEntreLeituras) {
    auto par = novo_par();
    auto envio = enviar_em_pedacos(par.cliente, {"GET / HTTP/1.1\r\n\r", "\n"});

    auto resultado = read_request_head(par.servidor, kLimite);
    envio.join();

    EXPECT_EQ(resultado.status, ReadStatus::COMPLETE);
}

TEST(RequestReaderTest, MantemBytesQueChegaramAposOsCabecalhos) {
    auto par = novo_par();
    ASSERT_TRUE(par.cliente.send_all("POST / HTTP/1.1\r\n\r\ncorpo"));

    auto resultado = read_request_head(par.servidor, kLimite);

    EXPECT_EQ(resultado.status, ReadStatus::COMPLETE);
    EXPECT_EQ(resultado.data, "POST / HTTP/1.1\r\n\r\ncorpo");
}

TEST(RequestReaderTest, RejeitaCabecalhosMaioresQueOLimite) {
    auto par = novo_par();
    ASSERT_TRUE(par.cliente.send_all(std::string(200, 'A')));  // nunca envia \r\n\r\n

    auto resultado = read_request_head(par.servidor, 64);

    EXPECT_EQ(resultado.status, ReadStatus::TOO_LARGE);
}

TEST(RequestReaderTest, ClienteQueFechaSemEnviarNada) {
    auto par = novo_par();
    par.cliente.reset();

    auto resultado = read_request_head(par.servidor, kLimite);

    EXPECT_EQ(resultado.status, ReadStatus::CLOSED);
    EXPECT_TRUE(resultado.data.empty());
}

TEST(RequestReaderTest, ClienteQueFechaComRequisicaoIncompleta) {
    auto par = novo_par();
    ASSERT_TRUE(par.cliente.send_all("GET / HTTP/1.1\r\nHo"));
    par.cliente.reset();

    auto resultado = read_request_head(par.servidor, kLimite);

    EXPECT_EQ(resultado.status, ReadStatus::CLOSED);
}

TEST(RequestReaderTest, SocketInvalidoDaErro) {
    Socket invalido;

    EXPECT_EQ(read_request_head(invalido, kLimite).status, ReadStatus::ERROR);
}
