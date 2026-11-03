#include "citius/server/Connection.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

using citius::Connection;
using citius::ConnectionOptions;
using citius::HttpRequest;
using citius::HttpResponse;
using citius::Interest;
using citius::Socket;
using namespace std::chrono_literals;

namespace {

using Clock = Connection::Clock;

constexpr std::string_view kGet = "GET /a HTTP/1.1\r\nHost: x\r\n\r\n";

// Prazos curtos e distintos para os testes: nenhum espera de verdade, o tempo e passado a mao
ConnectionOptions test_options() {
    ConnectionOptions options;
    options.read_timeout = 1000ms;
    options.idle_timeout = 500ms;
    options.write_timeout = 2000ms;
    options.linger_timeout = 500ms;
    return options;
}

bool contains(const std::string& text, std::string_view part) {
    return text.find(part) != std::string::npos;
}

std::size_t count(const std::string& text, std::string_view part) {
    std::size_t n = 0;
    for (std::size_t pos = text.find(part); pos != std::string::npos; pos = text.find(part, pos + 1)) ++n;
    return n;
}

// A "outra ponta" do cliente: le o que a Connection ja escreveu, sem bloquear.
std::string drain(Socket& peer) {
    std::string out;
    std::array<char, 65536> buffer;
    ssize_t n;
    while ((n = peer.recv_some(buffer)) > 0) out.append(buffer.data(), static_cast<std::size_t>(n));
    return out;
}

struct Fixture {
    Socket peer;
    std::vector<HttpRequest> seen; ///< requisicoes que chegaram ao handler
    std::unique_ptr<Connection> conn;
    Clock::time_point t0 = Clock::now();

    explicit Fixture(Connection::Handler handler = {}, ConnectionOptions options = test_options()) {
        int fds[2];
        EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
        peer.reset(fds[0]);
        EXPECT_TRUE(peer.set_nonblocking());
        if (!handler) {
            handler = [this](const HttpRequest& request) {
                seen.push_back(request);
                HttpResponse response;
                response.set_body("ok");
                return response;
            };
        }
        conn = std::make_unique<Connection>(Socket{fds[1]}, std::move(handler), t0, options);
    }

    void send(std::string_view data) {
        ASSERT_TRUE(peer.send_all(data));
    }
};

// Uma conexao que vai fechar passa por lingering e so libera o fd quando o cliente fecha o lado dele.
void cliente_fecha(Fixture& f) {
    f.peer.reset();
    f.conn->on_readable(f.t0);
}

} // namespace

TEST(ConnectionTest, RespondeEMantemAConexaoAberta) {
    Fixture f;
    f.send(kGet);

    f.conn->on_readable(f.t0);
    const std::string response = drain(f.peer);

    EXPECT_TRUE(contains(response, "HTTP/1.1 200 OK"));
    EXPECT_TRUE(contains(response, "Connection: keep-alive"));
    EXPECT_TRUE(contains(response, "\r\n\r\nok"));
    EXPECT_FALSE(f.conn->closed());
    EXPECT_EQ(f.conn->interest(), Interest::READ);
    ASSERT_EQ(f.seen.size(), 1u);
    EXPECT_EQ(f.seen[0].path, "/a");
}

TEST(ConnectionTest, ConnectionCloseFechaDepoisDeResponder) {
    Fixture f;
    f.send("GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");

    f.conn->on_readable(f.t0);
    const std::string response = drain(f.peer);

    EXPECT_TRUE(contains(response, "200 OK"));
    EXPECT_TRUE(contains(response, "Connection: close"));
    EXPECT_TRUE(f.conn->lingering()); // fechou a resposta e espera o cliente fechar
    cliente_fecha(f);
    EXPECT_TRUE(f.conn->closed());
}

TEST(ConnectionTest, Http10SemKeepAliveFecha) {
    Fixture f;
    f.send("GET / HTTP/1.0\r\nHost: x\r\n\r\n");

    f.conn->on_readable(f.t0);

    EXPECT_TRUE(contains(drain(f.peer), "Connection: close"));
    EXPECT_TRUE(f.conn->lingering()); // fechou a resposta e espera o cliente fechar
    cliente_fecha(f);
    EXPECT_TRUE(f.conn->closed());
}

TEST(ConnectionTest, RequisicaoChegandoEmDoisPedacos) {
    Fixture f;
    f.send("GET /a HTTP/1.1\r\nHo");
    f.conn->on_readable(f.t0);
    EXPECT_EQ(drain(f.peer), "");
    EXPECT_TRUE(f.seen.empty());

    f.send("st: x\r\n\r\n");
    f.conn->on_readable(f.t0);

    EXPECT_TRUE(contains(drain(f.peer), "200 OK"));
    EXPECT_EQ(f.seen.size(), 1u);
}

