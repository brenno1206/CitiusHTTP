#include "citius/Server.hpp"
#include "citius/Socket.hpp"
#include <arpa/inet.h>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <thread>
#include <vector>

using citius::Server;
using citius::Socket;

namespace {

// Pasta publica de teste (criada uma vez): index.html, style.css e um segredo FORA da raiz.
const std::filesystem::path& raiz_de_teste() {
    static const std::filesystem::path root = [] {
        auto base = std::filesystem::temp_directory_path() / "citius_server_test";
        std::filesystem::remove_all(base);
        std::filesystem::create_directories(base / "www");
        std::ofstream{base / "www" / "index.html"} << "Hello Citius!";
        std::ofstream{base / "www" / "style.css"} << "body{}";
        std::ofstream{base / "secret.txt"} << "segredo";
        return base / "www";
    }();
    return root;
}

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

// Conecta, envia a requisicao inteira e le a resposta. Para servidores que ja estao em run().
std::string requisitar_em(std::uint16_t port, const std::string& raw) {
    Socket cliente = conectar(port);
    EXPECT_TRUE(cliente.send_all(raw));
    return ler_tudo(cliente);
}

void esperar(int milissegundos) {
    std::this_thread::sleep_for(std::chrono::milliseconds(milissegundos));
}

// Servidor com run() numa thread. O destrutor pede a parada e espera: como o pool espera os clientes
// em andamento, os clientes do teste precisam ser fechados ANTES (declare-os depois deste objeto).
struct ServidorEmExecucao {
    explicit ServidorEmExecucao(std::size_t threads = 2, std::size_t fila = 128)
        : server{0, raiz_de_teste(), threads, fila}, thread{[this] { server.run(); }} {}

    ~ServidorEmExecucao() {
        server.request_stop();
        thread.join();
    }

    Server server;
    std::thread thread;
};

constexpr const char* kGetIndex = "GET /index.html HTTP/1.1\r\nHost: x\r\n\r\n";

} // namespace

TEST(ServerTest, PortaZeroEscolheUmaPortaLivre) {
    Server server{0, raiz_de_teste()};

    EXPECT_NE(server.port(), 0);
}

TEST(ServerTest, ResponderGetValidoCom200) {
    Server server{0, raiz_de_teste()};

    std::string resposta = requisitar(server, "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n");

    EXPECT_EQ(resposta,
              "HTTP/1.1 200 OK\r\n"
              "Content-Type: text/html; charset=utf-8\r\n"
              "Connection: close\r\n"
              "Content-Length: 13\r\n"
              "\r\n"
              "Hello Citius!");
}

TEST(ServerTest, MontaRequisicaoEnviadaEmPedacos) {
    Server server{0, raiz_de_teste()};

    std::string resposta = requisitar(server, {"GET /index.html HT", "TP/1.1\r\nHo", "st: x\r\n\r", "\n"});

    EXPECT_TRUE(comeca_com(resposta, "HTTP/1.1 200 OK\r\n"));
}

TEST(ServerTest, Http10SemHostEhAceito) {
    Server server{0, raiz_de_teste()};

    EXPECT_TRUE(comeca_com(requisitar(server, "GET / HTTP/1.0\r\n\r\n"), "HTTP/1.1 200 OK\r\n"));
}

TEST(ServerTest, HostAusenteEm11Da400) {
    Server server{0, raiz_de_teste()};

    std::string resposta = requisitar(server, "GET / HTTP/1.1\r\n\r\n");

    EXPECT_TRUE(comeca_com(resposta, "HTTP/1.1 400 Bad Request\r\n"));
    EXPECT_NE(resposta.find("Connection: close\r\n"), std::string::npos);
}

TEST(ServerTest, MetodoDesconhecidoDa501) {
    Server server{0, raiz_de_teste()};

    EXPECT_TRUE(comeca_com(requisitar(server, "FOO / HTTP/1.1\r\nHost: x\r\n\r\n"),
                           "HTTP/1.1 501 Not Implemented\r\n"));
}

