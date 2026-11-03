#include "citius/server/Server.hpp"
#include "citius/net/Socket.hpp"
#include <arpa/inet.h>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <optional>
#include <stdexcept>
#include <string>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <thread>
#include <unistd.h>
#include <vector>

using citius::Server;
using citius::Socket;

namespace {

// Pasta publica de teste (criada uma vez por processo): index.html, style.css e um segredo FORA da raiz.
// O nome leva o pid porque o ctest roda cada teste num processo e, com --parallel, varios ao mesmo
// tempo: uma pasta fixa seria apagada e recriada por um enquanto outro a usa.
const std::filesystem::path& raiz_de_teste() {
    struct Pasta {
        std::filesystem::path base =
            std::filesystem::temp_directory_path() / ("citius_server_test_" + std::to_string(::getpid()));

        Pasta() {
            std::filesystem::remove_all(base);
            std::filesystem::create_directories(base / "www");
            std::ofstream{base / "www" / "index.html"} << "Hello Citius!";
            std::ofstream{base / "www" / "style.css"} << "body{}";
            std::ofstream{base / "secret.txt"} << "segredo";
        }
        ~Pasta() {
            std::error_code ignorado;
            std::filesystem::remove_all(base, ignorado);
        }
    };
    static const Pasta pasta;
    static const std::filesystem::path root = pasta.base / "www";
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

// Avisa que o cliente nao enviara mais nada (half-close). Com keep-alive o servidor espera a proxima
// requisicao; ver o fim do envio faz ele encerrar a conexao, e so entao ler_tudo() termina.
void terminar_envio(Socket& socket) {
    EXPECT_EQ(::shutdown(socket.get(), SHUT_WR), 0);
}

// Atende um cliente: sobe serve_one() numa thread, envia os pedacos (com pausa entre eles) e
// devolve a resposta completa. Com @p encerrar_envio, faz half-close depois de enviar; passe false
// para testar o servidor esperando algo que nunca chega (timeouts).
std::string requisitar(Server& server, const std::vector<std::string>& pedacos, bool encerrar_envio = true) {
    std::thread servidor([&] { server.serve_one(); });

    Socket cliente = conectar(server.port());
    for (const auto& pedaco : pedacos) {
        EXPECT_TRUE(cliente.send_all(pedaco));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (encerrar_envio) terminar_envio(cliente);
    std::string resposta = ler_tudo(cliente);
    servidor.join();
    return resposta;
}

std::string requisitar(Server& server, const std::string& raw, bool encerrar_envio = true) {
    return requisitar(server, std::vector<std::string>{raw}, encerrar_envio);
}

bool comeca_com(const std::string& texto, const std::string& prefixo) {
    return texto.rfind(prefixo, 0) == 0;
}

// Conecta, envia a requisicao inteira e le a resposta. Para servidores que ja estao em run().
std::string requisitar_em(std::uint16_t port, const std::string& raw) {
    Socket cliente = conectar(port);
    EXPECT_TRUE(cliente.send_all(raw));
    terminar_envio(cliente);
    return ler_tudo(cliente);
}

void esperar(int milissegundos) {
    std::this_thread::sleep_for(std::chrono::milliseconds(milissegundos));
}

// Servidor com run() numa thread. O destrutor pede a parada e espera: como run() espera os clientes
// em andamento, os clientes do teste precisam ser fechados ANTES (declare-os depois deste objeto).
struct ServidorEmExecucao {
    explicit ServidorEmExecucao(std::size_t max_conexoes = citius::kDefaultMaxConnections)
        : server{0, raiz_de_teste(), max_conexoes}, thread{[this] { server.run(); }} {}

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

    EXPECT_EQ(resposta, "HTTP/1.1 200 OK\r\n"
                        "Content-Type: text/html; charset=utf-8\r\n"
                        "Connection: keep-alive\r\n"
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

    EXPECT_TRUE(
        comeca_com(requisitar(server, "FOO / HTTP/1.1\r\nHost: x\r\n\r\n"), "HTTP/1.1 501 Not Implemented\r\n"));
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

    EXPECT_TRUE(comeca_com(requisitar(server, requisicao), "HTTP/1.1 431 Request Header Fields Too Large\r\n"));
}

TEST(ServerTest, CabecaMaiorQue8KbSemFimDa431) {
    Server server{0, raiz_de_teste()};

    // 8193 bytes (um acima do limite) e sem "\r\n\r\n": o servidor le tudo antes de responder
    std::string requisicao = "GET / HTTP/1.1\r\nX: " + std::string(8193 - 19, 'a');
    ASSERT_EQ(requisicao.size(), 8193u);

    EXPECT_TRUE(comeca_com(requisitar(server, requisicao), "HTTP/1.1 431 Request Header Fields Too Large\r\n"));
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
    {
        Socket cliente = conectar(server.port());
    } // conecta e fecha
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

    EXPECT_TRUE(
        comeca_com(requisitar(server, "GET /nao-existe HTTP/1.1\r\nHost: x\r\n\r\n"), "HTTP/1.1 404 Not Found\r\n"));
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
    EXPECT_NE(resposta.find("Connection: keep-alive\r\n"), std::string::npos);
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
    ServidorEmExecucao em_execucao;
    Socket mudo = conectar(em_execucao.server.port()); // conecta e nao envia nada
    esperar(100);

    std::string resposta = requisitar_em(em_execucao.server.port(), kGetIndex);

    EXPECT_TRUE(comeca_com(resposta, "HTTP/1.1 200 OK\r\n"));
}

TEST(ServerConcorrenciaTest, MuitosClientesAoMesmoTempo) {
    ServidorEmExecucao em_execucao;
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

TEST(ServerConcorrenciaTest, LimiteDeConexoesResponde503EDepoisSeRecupera) {
    // limite de 2 conexoes: os dois mudos ocupam as duas vagas e o terceiro leva 503
    ServidorEmExecucao em_execucao{2};
    std::string rejeitada;
    {
        Socket mudo1 = conectar(em_execucao.server.port());
        esperar(150);
        Socket mudo2 = conectar(em_execucao.server.port());
        esperar(150);

        rejeitada = requisitar_em(em_execucao.server.port(), kGetIndex);
    } // fecha os mudos: as vagas voltam

    EXPECT_TRUE(comeca_com(rejeitada, "HTTP/1.1 503 Service Unavailable\r\n"));
    EXPECT_NE(rejeitada.find("Retry-After: 1\r\n"), std::string::npos);
    EXPECT_NE(rejeitada.find("Connection: close\r\n"), std::string::npos);

    // depois que os mudos saem, o servidor volta a atender (da um instante para ele notar que fecharam)
    esperar(150);
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
    esperar(150);                                                             // servidor ja aceitou

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

TEST(ServerConcorrenciaTest, LimiteZeroDeConexoesLancaExcecao) {
    EXPECT_THROW((Server{0, raiz_de_teste(), 0}), std::invalid_argument);
}

namespace {

// Cada conexao do teste gasta dois descritores (o cliente e o lado do servidor, no mesmo processo),
// e o limite padrao do macOS (256) nao comporta centenas delas.
bool elevar_limite_de_fds(rlim_t minimo) {
    rlimit limite{};
    if (::getrlimit(RLIMIT_NOFILE, &limite) != 0) return false;
    if (limite.rlim_cur >= minimo) return true;
    limite.rlim_cur = std::min(minimo, limite.rlim_max);
    return ::setrlimit(RLIMIT_NOFILE, &limite) == 0 && limite.rlim_cur >= minimo;
}

} // namespace

// O ponto da Fase 7: com uma thread por conexao, 500 clientes parados prenderiam 500 threads.
TEST(ServerConcorrenciaTest, QuinhentasConexoesAbertasAoMesmoTempoSaoTodasAtendidas) {
    constexpr int kClientes = 500;
    if (!elevar_limite_de_fds(2 * kClientes + 100)) GTEST_SKIP() << "nao foi possivel subir o limite de descritores";
    ServidorEmExecucao exec;
    std::vector<Socket> clientes; // depois do servidor: fecham antes dele
    clientes.reserve(kClientes);

    for (int i = 0; i < kClientes; ++i) clientes.push_back(conectar(exec.server.port()));
    for (auto& cliente : clientes) ASSERT_TRUE(cliente.send_all(kGetIndex));

    // todas continuam abertas (keep-alive) e todas recebem a resposta
    std::array<char, 4096> buffer;
    for (int i = 0; i < kClientes; ++i) {
        ssize_t n = clientes[i].recv_some(buffer);
        ASSERT_GT(n, 0) << "cliente " << i;
        EXPECT_TRUE(comeca_com(std::string(buffer.data(), static_cast<std::size_t>(n)), "HTTP/1.1 200 OK\r\n"))
            << "cliente " << i;
    }
}

TEST(ServerConcorrenciaTest, SlowlorisComUmByteDeVezEmQuandoRecebe408NoPrazoTotal) {
    ServidorEmExecucao exec;
    exec.server.set_read_timeout(std::chrono::milliseconds(300));
    Socket cliente = conectar(exec.server.port());
    ASSERT_TRUE(cliente.set_recv_timeout(std::chrono::milliseconds(100)));

    // 1 byte a cada ~100 ms: cada leitura chega bem antes de 300 ms, entao um prazo "por leitura"
    // nunca estouraria e a conexao ficaria presa. O prazo certo e o da requisicao inteira.
    const std::string requisicao = "GET /index.html HTTP/1.1\r\nHost: x\r\nX-Lento: aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    const auto inicio = std::chrono::steady_clock::now();
    std::string resposta;
    std::array<char, 4096> buffer;
    for (std::size_t i = 0; i < requisicao.size() && resposta.empty(); ++i) {
        EXPECT_TRUE(cliente.send_all(requisicao.substr(i, 1)));
        ssize_t n = cliente.recv_some(buffer); // espera ate 100 ms: serve de pausa e de leitura
        if (n > 0) resposta.assign(buffer.data(), static_cast<std::size_t>(n));
    }
    const auto demorou = std::chrono::steady_clock::now() - inicio;

    EXPECT_TRUE(comeca_com(resposta, "HTTP/1.1 408 Request Timeout\r\n")) << resposta;
    EXPECT_LT(demorou, std::chrono::milliseconds(1000));
}

TEST(ServerTest, ClienteMudoRecebe408ELiberaAThread) {
    Server server{0, raiz_de_teste()};
    server.set_read_timeout(std::chrono::milliseconds(100));

    // nao envia nada: sem timeout, o servidor ficaria esperando para sempre
    std::string resposta = requisitar(server, std::vector<std::string>{}, false);

    EXPECT_TRUE(comeca_com(resposta, "HTTP/1.1 408 Request Timeout\r\n")) << resposta;
    EXPECT_NE(resposta.find("Connection: close\r\n"), std::string::npos);
}

TEST(ServerTest, ClienteMudoLiberaAUnicaVagaDeConexaoDepoisDoTimeout) {
    ServidorEmExecucao exec{1};
    exec.server.set_read_timeout(std::chrono::milliseconds(100));
    Socket mudo = conectar(exec.server.port());

    // a unica vaga esta com o cliente mudo; depois do timeout (408) ela volta e o proximo e atendido
    esperar(300);
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

    std::string resposta =
        requisitar(server, std::vector<std::string>{"POST /eco HTTP/1.1\r\nHost: x\r\nContent-Length: 10\r\n\r\nabc",
                                                    "def", "ghij"});

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

    std::string resposta = requisitar(server, "POST /eco HTTP/1.1\r\nHost: x\r\nContent-Length: 10\r\n\r\nabc", false);

    EXPECT_TRUE(comeca_com(resposta, "HTTP/1.1 408 Request Timeout\r\n")) << resposta;
}

TEST(ServerTest, TransferEncodingChunkedDa501) {
    Server server{0, raiz_de_teste()};
    registrar_eco(server);

    std::string resposta = requisitar(
        server, "POST /eco HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n");

    EXPECT_TRUE(comeca_com(resposta, "HTTP/1.1 501 Not Implemented\r\n")) << resposta;
}

TEST(ServerTest, TransferEncodingMaisContentLengthDa400) {
    Server server{0, raiz_de_teste()};
    registrar_eco(server);

    std::string resposta = requisitar(
        server, "POST /eco HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nTransfer-Encoding: chunked\r\n\r\nhello");

    EXPECT_TRUE(comeca_com(resposta, "HTTP/1.1 400 Bad Request\r\n")) << resposta;
}

// ---- keep-alive ----

namespace {

// Le respostas HTTP, uma por vez, de uma conexao que continua aberta. Delimita cada resposta pelo
// Content-Length (ler ate fechar nao serve: a conexao nao fecha). Bytes a mais ficam guardados.
struct LeitorDeRespostas {
    explicit LeitorDeRespostas(Socket& s) : socket{s} {}

    // @return a resposta completa, ou "" se a conexao fechou antes (ou deu timeout) sem resposta.
    std::string proxima() {
        std::size_t fim_cabeca;
        while ((fim_cabeca = buffer.find("\r\n\r\n")) == std::string::npos) {
            if (!ler_mais()) return "";
        }
        fim_cabeca += 4;
        std::size_t tamanho = 0;
        if (auto pos = buffer.find("Content-Length: "); pos != std::string::npos && pos < fim_cabeca) {
            tamanho = std::stoul(buffer.substr(pos + 16));
        }
        while (buffer.size() < fim_cabeca + tamanho) {
            if (!ler_mais()) return "";
        }
        std::string resposta = buffer.substr(0, fim_cabeca + tamanho);
        buffer.erase(0, fim_cabeca + tamanho);
        return resposta;
    }

    // true se o servidor fechou a conexao (EOF) sem enviar mais nada.
    bool fechada() {
        return buffer.empty() && !ler_mais();
    }

private:
    bool ler_mais() {
        std::array<char, 4096> tmp;
        ssize_t n = socket.recv_some(tmp);
        if (n <= 0) return false;
        buffer.append(tmp.data(), static_cast<std::size_t>(n));
        return true;
    }

    Socket& socket;
    std::string buffer;
};

} // namespace

TEST(ServerKeepAliveTest, DuasRequisicoesNaMesmaConexao) {
    ServidorEmExecucao exec;
    Socket cliente = conectar(exec.server.port());
    LeitorDeRespostas leitor{cliente};

    ASSERT_TRUE(cliente.send_all(kGetIndex));
    std::string primeira = leitor.proxima();
    ASSERT_TRUE(cliente.send_all(kGetIndex)); // mesma conexao: se o servidor tivesse fechado, falharia
    std::string segunda = leitor.proxima();

    EXPECT_TRUE(comeca_com(primeira, "HTTP/1.1 200 OK\r\n")) << primeira;
    EXPECT_NE(primeira.find("Connection: keep-alive\r\n"), std::string::npos);
    EXPECT_EQ(segunda, primeira);
}

TEST(ServerKeepAliveTest, RequisicoesEncadeadasNumUnicoEnvio) {
    ServidorEmExecucao exec;
    registrar_eco(exec.server);
    Socket cliente = conectar(exec.server.port());
    LeitorDeRespostas leitor{cliente};

    // 3 requisicoes coladas (pipelining): a sobra de cada uma e o comeco da proxima
    std::string post = "POST /eco HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\n\r\nabc";
    ASSERT_TRUE(cliente.send_all(post + kGetIndex + post));

    std::string r1 = leitor.proxima();
    std::string r2 = leitor.proxima();
    std::string r3 = leitor.proxima();

    EXPECT_TRUE(r1.ends_with("\r\n\r\nabc")) << r1;
    EXPECT_TRUE(r2.ends_with("\r\n\r\nHello Citius!")) << r2;
    EXPECT_TRUE(r3.ends_with("\r\n\r\nabc")) << r3;
}

TEST(ServerKeepAliveTest, ConnectionCloseFechaDepoisDaResposta) {
    ServidorEmExecucao exec;
    Socket cliente = conectar(exec.server.port());
    LeitorDeRespostas leitor{cliente};

    ASSERT_TRUE(cliente.send_all("GET /index.html HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"));
    std::string resposta = leitor.proxima();

    EXPECT_TRUE(comeca_com(resposta, "HTTP/1.1 200 OK\r\n")) << resposta;
    EXPECT_NE(resposta.find("Connection: close\r\n"), std::string::npos);
    EXPECT_TRUE(leitor.fechada());
}

TEST(ServerKeepAliveTest, Http10FechaPorPadrao) {
    ServidorEmExecucao exec;
    Socket cliente = conectar(exec.server.port());
    LeitorDeRespostas leitor{cliente};

    ASSERT_TRUE(cliente.send_all("GET /index.html HTTP/1.0\r\n\r\n"));
    std::string resposta = leitor.proxima();

    EXPECT_NE(resposta.find("Connection: close\r\n"), std::string::npos) << resposta;
    EXPECT_TRUE(leitor.fechada());
}

TEST(ServerKeepAliveTest, Http10ComKeepAliveContinuaAberta) {
    ServidorEmExecucao exec;
    Socket cliente = conectar(exec.server.port());
    LeitorDeRespostas leitor{cliente};
    const std::string requisicao = "GET /index.html HTTP/1.0\r\nConnection: keep-alive\r\n\r\n";

    ASSERT_TRUE(cliente.send_all(requisicao));
    std::string primeira = leitor.proxima();
    ASSERT_TRUE(cliente.send_all(requisicao));
    std::string segunda = leitor.proxima();

    EXPECT_NE(primeira.find("Connection: keep-alive\r\n"), std::string::npos) << primeira;
    EXPECT_EQ(segunda, primeira);
}

TEST(ServerKeepAliveTest, RequisicaoInvalidaFechaAConexao) {
    ServidorEmExecucao exec;
    Socket cliente = conectar(exec.server.port());
    LeitorDeRespostas leitor{cliente};

    // depois de um erro nao sabemos mais onde a proxima requisicao comeca: fechar e o seguro
    ASSERT_TRUE(cliente.send_all("LIXO\r\n\r\n"));
    std::string resposta = leitor.proxima();

    EXPECT_TRUE(comeca_com(resposta, "HTTP/1.1 400 Bad Request\r\n")) << resposta;
    EXPECT_NE(resposta.find("Connection: close\r\n"), std::string::npos);
    EXPECT_TRUE(leitor.fechada());
}

TEST(ServerKeepAliveTest, ErroNaSegundaRequisicaoNaoAfetaAPrimeira) {
    ServidorEmExecucao exec;
    Socket cliente = conectar(exec.server.port());
    LeitorDeRespostas leitor{cliente};

    ASSERT_TRUE(cliente.send_all(kGetIndex));
    std::string ok = leitor.proxima();
    ASSERT_TRUE(cliente.send_all("GET /nao-existe HTTP/1.1\r\nHost: x\r\n\r\n"));
    std::string nao_achou = leitor.proxima();
    ASSERT_TRUE(cliente.send_all(kGetIndex)); // 404 e resposta normal: a conexao segue viva
    std::string de_novo = leitor.proxima();

    EXPECT_TRUE(comeca_com(ok, "HTTP/1.1 200")) << ok;
    EXPECT_TRUE(comeca_com(nao_achou, "HTTP/1.1 404")) << nao_achou;
    EXPECT_EQ(de_novo, ok);
}

TEST(ServerKeepAliveTest, ConexaoOciosaFechaEmSilencio) {
    ServidorEmExecucao exec;
    exec.server.set_idle_timeout(std::chrono::milliseconds(150));
    Socket cliente = conectar(exec.server.port());
    LeitorDeRespostas leitor{cliente};

    ASSERT_TRUE(cliente.send_all(kGetIndex));
    ASSERT_FALSE(leitor.proxima().empty());

    // nao envia a segunda: o servidor fecha sem mandar 408 (nao havia requisicao a responder)
    EXPECT_TRUE(leitor.fechada());
}

TEST(ServerKeepAliveTest, OciosidadeLiberaAUnicaVagaDeConexaoParaOutroCliente) {
    ServidorEmExecucao exec{1};
    exec.server.set_idle_timeout(std::chrono::milliseconds(150));
    Socket ocioso = conectar(exec.server.port());
    LeitorDeRespostas leitor{ocioso};
    ASSERT_TRUE(ocioso.send_all(kGetIndex));
    ASSERT_FALSE(leitor.proxima().empty());

    // a unica vaga esta com o 'ocioso' ate o prazo de ociosidade
    esperar(400);
    std::string resposta = requisitar_em(exec.server.port(), kGetIndex);

    EXPECT_TRUE(comeca_com(resposta, "HTTP/1.1 200 OK\r\n")) << resposta;
}

TEST(ServerKeepAliveTest, ConexaoFechaNoLimiteDeRequisicoes) {
    ServidorEmExecucao exec;
    Socket cliente = conectar(exec.server.port());
    LeitorDeRespostas leitor{cliente};

    for (std::size_t i = 1; i <= citius::kMaxRequestsPerConnection; ++i) {
        ASSERT_TRUE(cliente.send_all(kGetIndex));
        std::string resposta = leitor.proxima();
        bool ultima = i == citius::kMaxRequestsPerConnection;
        EXPECT_NE(resposta.find(ultima ? "Connection: close\r\n" : "Connection: keep-alive\r\n"), std::string::npos)
            << "requisicao " << i << ": " << resposta;
    }
    EXPECT_TRUE(leitor.fechada());
}

TEST(ServerKeepAliveTest, PedidoDeParadaFechaConexoesOciosas) {
    std::optional<ServidorEmExecucao> exec;
    exec.emplace();
    Socket cliente = conectar(exec->server.port()); // continua aberto durante a parada
    LeitorDeRespostas leitor{cliente};
    ASSERT_TRUE(cliente.send_all(kGetIndex));
    ASSERT_FALSE(leitor.proxima().empty());

    auto inicio = std::chrono::steady_clock::now();
    exec.reset(); // request_stop + join: sem o poll em fatias, esperaria o prazo de ociosidade (5 s)

    EXPECT_LT(std::chrono::steady_clock::now() - inicio, std::chrono::seconds(2));
}