TEST(ConnectionTest, SemDadosNaoFazNada) {
    Fixture f;

    f.conn->on_readable(f.t0); // recv devolve EAGAIN

    EXPECT_FALSE(f.conn->closed());
    EXPECT_EQ(drain(f.peer), "");
}

TEST(ConnectionTest, HandlerRecebeOCorpo) {
    Fixture f;
    f.send("POST /p HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhello");

    f.conn->on_readable(f.t0);

    ASSERT_EQ(f.seen.size(), 1u);
    EXPECT_EQ(f.seen[0].body, "hello");
}

TEST(ConnectionTest, RequisicoesEncadeadasSaemEmOrdem) {
    Fixture f;
    f.send(std::string(kGet) + "GET /b HTTP/1.1\r\nHost: x\r\n\r\n");

    f.conn->on_readable(f.t0); // uma leitura traz as duas
    const std::string response = drain(f.peer);

    EXPECT_EQ(count(response, "HTTP/1.1 200 OK"), 2u);
    ASSERT_EQ(f.seen.size(), 2u);
    EXPECT_EQ(f.seen[0].path, "/a");
    EXPECT_EQ(f.seen[1].path, "/b");
    EXPECT_FALSE(f.conn->closed());
}

TEST(ConnectionTest, HandlerQueLancaVira500SemFecharAConexao) {
    Fixture f([](const HttpRequest&) -> HttpResponse { throw std::runtime_error("bug"); });
    f.send(kGet);

    f.conn->on_readable(f.t0);
    const std::string response = drain(f.peer);

    EXPECT_TRUE(contains(response, "500"));
    EXPECT_FALSE(contains(response, "bug")); // a mensagem da excecao nao vaza para o cliente
    EXPECT_TRUE(contains(response, "Connection: keep-alive"));
    EXPECT_FALSE(f.conn->closed());
}

TEST(ConnectionTest, LimiteDeRequisicoesPorConexao) {
    ConnectionOptions options = test_options();
    options.max_requests = 2;
    Fixture f({}, options);

    f.send(kGet);
    f.conn->on_readable(f.t0);
    EXPECT_TRUE(contains(drain(f.peer), "Connection: keep-alive"));
    ASSERT_FALSE(f.conn->closed());

    f.send(kGet);
    f.conn->on_readable(f.t0);
    EXPECT_TRUE(contains(drain(f.peer), "Connection: close"));
    EXPECT_TRUE(f.conn->lingering()); // fechou a resposta e espera o cliente fechar
    cliente_fecha(f);
    EXPECT_TRUE(f.conn->closed());
}

// ---- erros do cliente ------------------------------------------------------------------------

TEST(ConnectionTest, RequisicaoInvalidaDa400EFecha) {
    Fixture f;
    f.send("LIXO\r\n\r\n");

    f.conn->on_readable(f.t0);
    const std::string response = drain(f.peer);

    EXPECT_TRUE(contains(response, "400"));
    EXPECT_TRUE(contains(response, "Connection: close"));
    EXPECT_TRUE(f.conn->lingering()); // fechou a resposta e espera o cliente fechar
    cliente_fecha(f);
    EXPECT_TRUE(f.conn->closed());
    EXPECT_TRUE(f.seen.empty());
}

TEST(ConnectionTest, CabecaGrandeDemaisDa431EFecha) {
    ConnectionOptions options = test_options();
    options.max_head_bytes = 100;
    Fixture f({}, options);
    f.send(std::string(200, 'a'));

    f.conn->on_readable(f.t0);

    EXPECT_TRUE(contains(drain(f.peer), "431"));
    EXPECT_TRUE(f.conn->lingering()); // fechou a resposta e espera o cliente fechar
    cliente_fecha(f);
    EXPECT_TRUE(f.conn->closed());
}

TEST(ConnectionTest, ErroNaSegundaRequisicaoEncadeadaFechaDepoisDaPrimeira) {
    Fixture f;
    f.send(std::string(kGet) + "LIXO\r\n\r\n");

    f.conn->on_readable(f.t0);
    const std::string response = drain(f.peer);

    EXPECT_TRUE(contains(response, "200 OK"));
    EXPECT_TRUE(contains(response, "400"));
    EXPECT_TRUE(f.conn->lingering()); // fechou a resposta e espera o cliente fechar
    cliente_fecha(f);
    EXPECT_TRUE(f.conn->closed());
}

