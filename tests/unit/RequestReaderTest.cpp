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
using citius::read_request_body;
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

TEST(RequestReaderTest, ClienteMudoDaTimeout) {
    auto par = novo_par();  // 'cliente' fica aberto e nao envia nada
    ASSERT_TRUE(par.servidor.set_recv_timeout(std::chrono::milliseconds(50)));

    auto resultado = read_request_head(par.servidor, kLimite);

    EXPECT_EQ(resultado.status, ReadStatus::TIMEOUT);
}

TEST(RequestReaderTest, TimeoutNoMeioDaRequisicaoMantemOsBytesLidos) {
    auto par = novo_par();
    ASSERT_TRUE(par.servidor.set_recv_timeout(std::chrono::milliseconds(50)));
    ASSERT_TRUE(par.cliente.send_all("GET / HTTP/1.1\r\nHo"));

    auto resultado = read_request_head(par.servidor, kLimite);

    EXPECT_EQ(resultado.status, ReadStatus::TIMEOUT);
    EXPECT_EQ(resultado.data, "GET / HTTP/1.1\r\nHo");
}

// ---- cabeca + corpo ----

TEST(RequestReaderTest, InformaOndeTerminaACabeca) {
    auto par = novo_par();
    ASSERT_TRUE(par.cliente.send_all("POST / HTTP/1.1\r\n\r\ncorpo"));

    auto resultado = read_request_head(par.servidor, kLimite);

    EXPECT_EQ(resultado.head_size, std::string("POST / HTTP/1.1\r\n\r\n").size());
}

TEST(RequestReaderTest, CorpoJaRecebidoJuntoComACabecaNaoLeDoSocket) {
    auto par = novo_par();  // nada sera enviado: se tentasse ler, daria timeout
    ASSERT_TRUE(par.servidor.set_recv_timeout(std::chrono::milliseconds(50)));

    auto resultado = read_request_body(par.servidor, "abcde", 5);

    EXPECT_EQ(resultado.status, ReadStatus::COMPLETE);
    EXPECT_EQ(resultado.data, "abcde");
}

TEST(RequestReaderTest, CompletaCorpoQueChegaAosPedacos) {
    auto par = novo_par();
    auto envio = enviar_em_pedacos(par.cliente, {"cde", "fgh"});

    auto resultado = read_request_body(par.servidor, "ab", 8);
    envio.join();

    EXPECT_EQ(resultado.status, ReadStatus::COMPLETE);
    EXPECT_EQ(resultado.data, "abcdefgh");
}

TEST(RequestReaderTest, CorpoNaoPassaDoContentLength) {
    auto par = novo_par();
    ASSERT_TRUE(par.cliente.send_all("cdefXXXX"));  // sobra: seria a proxima requisicao

    auto resultado = read_request_body(par.servidor, "ab", 6);

    EXPECT_EQ(resultado.status, ReadStatus::COMPLETE);
    EXPECT_EQ(resultado.data, "abcdef");
}

TEST(RequestReaderTest, CorpoIncompletoComClienteFechadoDaClosed) {
    auto par = novo_par();
    ASSERT_TRUE(par.cliente.send_all("cd"));
    par.cliente.reset();

    EXPECT_EQ(read_request_body(par.servidor, "ab", 10).status, ReadStatus::CLOSED);
}

TEST(RequestReaderTest, CorpoParadoDaTimeout) {
    auto par = novo_par();
    ASSERT_TRUE(par.servidor.set_recv_timeout(std::chrono::milliseconds(50)));
    ASSERT_TRUE(par.cliente.send_all("cd"));

    EXPECT_EQ(read_request_body(par.servidor, "ab", 10).status, ReadStatus::TIMEOUT);
}

// ---- sobra de uma requisicao para a proxima (keep-alive) ----

TEST(RequestReaderTest, CabecaCompletaEmInitialNaoLeDoSocket) {
    auto par = novo_par();  // mudo: ler do socket daria timeout
    ASSERT_TRUE(par.servidor.set_recv_timeout(std::chrono::milliseconds(50)));

    auto resultado = read_request_head(par.servidor, kLimite, "GET / HTTP/1.1\r\nHost: x\r\n\r\nSOBRA");

    EXPECT_EQ(resultado.status, ReadStatus::COMPLETE);
    EXPECT_EQ(resultado.data.substr(resultado.head_size), "SOBRA");
}

TEST(RequestReaderTest, InitialIncompletoEcompletadoPeloSocket) {
    auto par = novo_par();
    ASSERT_TRUE(par.cliente.send_all("t: x\r\n\r\n"));

    auto resultado = read_request_head(par.servidor, kLimite, "GET / HTTP/1.1\r\nHos");

    EXPECT_EQ(resultado.status, ReadStatus::COMPLETE);
    EXPECT_EQ(resultado.data, "GET / HTTP/1.1\r\nHost: x\r\n\r\n");
}

TEST(RequestReaderTest, DelimitadorPartidoEntreInitialESocket) {
    auto par = novo_par();
    ASSERT_TRUE(par.cliente.send_all("\n"));

    auto resultado = read_request_head(par.servidor, kLimite, "GET / HTTP/1.1\r\n\r");

    EXPECT_EQ(resultado.status, ReadStatus::COMPLETE);
}

TEST(RequestReaderTest, CabecaMaiorQueOLimiteJaEmInitialDa431) {
    auto par = novo_par();
    std::string cabeca = "GET / HTTP/1.1\r\nX: " + std::string(200, 'a') + "\r\n\r\n";

    EXPECT_EQ(read_request_head(par.servidor, 64, cabeca).status, ReadStatus::TOO_LARGE);
}

TEST(RequestReaderTest, CorpoDevolveASobraDeInitial) {
    auto par = novo_par();

    auto resultado = read_request_body(par.servidor, "abcdeGET /", 5);

    EXPECT_EQ(resultado.status, ReadStatus::COMPLETE);
    EXPECT_EQ(resultado.data, "abcde");
    EXPECT_EQ(resultado.surplus, "GET /");
}
