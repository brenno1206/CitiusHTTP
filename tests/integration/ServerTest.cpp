#include "citius/Server.hpp"
#include "citius/Socket.hpp"
#include <arpa/inet.h>
#include <array>
#include <chrono>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <thread>
#include <vector>

using citius::Server;
using citius::Socket;

namespace {

// Conecta em 127.0.0.1:port. Um timeout de leitura evita que um bug deixe o teste preso.
Socket conectar(std::uint16_t port) {
    Socket socket{::socket(AF_INET, SOCK_STREAM, 0)};
    EXPECT_TRUE(socket.valid());
    EXPECT_TRUE(socket.set_no_sigpipe());

    timeval timeout{.tv_sec = 5, .tv_usec = 0};
    EXPECT_EQ(::setsockopt(socket.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)), 0);

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    EXPECT_EQ(::connect(socket.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)), 0);
    return socket;
}

// Le ate o servidor fechar a conexao.
std::string ler_tudo(Socket& socket) {
    std::string recebido;
    std::array<char, 4096> buffer;
    ssize_t n;
    while ((n = socket.recv_some(buffer)) > 0) {
        recebido.append(buffer.data(), static_cast<std::size_t>(n));
    }
    return recebido;
}

// Atende um cliente: sobe serve_one() numa thread, envia os pedacos (com pausa entre eles) e
// devolve a resposta completa.
std::string requisitar(Server& server, const std::vector<std::string>& pedacos) {
    std::thread servidor([&] { server.serve_one(); });

    Socket cliente = conectar(server.port());
    for (const auto& pedaco : pedacos) {
        EXPECT_TRUE(cliente.send_all(pedaco));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    std::string resposta = ler_tudo(cliente);
    servidor.join();
    return resposta;
}

std::string requisitar(Server& server, const std::string& raw) {
    return requisitar(server, std::vector<std::string>{raw});
}

bool comeca_com(const std::string& texto, const std::string& prefixo) {
    return texto.rfind(prefixo, 0) == 0;
}

} // namespace

TEST(ServerTest, PortaZeroEscolheUmaPortaLivre) {
    Server server{0};

    EXPECT_NE(server.port(), 0);
}

TEST(ServerTest, ResponderGetValidoCom200) {
    Server server{0};

    std::string resposta = requisitar(server, "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n");

    EXPECT_EQ(resposta,
              "HTTP/1.1 200 OK\r\n"
              "Content-Type: text/plain; charset=utf-8\r\n"
              "Connection: close\r\n"
              "Content-Length: 13\r\n"
              "\r\n"
              "Hello Citius!");
}

TEST(ServerTest, MontaRequisicaoEnviadaEmPedacos) {
    Server server{0};

    std::string resposta = requisitar(server, {"GET /index.html HT", "TP/1.1\r\nHo", "st: x\r\n\r", "\n"});

    EXPECT_TRUE(comeca_com(resposta, "HTTP/1.1 200 OK\r\n"));
}

TEST(ServerTest, Http10SemHostEhAceito) {
    Server server{0};

    EXPECT_TRUE(comeca_com(requisitar(server, "GET / HTTP/1.0\r\n\r\n"), "HTTP/1.1 200 OK\r\n"));
}

TEST(ServerTest, HostAusenteEm11Da400) {
    Server server{0};

    std::string resposta = requisitar(server, "GET / HTTP/1.1\r\n\r\n");

    EXPECT_TRUE(comeca_com(resposta, "HTTP/1.1 400 Bad Request\r\n"));
    EXPECT_NE(resposta.find("Connection: close\r\n"), std::string::npos);
}

TEST(ServerTest, MetodoDesconhecidoDa501) {
    Server server{0};

    EXPECT_TRUE(comeca_com(requisitar(server, "FOO / HTTP/1.1\r\nHost: x\r\n\r\n"),
                           "HTTP/1.1 501 Not Implemented\r\n"));
}

TEST(ServerTest, VersaoNaoSuportadaDa505) {
    Server server{0};

    EXPECT_TRUE(comeca_com(requisitar(server, "GET / HTTP/2.0\r\nHost: x\r\n\r\n"),
                           "HTTP/1.1 505 HTTP Version Not Supported\r\n"));
}

TEST(ServerTest, AlvoMuitoLongoDa414) {
    Server server{0};

    std::string requisicao = "GET /" + std::string(3000, 'a') + " HTTP/1.1\r\nHost: x\r\n\r\n";

    EXPECT_TRUE(comeca_com(requisitar(server, requisicao), "HTTP/1.1 414 URI Too Long\r\n"));
}

TEST(ServerTest, MuitosCabecalhosDa431) {
    Server server{0};

    std::string requisicao = "GET / HTTP/1.1\r\nHost: x\r\n";
    for (int i = 0; i < 70; ++i) {
        requisicao += "X-" + std::to_string(i) + ": v\r\n";
    }
    requisicao += "\r\n";

    EXPECT_TRUE(comeca_com(requisitar(server, requisicao),
                           "HTTP/1.1 431 Request Header Fields Too Large\r\n"));
}

TEST(ServerTest, CabecaMaiorQue8KbSemFimDa431) {
    Server server{0};

    // 8193 bytes (um acima do limite) e sem "\r\n\r\n": o servidor le tudo antes de responder
    std::string requisicao = "GET / HTTP/1.1\r\nX: " + std::string(8193 - 19, 'a');
    ASSERT_EQ(requisicao.size(), 8193u);

    EXPECT_TRUE(comeca_com(requisitar(server, requisicao),
                           "HTTP/1.1 431 Request Header Fields Too Large\r\n"));
}

TEST(ServerTest, ContentLengthDaRespostaCombinaComOCorpo) {
    Server server{0};

    std::string resposta = requisitar(server, "GET /x HTTP/1.1\r\nHost: x\r\n\r\n");

    auto fim_cabecalhos = resposta.find("\r\n\r\n");
    ASSERT_NE(fim_cabecalhos, std::string::npos);
    std::string corpo = resposta.substr(fim_cabecalhos + 4);
    EXPECT_NE(resposta.find("Content-Length: " + std::to_string(corpo.size()) + "\r\n"), std::string::npos);
}

TEST(ServerTest, ClienteQueFechaSemEnviarNadaNaoDerrubaOServidor) {
    Server server{0};

    std::thread servidor([&] { server.serve_one(); });
    { Socket cliente = conectar(server.port()); }  // conecta e fecha
    servidor.join();

    // o proximo cliente e atendido normalmente
    EXPECT_TRUE(comeca_com(requisitar(server, "GET / HTTP/1.1\r\nHost: x\r\n\r\n"), "HTTP/1.1 200 OK\r\n"));
}

TEST(ServerTest, AtendeClientesEmSequencia) {
    Server server{0};

    for (int i = 0; i < 3; ++i) {
        EXPECT_TRUE(comeca_com(requisitar(server, "GET / HTTP/1.1\r\nHost: x\r\n\r\n"), "HTTP/1.1 200 OK\r\n"));
    }
}