TEST(ConnectionTest, ParFechandoDepoisDeResponderFechaAConexao) {
    Fixture f;
    f.send(kGet);
    f.conn->on_readable(f.t0);
    (void)drain(f.peer);

    f.peer.reset();
    f.conn->on_readable(f.t0); // recv devolve 0

    EXPECT_TRUE(f.conn->closed());
}

TEST(ConnectionTest, ParFechandoNoMeioDaRequisicaoFechaSemResponder) {
    Fixture f;
    f.send("GET /");
    f.conn->on_readable(f.t0);
    f.peer.reset();

    f.conn->on_readable(f.t0);

    EXPECT_TRUE(f.conn->closed());
    EXPECT_TRUE(f.seen.empty());
}

// ---- escrita parcial -------------------------------------------------------------------------

namespace {

Connection::Handler big_body_handler(std::size_t size) {
    return [size](const HttpRequest&) {
        HttpResponse response;
        response.set_body(std::string(size, 'x'));
        return response;
    };
}

// O cliente le, a conexao escreve mais, ate acabar. @return tudo que o cliente recebeu.
std::string pump(Fixture& f, Clock::time_point now) {
    std::string received = drain(f.peer);
    for (int guard = 0; guard < 10000 && !f.conn->closed() && f.conn->interest() == Interest::WRITE; ++guard) {
        f.conn->on_writable(now);
        received += drain(f.peer);
    }
    return received;
}

} // namespace

TEST(ConnectionTest, RespostaGrandeSaiEmPartesEPedeParaSerAvisadaDaEscrita) {
    constexpr std::size_t kSize = 4 * 1024 * 1024; // bem maior que o buffer do socket
    Fixture f(big_body_handler(kSize));
    f.send(kGet);

    f.conn->on_readable(f.t0);

    ASSERT_FALSE(f.conn->closed());
    EXPECT_EQ(f.conn->interest(), Interest::WRITE); // o buffer do kernel encheu: sobrou resposta

    const std::string received = pump(f, f.t0);
    EXPECT_GT(received.size(), kSize);
    EXPECT_EQ(received.substr(received.size() - kSize), std::string(kSize, 'x'));
    EXPECT_EQ(f.conn->interest(), Interest::READ);
    EXPECT_FALSE(f.conn->closed());
}

TEST(ConnectionTest, NaoProcessaAProximaRequisicaoEnquantoARespostaNaoSaiu) {
    Fixture f(big_body_handler(4 * 1024 * 1024));
    f.send(std::string(kGet) + "GET /b HTTP/1.1\r\nHost: x\r\n\r\n");

    f.conn->on_readable(f.t0);
    ASSERT_EQ(f.conn->interest(), Interest::WRITE);
    EXPECT_EQ(count(drain(f.peer), "HTTP/1.1 200 OK"), 1u); // so a primeira comecou a sair

    const std::string rest = pump(f, f.t0);

    EXPECT_EQ(count(rest, "HTTP/1.1 200 OK"), 1u); // a segunda sai depois que a primeira acaba
}

TEST(ConnectionTest, ClienteQueNaoLeEstouraOPrazoDeEscrita) {
    Fixture f(big_body_handler(4 * 1024 * 1024));
    f.send(kGet);
    f.conn->on_readable(f.t0);
    ASSERT_EQ(f.conn->interest(), Interest::WRITE);

    // write_timeout + tempo para o cliente "ler" a resposta na velocidade minima (kMinWriteBytesPerSecond)
    const auto limite =
        f.t0 + 2000ms + std::chrono::milliseconds(4ull * 1024 * 1024 * 1000 / citius::kMinWriteBytesPerSecond);
    f.conn->check_deadline(limite - 1s); // a cabeca (~100 bytes) a mais cabe nessa folga de 1 s
    EXPECT_FALSE(f.conn->closed());

    f.conn->check_deadline(limite + 1s);
    EXPECT_TRUE(f.conn->closed());
}

TEST(ConnectionTest, RespostaPequenaTemSoOPrazoBaseDeEscrita) {
    // abaixo do limite do buffer do socket a resposta sai de uma vez: o prazo de escrita nem chega a valer
    Fixture f;
    f.send(kGet);
    f.conn->on_readable(f.t0);

    ASSERT_TRUE(f.conn->deadline().has_value());
    EXPECT_EQ(*f.conn->deadline(), f.t0 + 500ms); // ja voltou a READING: ociosidade
}

// ---- prazos ----------------------------------------------------------------------------------