TEST(ServerTest, VersaoNaoSuportadaDa505) {
    Server server{0, raiz_de_teste()};

    EXPECT_TRUE(comeca_com(requisitar(server, "GET / HTTP/2.0\r\nHost: x\r\n\r\n"),
                           "HTTP/1.1 505 HTTP Version Not Supported\r\n"));
}

TEST(ServerTest, AlvoMuitoLongoDa414) {
    Server server{0, raiz_de_teste()};

    std::string requisicao = "GET /" + std::string(3000, 'a') + " HTTP/1.1\r\nHost: x\r\n\r\n";

    EXPECT_TRUE(comeca_com(requisitar(server, requisicao), "HTTP/1.1 414 URI Too Long\r\n"));
}

TEST(ServerTest, MuitosCabecalhosDa431) {
    Server server{0, raiz_de_teste()};

    std::string requisicao = "GET / HTTP/1.1\r\nHost: x\r\n";
    for (int i = 0; i < 70; ++i) {
        requisicao += "X-" + std::to_string(i) + ": v\r\n";
    }
    requisicao += "\r\n";

    EXPECT_TRUE(comeca_com(requisitar(server, requisicao),
                           "HTTP/1.1 431 Request Header Fields Too Large\r\n"));
}

TEST(ServerTest, CabecaMaiorQue8KbSemFimDa431) {
    Server server{0, raiz_de_teste()};

    // 8193 bytes (um acima do limite) e sem "\r\n\r\n": o servidor le tudo antes de responder
    std::string requisicao = "GET / HTTP/1.1\r\nX: " + std::string(8193 - 19, 'a');
    ASSERT_EQ(requisicao.size(), 8193u);

    EXPECT_TRUE(comeca_com(requisitar(server, requisicao),
                           "HTTP/1.1 431 Request Header Fields Too Large\r\n"));
}

TEST(ServerTest, ContentLengthDaRespostaCombinaComOCorpo) {
    Server server{0, raiz_de_teste()};

    std::string resposta = requisitar(server, "GET /x HTTP/1.1\r\nHost: x\r\n\r\n");

    auto fim_cabecalhos = resposta.find("\r\n\r\n");
    ASSERT_NE(fim_cabecalhos, std::string::npos);
    std::string corpo = resposta.substr(fim_cabecalhos + 4);
    EXPECT_NE(resposta.find("Content-Length: " + std::to_string(corpo.size()) + "\r\n"), std::string::npos);
}

TEST(ServerTest, ClienteQueFechaSemEnviarNadaNaoDerrubaOServidor) {
    Server server{0, raiz_de_teste()};

    std::thread servidor([&] { server.serve_one(); });
    { Socket cliente = conectar(server.port()); }  // conecta e fecha
    servidor.join();

    // o proximo cliente e atendido normalmente
    EXPECT_TRUE(comeca_com(requisitar(server, "GET / HTTP/1.1\r\nHost: x\r\n\r\n"), "HTTP/1.1 200 OK\r\n"));
}

TEST(ServerTest, AtendeClientesEmSequencia) {
    Server server{0, raiz_de_teste()};

    for (int i = 0; i < 3; ++i) {
        EXPECT_TRUE(comeca_com(requisitar(server, "GET / HTTP/1.1\r\nHost: x\r\n\r\n"), "HTTP/1.1 200 OK\r\n"));
    }
}

TEST(ServerTest, ServeCssComTipoCerto) {
    Server server{0, raiz_de_teste()};

    std::string resposta = requisitar(server, "GET /style.css?v=1 HTTP/1.1\r\nHost: x\r\n\r\n");

    EXPECT_TRUE(comeca_com(resposta, "HTTP/1.1 200 OK\r\n"));
    EXPECT_NE(resposta.find("Content-Type: text/css; charset=utf-8\r\n"), std::string::npos);
    EXPECT_TRUE(resposta.ends_with("\r\n\r\nbody{}"));
}

TEST(ServerTest, ArquivoInexistenteDa404) {
    Server server{0, raiz_de_teste()};

    EXPECT_TRUE(comeca_com(requisitar(server, "GET /nao-existe HTTP/1.1\r\nHost: x\r\n\r\n"),
                           "HTTP/1.1 404 Not Found\r\n"));
}