TEST(ConnectionTest, PrazoInicialEORead_timeoutDaPrimeiraRequisicao) {
    Fixture f;

    ASSERT_TRUE(f.conn->deadline().has_value());
    EXPECT_EQ(*f.conn->deadline(), f.t0 + 1000ms);
}

TEST(ConnectionTest, PrimeiraRequisicaoSemNadaDa408) {
    Fixture f;

    f.conn->check_deadline(f.t0 + 999ms);
    EXPECT_FALSE(f.conn->closed());

    f.conn->check_deadline(f.t0 + 1000ms);
    EXPECT_TRUE(contains(drain(f.peer), "408"));
    EXPECT_TRUE(f.conn->lingering()); // fechou a resposta e espera o cliente fechar
    cliente_fecha(f);
    EXPECT_TRUE(f.conn->closed());
}

TEST(ConnectionTest, SlowlorisNaoRenovaOPrazoComUmByteDeVezEmQuando) {
    Fixture f;

    f.send("GET / HT");
    f.conn->on_readable(f.t0 + 900ms);
    f.send("TP/1.1\r\n");
    f.conn->on_readable(f.t0 + 999ms); // chegou dado, mas a requisicao continua incompleta

    f.conn->check_deadline(f.t0 + 1000ms); // o prazo e total, nao por leitura

    EXPECT_TRUE(contains(drain(f.peer), "408"));
    EXPECT_TRUE(f.conn->lingering()); // fechou a resposta e espera o cliente fechar
    cliente_fecha(f);
    EXPECT_TRUE(f.conn->closed());
}

TEST(ConnectionTest, CorpoIncompletoEstouraOPrazoDa408) {
    Fixture f;
    f.send("POST /p HTTP/1.1\r\nHost: x\r\nContent-Length: 10\r\n\r\nabc");
    f.conn->on_readable(f.t0);

    f.conn->check_deadline(f.t0 + 1000ms);

    EXPECT_TRUE(contains(drain(f.peer), "408"));
    EXPECT_TRUE(f.conn->lingering()); // fechou a resposta e espera o cliente fechar
    cliente_fecha(f);
    EXPECT_TRUE(f.conn->closed());
    EXPECT_TRUE(f.seen.empty());
}

TEST(ConnectionTest, DepoisDeResponderOPrazoPassaAOciosidade) {
    Fixture f;
    f.send(kGet);
    f.conn->on_readable(f.t0 + 100ms);
    (void)drain(f.peer);

    ASSERT_TRUE(f.conn->deadline().has_value());
    EXPECT_EQ(*f.conn->deadline(), f.t0 + 100ms + 500ms); // idle_timeout
}

TEST(ConnectionTest, OciosidadeFechaEmSilencioSemMandar408) {
    Fixture f;
    f.send(kGet);
    f.conn->on_readable(f.t0);
    (void)drain(f.peer);

    f.conn->check_deadline(f.t0 + 499ms);
    EXPECT_FALSE(f.conn->closed());

    f.conn->check_deadline(f.t0 + 500ms);
    EXPECT_TRUE(f.conn->closed());
    EXPECT_EQ(drain(f.peer), ""); // nao ha requisicao a responder
}

TEST(ConnectionTest, PrazoDaSegundaRequisicaoComecaNoPrimeiroByteDela) {
    Fixture f;
    f.send(kGet);
    f.conn->on_readable(f.t0);
    (void)drain(f.peer);

    f.send("GET /b HT"); // chegou 400 ms depois: ainda dentro da ociosidade
    f.conn->on_readable(f.t0 + 400ms);

    ASSERT_TRUE(f.conn->deadline().has_value());
    EXPECT_EQ(*f.conn->deadline(), f.t0 + 400ms + 1000ms); // agora vale o read_timeout

    f.conn->check_deadline(f.t0 + 500ms); // o prazo de ociosidade antigo ja nao vale
    EXPECT_FALSE(f.conn->closed());

    f.conn->check_deadline(f.t0 + 1400ms);
    EXPECT_TRUE(contains(drain(f.peer), "408"));
    EXPECT_TRUE(f.conn->lingering()); // fechou a resposta e espera o cliente fechar
    cliente_fecha(f);
    EXPECT_TRUE(f.conn->closed());
}

TEST(ConnectionTest, FechadaNaoTemPrazo) {
    Fixture f;
    f.send("LIXO\r\n\r\n");
    f.conn->on_readable(f.t0);
    ASSERT_TRUE(f.conn->lingering());
    EXPECT_TRUE(f.conn->deadline().has_value()); // em lingering ainda ha prazo (linger_timeout)

    cliente_fecha(f);
    ASSERT_TRUE(f.conn->closed());
    EXPECT_FALSE(f.conn->deadline().has_value());
}

// ---- encerramento ----------------------------------------------------------------------------

TEST(ConnectionTest, CloseWhenIdleFechaConexaoParada) {
    Fixture f;
    f.send(kGet);
    f.conn->on_readable(f.t0);
    (void)drain(f.peer);

    f.conn->close_when_idle();

    EXPECT_TRUE(f.conn->closed());
}

TEST(ConnectionTest, CloseWhenIdleDeixaTerminarARequisicaoEmAndamento) {
    Fixture f;
    f.send("GET /a HTTP/1.1\r\nHo");
    f.conn->on_readable(f.t0);

    f.conn->close_when_idle();
    EXPECT_FALSE(f.conn->closed()); // ha uma requisicao pela metade: deixa terminar

    f.send("st: x\r\n\r\n");
    f.conn->on_readable(f.t0);

    const std::string response = drain(f.peer);
    EXPECT_TRUE(contains(response, "200 OK"));
    EXPECT_TRUE(contains(response, "Connection: close")); // e avisa que vai fechar
    EXPECT_TRUE(f.conn->lingering());                     // fechou a resposta e espera o cliente fechar
    cliente_fecha(f);
    EXPECT_TRUE(f.conn->closed());
}

TEST(ConnectionTest, SocketInvalidoNasceFechada) {
    Connection conn{Socket{}, [](const HttpRequest&) { return HttpResponse{}; }, Clock::now()};

    EXPECT_TRUE(conn.closed());
}

// ---- lingering close -------------------------------------------------------------------------
// Depois de uma resposta que fecha a conexao, o servidor envia o FIN (shutdown de escrita) e continua lendo e
// descartando ate o cliente fechar: fechar de uma vez com dados do cliente ainda por ler causaria um RST, que pode
// apagar a resposta antes de o cliente le-la.

TEST(ConnectionTest, ConnectionCloseEnviaFinMasSoLiberaOFdQuandoOClienteFecha) {
    Fixture f;
    f.send("GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");

    f.conn->on_readable(f.t0);

    // o cliente le a resposta inteira e depois o fim da conexao
    std::string response;
    std::array<char, 4096> buffer;
    ssize_t n;
    while ((n = f.peer.recv_some(buffer)) > 0) response.append(buffer.data(), static_cast<std::size_t>(n));
    EXPECT_EQ(n, 0) << "o cliente deveria ver o FIN logo depois da resposta";
    EXPECT_TRUE(contains(response, "200 OK"));

    EXPECT_FALSE(f.conn->closed());
    EXPECT_TRUE(f.conn->lingering());
    EXPECT_EQ(f.conn->interest(), Interest::READ);

    f.peer.reset(); // o cliente fecha o lado dele
    f.conn->on_readable(f.t0);
    EXPECT_TRUE(f.conn->closed());
}

TEST(ConnectionTest, LingeringDescartaOQueOClienteAindaMandaSemResponder) {
    Fixture f;
    f.send("LIXO\r\n\r\n"); // 400 e fecha
    f.conn->on_readable(f.t0);
    ASSERT_TRUE(f.conn->lingering());
    ASSERT_TRUE(contains(drain(f.peer), "400"));

    f.send(std::string(1000, 'x')); // o cliente ainda nao percebeu e continua enviando
    f.conn->on_readable(f.t0);

    EXPECT_TRUE(f.conn->lingering());
    EXPECT_EQ(drain(f.peer), ""); // nada de segunda resposta
    EXPECT_TRUE(f.seen.empty());
}

TEST(ConnectionTest, LingeringFechaQuandoOPrazoAcaba) {
    Fixture f;
    f.send("LIXO\r\n\r\n");
    f.conn->on_readable(f.t0);
    ASSERT_TRUE(f.conn->lingering());

    ASSERT_TRUE(f.conn->deadline().has_value());
    EXPECT_EQ(*f.conn->deadline(), f.t0 + 500ms); // linger_timeout

    f.conn->check_deadline(f.t0 + 499ms);
    EXPECT_FALSE(f.conn->closed());
    f.conn->check_deadline(f.t0 + 500ms);
    EXPECT_TRUE(f.conn->closed());
}