TEST(ServerTest, PathTraversalNaoVazaArquivoDeForaDaRaiz) {
    for (const char* alvo : {"/../secret.txt", "/%2e%2e/secret.txt", "/..%2fsecret.txt"}) {
        Server server{0, raiz_de_teste()};

        std::string resposta = requisitar(server, std::string("GET ") + alvo + " HTTP/1.1\r\nHost: x\r\n\r\n");

        EXPECT_TRUE(comeca_com(resposta, "HTTP/1.1 404 Not Found\r\n")) << alvo;
        EXPECT_EQ(resposta.find("segredo"), std::string::npos) << alvo;
    }
}

TEST(ServerTest, PostEmArquivoEstaticoDa405) {
    Server server{0, raiz_de_teste()};

    std::string resposta = requisitar(server, "POST / HTTP/1.1\r\nHost: x\r\n\r\n");

    EXPECT_TRUE(comeca_com(resposta, "HTTP/1.1 405 Method Not Allowed\r\n"));
    EXPECT_NE(resposta.find("Allow: GET\r\n"), std::string::npos);
}

TEST(ServerTest, RotaRegistradaResponde200) {
    Server server{0, raiz_de_teste()};
    server.router().get("/ping", [](const citius::HttpRequest&) {
        citius::HttpResponse response;
        response.set_body("pong");
        return response;
    });

    std::string resposta = requisitar(server, "GET /ping HTTP/1.1\r\nHost: x\r\n\r\n");

    EXPECT_TRUE(comeca_com(resposta, "HTTP/1.1 200 OK\r\n"));
    EXPECT_NE(resposta.find("Connection: close\r\n"), std::string::npos);
    EXPECT_TRUE(resposta.ends_with("\r\n\r\npong"));
}

TEST(ServerTest, MetodoErradoNaRotaRegistradaDa405ComAllow) {
    Server server{0, raiz_de_teste()};
    server.router().get("/ping", [](const citius::HttpRequest&) { return citius::HttpResponse{}; });

    std::string resposta = requisitar(server, "POST /ping HTTP/1.1\r\nHost: x\r\n\r\n");

    EXPECT_TRUE(comeca_com(resposta, "HTTP/1.1 405 Method Not Allowed\r\n"));
    EXPECT_NE(resposta.find("Allow: GET\r\n"), std::string::npos);
}

TEST(ServerTest, ArquivoEstaticoContinuaFuncionandoComRotasRegistradas) {
    Server server{0, raiz_de_teste()};
    server.router().get("/ping", [](const citius::HttpRequest&) { return citius::HttpResponse{}; });

    std::string resposta = requisitar(server, "GET /index.html HTTP/1.1\r\nHost: x\r\n\r\n");

    EXPECT_TRUE(comeca_com(resposta, "HTTP/1.1 200 OK\r\n"));
    EXPECT_TRUE(resposta.ends_with("\r\n\r\nHello Citius!"));
}

TEST(ServerTest, RotaTemPrioridadeSobreArquivoEstatico) {
    Server server{0, raiz_de_teste()};
    server.router().get("/index.html", [](const citius::HttpRequest&) {
        citius::HttpResponse response;
        response.set_body("da rota");
        return response;
    });

    EXPECT_TRUE(requisitar(server, "GET /index.html HTTP/1.1\r\nHost: x\r\n\r\n").ends_with("\r\n\r\nda rota"));
}

TEST(ServerTest, HandlerQueLancaExcecaoDa500EOServidorContinua) {
    Server server{0, raiz_de_teste()};
    server.router().get("/falha", [](const citius::HttpRequest&) -> citius::HttpResponse {
        throw std::runtime_error("bug no handler");
    });

    std::string resposta = requisitar(server, "GET /falha HTTP/1.1\r\nHost: x\r\n\r\n");

    EXPECT_TRUE(comeca_com(resposta, "HTTP/1.1 500 Internal Server Error\r\n"));
    EXPECT_EQ(resposta.find("bug no handler"), std::string::npos); // nao vaza detalhe interno ao cliente
    // o proximo cliente e atendido normalmente
    EXPECT_TRUE(comeca_com(requisitar(server, "GET / HTTP/1.1\r\nHost: x\r\n\r\n"), "HTTP/1.1 200 OK\r\n"));
}