TEST(ConnectionTest, LingeringFechaSeOClienteMandaDemais) {
    Fixture f;
    f.send("LIXO\r\n\r\n");
    f.conn->on_readable(f.t0);
    ASSERT_TRUE(f.conn->lingering());
    (void)drain(f.peer);

    // um cliente que nunca para de enviar nao pode manter a conexao (e a banda) para sempre
    const std::string bloco(16 * 1024, 'x');
    std::size_t enviado = 0;
    while (!f.conn->closed() && enviado < 4 * citius::kMaxLingerBytes) {
        const ssize_t n = f.peer.send_some(bloco);
        if (n > 0) enviado += static_cast<std::size_t>(n);
        f.conn->on_readable(f.t0);
    }

    EXPECT_TRUE(f.conn->closed());
    EXPECT_LT(enviado, 4 * citius::kMaxLingerBytes);
}

TEST(ConnectionTest, Timeout408TambemFazLingering) {
    Fixture f;

    f.conn->check_deadline(f.t0 + 1000ms);

    EXPECT_TRUE(contains(drain(f.peer), "408"));
    EXPECT_TRUE(f.conn->lingering());
    EXPECT_FALSE(f.conn->closed());
}

TEST(ConnectionTest, CloseWhenIdleEncerraAConexaoEmLingering) {
    Fixture f;
    f.send("LIXO\r\n\r\n");
    f.conn->on_readable(f.t0);
    ASSERT_TRUE(f.conn->lingering());

    f.conn->close_when_idle(); // parada do servidor: ninguem a esperar

    EXPECT_TRUE(f.conn->closed());
}

TEST(ConnectionTest, FimPorOciosidadeFechaDeUmaVezSemLingering) {
    Fixture f;
    f.send(kGet);
    f.conn->on_readable(f.t0);
    (void)drain(f.peer);

    f.conn->check_deadline(f.t0 + 500ms); // idle_timeout: nao ha resposta a proteger

    EXPECT_TRUE(f.conn->closed());
}

// ---- corpo chunked e Expect: 100-continue -----------------------------------------------------------------------

TEST(ConnectionTest, ChunkedChegaAoHandlerDecodificado) {
    Fixture f;
    f.send("POST /p HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n");

    f.conn->on_readable(f.t0);

    ASSERT_EQ(f.seen.size(), 1u);
    EXPECT_EQ(f.seen[0].body, "hello");
    EXPECT_EQ(f.seen[0].content_length, 5u);
    EXPECT_TRUE(contains(drain(f.peer), "200 OK"));
}

TEST(ConnectionTest, ChunkedChegandoAosPoucosEPipelining) {
    Fixture f;
    f.send("POST /p HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nab");
    f.conn->on_readable(f.t0);
    EXPECT_TRUE(f.seen.empty());

    f.send("c\r\n0\r\n\r\nGET /b HTTP/1.1\r\nHost: x\r\n\r\n"); // o resto do corpo e ja a proxima requisicao
    f.conn->on_readable(f.t0);

    ASSERT_EQ(f.seen.size(), 2u);
    EXPECT_EQ(f.seen[0].body, "abc");
    EXPECT_EQ(f.seen[1].path, "/b");
    EXPECT_EQ(count(drain(f.peer), "HTTP/1.1 200 OK"), 2u);
}

TEST(ConnectionTest, ChunkedInvalidoDa400EFecha) {
    Fixture f;
    f.send("POST /p HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\nzz\r\n");

    f.conn->on_readable(f.t0);

    EXPECT_TRUE(contains(drain(f.peer), "400"));
    EXPECT_TRUE(f.seen.empty());
    EXPECT_TRUE(f.conn->lingering());
}

TEST(ConnectionTest, ChunkedIncompletoEstouraOPrazoDa408) {
    Fixture f;
    f.send("POST /p HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhel");
    f.conn->on_readable(f.t0);

    f.conn->check_deadline(f.t0 + 1000ms);

    EXPECT_TRUE(contains(drain(f.peer), "408"));
    EXPECT_TRUE(f.seen.empty());
}

TEST(ConnectionTest, Expect100ContinueRecebeOInterinoAntesDoCorpo) {
    Fixture f;
    f.send("POST /p HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nExpect: 100-continue\r\n\r\n");

    f.conn->on_readable(f.t0);

    // so o interino: o handler ainda nao foi chamado e nao ha resposta final
    EXPECT_EQ(drain(f.peer), "HTTP/1.1 100 Continue\r\n\r\n");
    EXPECT_TRUE(f.seen.empty());
    EXPECT_FALSE(f.conn->closed());

    f.send("hello");
    f.conn->on_readable(f.t0);

    const std::string response = drain(f.peer);
    EXPECT_TRUE(contains(response, "200 OK"));
    EXPECT_FALSE(contains(response, "100 Continue")); // o interino sai uma vez so
    ASSERT_EQ(f.seen.size(), 1u);
    EXPECT_EQ(f.seen[0].body, "hello");
}

TEST(ConnectionTest, Expect100ContinueUmaVezSoMesmoComCorpoEmPedacos) {
    Fixture f;
    f.send("POST /p HTTP/1.1\r\nHost: x\r\nContent-Length: 6\r\nExpect: 100-continue\r\n\r\n");
    f.conn->on_readable(f.t0);
    ASSERT_EQ(drain(f.peer), "HTTP/1.1 100 Continue\r\n\r\n");

    f.send("abc");
    f.conn->on_readable(f.t0);
    EXPECT_EQ(drain(f.peer), "");
    f.send("def");
    f.conn->on_readable(f.t0);

    EXPECT_TRUE(contains(drain(f.peer), "200 OK"));
}

TEST(ConnectionTest, ExpectComCorpoJaPresenteNaoManda100) {
    Fixture f;
    f.send("POST /p HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nExpect: 100-continue\r\n\r\nhello");

    f.conn->on_readable(f.t0);

    const std::string response = drain(f.peer);
    EXPECT_TRUE(contains(response, "200 OK"));
    EXPECT_FALSE(contains(response, "100 Continue"));
}

TEST(ConnectionTest, ExpectComChunkedTambemRecebeOInterino) {
    Fixture f;
    f.send("POST /p HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nExpect: 100-continue\r\n\r\n");
    f.conn->on_readable(f.t0);
    EXPECT_EQ(drain(f.peer), "HTTP/1.1 100 Continue\r\n\r\n");

    f.send("2\r\nhi\r\n0\r\n\r\n");
    f.conn->on_readable(f.t0);

    ASSERT_EQ(f.seen.size(), 1u);
    EXPECT_EQ(f.seen[0].body, "hi");
}

TEST(ConnectionTest, ExpectDesconhecidoDa417EFecha) {
    Fixture f;
    f.send("POST /p HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nExpect: algo-estranho\r\n\r\n");

    f.conn->on_readable(f.t0);

    const std::string response = drain(f.peer);
    EXPECT_TRUE(contains(response, "417 Expectation Failed"));
    EXPECT_FALSE(contains(response, "100 Continue"));
    EXPECT_TRUE(f.conn->lingering());
}

TEST(ConnectionTest, CorpoGrandeDemaisComExpectDa413SemInterino) {
    Fixture f;
    f.send("POST /p HTTP/1.1\r\nHost: x\r\nContent-Length: 2097152\r\nExpect: 100-continue\r\n\r\n");

    f.conn->on_readable(f.t0);

    const std::string response = drain(f.peer);
    EXPECT_TRUE(contains(response, "413"));
    EXPECT_FALSE(contains(response, "100 Continue")); // nao convida o cliente a enviar o que vai ser recusado
}

// ---- corpo de arquivo (streaming) -------------------------------------------------------------------------------