TEST(ServerConcorrenciaTest, ClienteMudoNaoBloqueiaOsOutros) {
    ServidorEmExecucao em_execucao{2};
    Socket mudo = conectar(em_execucao.server.port()); // conecta e nao envia nada
    esperar(100);

    std::string resposta = requisitar_em(em_execucao.server.port(), kGetIndex);

    EXPECT_TRUE(comeca_com(resposta, "HTTP/1.1 200 OK\r\n"));
}

TEST(ServerConcorrenciaTest, MuitosClientesAoMesmoTempo) {
    ServidorEmExecucao em_execucao{4};
    constexpr int kClientes = 32;
    std::vector<std::string> respostas(kClientes);

    std::vector<std::thread> clientes;
    for (int i = 0; i < kClientes; ++i) {
        clientes.emplace_back([&, i] { respostas[i] = requisitar_em(em_execucao.server.port(), kGetIndex); });
    }
    for (auto& cliente : clientes) {
        cliente.join();
    }

    for (const auto& resposta : respostas) {
        EXPECT_TRUE(comeca_com(resposta, "HTTP/1.1 200 OK\r\n"));
        EXPECT_TRUE(resposta.ends_with("\r\n\r\nHello Citius!"));
    }
}

TEST(ServerConcorrenciaTest, FilaCheiaResponde503EDepoisSeRecupera) {
    // 1 thread e fila de 1: o primeiro mudo ocupa a thread, o segundo ocupa a fila, o terceiro leva 503
    ServidorEmExecucao em_execucao{1, 1};
    std::string rejeitada;
    {
        Socket mudo1 = conectar(em_execucao.server.port());
        esperar(150);
        Socket mudo2 = conectar(em_execucao.server.port());
        esperar(150);

        rejeitada = requisitar_em(em_execucao.server.port(), kGetIndex);
    } // fecha os mudos: o pool desocupa

    EXPECT_TRUE(comeca_com(rejeitada, "HTTP/1.1 503 Service Unavailable\r\n"));
    EXPECT_NE(rejeitada.find("Retry-After: 1\r\n"), std::string::npos);
    EXPECT_NE(rejeitada.find("Connection: close\r\n"), std::string::npos);

    // depois que os mudos saem, o servidor volta a atender
    EXPECT_TRUE(comeca_com(requisitar_em(em_execucao.server.port(), kGetIndex), "HTTP/1.1 200 OK\r\n"));
}

TEST(ServerConcorrenciaTest, RequestStopFazRunRetornar) {
    Server server{0, raiz_de_teste()};
    std::future<void> terminou = std::async(std::launch::async, [&] { server.run(); });
    esperar(150);

    server.request_stop();

    EXPECT_EQ(terminou.wait_for(std::chrono::seconds(5)), std::future_status::ready);
}

TEST(ServerConcorrenciaTest, ParadaTerminaOClienteEmAndamento) {
    Server server{0, raiz_de_teste()};
    std::future<void> terminou = std::async(std::launch::async, [&] { server.run(); });

    Socket cliente = conectar(server.port());
    EXPECT_TRUE(cliente.send_all("GET /index.html HTTP/1.1\r\nHost: x\r\n")); // requisicao incompleta
    esperar(150);                                                                  // servidor ja aceitou

    server.request_stop();
    esperar(250); // run() ja saiu do laco de accept e esta esperando este cliente
    EXPECT_NE(terminou.wait_for(std::chrono::milliseconds(0)), std::future_status::ready);

    EXPECT_TRUE(cliente.send_all("\r\n")); // completa a requisicao depois do pedido de parada
    std::string resposta = ler_tudo(cliente);

    EXPECT_TRUE(comeca_com(resposta, "HTTP/1.1 200 OK\r\n"));
    EXPECT_EQ(terminou.wait_for(std::chrono::seconds(5)), std::future_status::ready);
}

TEST(ServerConcorrenciaTest, AposAParadaNaoAceitaMaisConexoes) {
    Server server{0, raiz_de_teste()};
    server.request_stop();

    server.run(); // retorna na hora: ninguem a atender

    server.serve_one(); // idem, sem bloquear esperando cliente
    SUCCEED();
}

TEST(ServerConcorrenciaTest, FilaZeroLancaExcecao) {
    EXPECT_THROW((Server{0, raiz_de_teste(), 2, 0}), std::invalid_argument);
}

TEST(ServerTest, ClienteMudoRecebe408ELiberaAThread) {
    Server server{0, raiz_de_teste()};
    server.set_read_timeout(std::chrono::milliseconds(100));

    // nao envia nada: sem timeout, o servidor ficaria esperando para sempre
    std::string resposta = requisitar(server, std::vector<std::string>{});

    EXPECT_TRUE(comeca_com(resposta, "HTTP/1.1 408 Request Timeout\r\n")) << resposta;
    EXPECT_NE(resposta.find("Connection: close\r\n"), std::string::npos);
}

TEST(ServerTest, ClienteMudoNaoPrendeOUnicoSlotDoPool) {
    ServidorEmExecucao exec{1, 4};
    exec.server.set_read_timeout(std::chrono::milliseconds(100));
    Socket mudo = conectar(exec.server.port());

    // a unica thread esta com o cliente mudo; depois do timeout ela atende o proximo
    std::string resposta = requisitar_em(exec.server.port(), kGetIndex);

    EXPECT_TRUE(comeca_com(resposta, "HTTP/1.1 200 OK\r\n")) << resposta;
}

// ---- corpo da requisicao ----

namespace {
// Registra POST /eco, que devolve o corpo recebido.
void registrar_eco(Server& server) {
    server.router().post("/eco", [](const citius::HttpRequest& request) {
        citius::HttpResponse response;
        response.set_body(request.body, "text/plain");
        return response;
    });
}
} // namespace

TEST(ServerTest, PostComCorpoChegaAoHandler) {
    Server server{0, raiz_de_teste()};
    registrar_eco(server);

    std::string resposta = requisitar(server, "POST /eco HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhello");

    EXPECT_TRUE(comeca_com(resposta, "HTTP/1.1 200 OK\r\n")) << resposta;
    EXPECT_TRUE(resposta.ends_with("\r\n\r\nhello")) << resposta;
}

TEST(ServerTest, CorpoEnviadoEmPedacosLentosEJuntado) {
    Server server{0, raiz_de_teste()};
    registrar_eco(server);

    std::string resposta = requisitar(server, std::vector<std::string>{
        "POST /eco HTTP/1.1\r\nHost: x\r\nContent-Length: 10\r\n\r\nabc", "def", "ghij"});

    EXPECT_TRUE(resposta.ends_with("\r\n\r\nabcdefghij")) << resposta;
}

TEST(ServerTest, ContentLengthAcimaDoLimiteDa413SemLerOCorpo) {
    Server server{0, raiz_de_teste()};
    registrar_eco(server);

    std::string resposta = requisitar(server, "POST /eco HTTP/1.1\r\nHost: x\r\nContent-Length: 999999999\r\n\r\n");

    EXPECT_TRUE(comeca_com(resposta, "HTTP/1.1 413 Content Too Large\r\n")) << resposta;
}

TEST(ServerTest, ContentLengthInvalidoDa400) {
    Server server{0, raiz_de_teste()};
    registrar_eco(server);

    std::string resposta = requisitar(server, "POST /eco HTTP/1.1\r\nHost: x\r\nContent-Length: abc\r\n\r\n");

    EXPECT_TRUE(comeca_com(resposta, "HTTP/1.1 400 Bad Request\r\n")) << resposta;
}

TEST(ServerTest, CorpoPrometidoMasNaoEnviadoDa408) {
    Server server{0, raiz_de_teste()};
    server.set_read_timeout(std::chrono::milliseconds(100));
    registrar_eco(server);

    std::string resposta = requisitar(server, "POST /eco HTTP/1.1\r\nHost: x\r\nContent-Length: 10\r\n\r\nabc");

    EXPECT_TRUE(comeca_com(resposta, "HTTP/1.1 408 Request Timeout\r\n")) << resposta;
}