namespace {

// Arquivo temporario com um padrao previsivel (nao so um byte repetido: um deslocamento errado seria detectado).
class ArquivoTemporario {
public:
    explicit ArquivoTemporario(std::size_t tamanho)
        : m_path(std::filesystem::temp_directory_path() /
                 ("citius_conn_" + std::to_string(::getpid()) + "_" +
                  ::testing::UnitTest::GetInstance()->current_test_info()->name())) {
        std::string dados(tamanho, '\0');
        for (std::size_t i = 0; i < tamanho; ++i) dados[i] = static_cast<char>(i % 251);
        std::ofstream{m_path, std::ios::binary} << dados;
        m_conteudo = std::move(dados);
    }
    ~ArquivoTemporario() {
        std::error_code ignorado;
        std::filesystem::remove(m_path, ignorado);
    }
    ArquivoTemporario(const ArquivoTemporario&) = delete;
    ArquivoTemporario& operator=(const ArquivoTemporario&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const {
        return m_path;
    }
    [[nodiscard]] const std::string& conteudo() const {
        return m_conteudo;
    }

    // Handler que responde com este arquivo como corpo de arquivo (promete @p prometido bytes).
    [[nodiscard]] Connection::Handler handler(std::size_t prometido) const {
        return [path = m_path, prometido](const HttpRequest&) {
            HttpResponse response;
            response.set_file_body(citius::FileDescriptor{::open(path.c_str(), O_RDONLY)}, prometido,
                                   "application/octet-stream");
            return response;
        };
    }

private:
    std::filesystem::path m_path;
    std::string m_conteudo;
};

} // namespace

TEST(ConnectionTest, ArquivoGrandeSaiEmPedacosEChegaIntegro) {
    constexpr std::size_t kTamanho = 3 * 1024 * 1024 + 123; // nao multiplo do pedaco de leitura
    ArquivoTemporario arquivo{kTamanho};
    Fixture f(arquivo.handler(kTamanho));
    f.send(kGet);

    f.conn->on_readable(f.t0);
    ASSERT_FALSE(f.conn->closed());
    EXPECT_EQ(f.conn->interest(), Interest::WRITE); // nao coube de uma vez

    const std::string recebido = pump(f, f.t0);

    const std::size_t fim_da_cabeca = recebido.find("\r\n\r\n") + 4;
    EXPECT_TRUE(contains(recebido.substr(0, fim_da_cabeca), "Content-Length: " + std::to_string(kTamanho) + "\r\n"));
    EXPECT_EQ(recebido.size() - fim_da_cabeca, kTamanho);
    EXPECT_EQ(recebido.substr(fim_da_cabeca), arquivo.conteudo()); // todos os bytes, na ordem certa
    EXPECT_EQ(f.conn->interest(), Interest::READ);
    EXPECT_FALSE(f.conn->closed()); // keep-alive continua valendo
}

TEST(ConnectionTest, ProximaRequisicaoEsperaOArquivoSair) {
    constexpr std::size_t kTamanho = 2 * 1024 * 1024;
    ArquivoTemporario arquivo{kTamanho};
    Fixture f(arquivo.handler(kTamanho));
    f.send(std::string(kGet) + std::string(kGet)); // duas requisicoes encadeadas

    f.conn->on_readable(f.t0);
    ASSERT_EQ(f.conn->interest(), Interest::WRITE);

    const std::string recebido = pump(f, f.t0);

    EXPECT_EQ(count(recebido, "HTTP/1.1 200 OK"), 2u);
    EXPECT_EQ(recebido.size(), 2 * (kTamanho + recebido.find("\r\n\r\n") + 4));
}

// Se o arquivo encolhe depois de o Content-Length ser enviado, nao ha como cumprir o que se prometeu: o unico
// comportamento correto e encerrar a conexao (o cliente ve o corpo curto e sabe que a resposta esta incompleta).
TEST(ConnectionTest, ArquivoQueEncolheDuranteOEnvioFechaAConexao) {
    constexpr std::size_t kTamanho = 1024 * 1024;
    ArquivoTemporario arquivo{kTamanho};
    Fixture f(arquivo.handler(kTamanho));
    ASSERT_EQ(::truncate(arquivo.path().c_str(), 10 * 1024), 0); // encolhe antes de o envio comecar
    f.send(kGet);

    f.conn->on_readable(f.t0);
    const std::string recebido = pump(f, f.t0);

    EXPECT_TRUE(f.conn->closed());
    EXPECT_LT(recebido.size(), kTamanho); // o cliente recebeu menos do que foi prometido
}

TEST(ConnectionTest, PrazoDeEscritaDeArquivoCresceComOTamanho) {
    constexpr std::size_t kTamanho = 3 * 1024 * 1024;
    ArquivoTemporario arquivo{kTamanho};
    Fixture f(arquivo.handler(kTamanho));
    f.send(kGet);
    f.conn->on_readable(f.t0); // o cliente nunca le

    ASSERT_EQ(f.conn->interest(), Interest::WRITE);
    const auto extra = std::chrono::milliseconds(kTamanho * 1000ull / citius::kMinWriteBytesPerSecond);
    ASSERT_TRUE(f.conn->deadline().has_value());
    EXPECT_GE(*f.conn->deadline(), f.t0 + 2000ms + extra);      // write_timeout + tempo na velocidade minima
    EXPECT_LT(*f.conn->deadline(), f.t0 + 2000ms + extra + 1s); // so a cabeca a mais

    f.conn->check_deadline(f.t0 + 2000ms + extra - 1ms);
    EXPECT_FALSE(f.conn->closed());
    f.conn->check_deadline(f.t0 + 2000ms + extra + 1s);
    EXPECT_TRUE(f.conn->closed());
}

TEST(ConnectionTest, FecharAConexaoNoMeioDoArquivoLiberaODescritor) {
    constexpr std::size_t kTamanho = 3 * 1024 * 1024;
    ArquivoTemporario arquivo{kTamanho};
    Fixture f(arquivo.handler(kTamanho));
    f.send(kGet);
    f.conn->on_readable(f.t0);
    ASSERT_EQ(f.conn->interest(), Interest::WRITE);

    f.peer.reset(); // o cliente some no meio do download
    f.conn->on_writable(f.t0);

    EXPECT_TRUE(f.conn->closed());
}
