#include "citius/HttpParser.hpp"
#include <gtest/gtest.h>
#include <string>

using citius::HttpMethod;
using citius::HttpParser;
using citius::ParseError;
using citius::ParseResult;

namespace {

// Monta uma cabeca de requisicao completa (termina em linha vazia).
std::string req(const std::string& linha, const std::string& cabecalhos = "Host: x\r\n") {
    return linha + "\r\n" + cabecalhos + "\r\n";
}

// Verifica que o parse falhou com o erro esperado.
void espera_erro(const std::string& raw, ParseError esperado) {
    ParseResult resultado = HttpParser::parse(raw);
    EXPECT_FALSE(resultado.ok()) << raw;
    ASSERT_TRUE(resultado.error.has_value()) << raw;
    EXPECT_EQ(*resultado.error, esperado) << raw;
}

} // namespace

// ---- linha de requisicao ----

TEST(HttpParserTest, ParseaRequisicaoGetValida) {
    ParseResult resultado = HttpParser::parse(req("GET /index.html HTTP/1.1"));

    ASSERT_TRUE(resultado.ok());
    EXPECT_FALSE(resultado.error.has_value());
    EXPECT_EQ(resultado.request->method, HttpMethod::GET);
    EXPECT_EQ(resultado.request->path, "/index.html");
    EXPECT_EQ(resultado.request->version, "HTTP/1.1");
}

TEST(HttpParserTest, ReconheceTodosOsMetodosConhecidos) {
    EXPECT_EQ(HttpParser::parse(req("GET / HTTP/1.1")).request->method, HttpMethod::GET);
    EXPECT_EQ(HttpParser::parse(req("POST / HTTP/1.1")).request->method, HttpMethod::POST);
    EXPECT_EQ(HttpParser::parse(req("PUT / HTTP/1.1")).request->method, HttpMethod::PUT);
    // regressao: DELETE era mapeado para GET
    EXPECT_EQ(HttpParser::parse(req("DELETE / HTTP/1.1")).request->method, HttpMethod::DELETE);
}

TEST(HttpParserTest, VersaoNaoIncluiOEspaco) {
    // regressao: substr(second_space) devolvia " HTTP/1.1"
    ParseResult resultado = HttpParser::parse(req("GET / HTTP/1.1"));

    ASSERT_TRUE(resultado.ok());
    EXPECT_EQ(resultado.request->version, "HTTP/1.1");
}

TEST(HttpParserTest, MetodoDesconhecidoEhNotImplemented) {
    espera_erro(req("FOO / HTTP/1.1"), ParseError::NOT_IMPLEMENTED);
    espera_erro(req("get / HTTP/1.1"), ParseError::NOT_IMPLEMENTED); // metodos sao case-sensitive
}

TEST(HttpParserTest, EntradaVaziaOuSemFimDeLinhaEhBadRequest) {
    espera_erro("", ParseError::BAD_REQUEST);
    espera_erro("GET / HTTP/1.1", ParseError::BAD_REQUEST);
}

TEST(HttpParserTest, LinhaDeRequisicaoMalformadaEhBadRequest) {
    espera_erro(req("GET"), ParseError::BAD_REQUEST);
    espera_erro(req("GET /"), ParseError::BAD_REQUEST);
    espera_erro(req("GET  HTTP/1.1"), ParseError::BAD_REQUEST);        // alvo vazio
    espera_erro(req("GET / "), ParseError::BAD_REQUEST);               // versao vazia
    espera_erro(req(" / HTTP/1.1"), ParseError::BAD_REQUEST);          // metodo vazio
    espera_erro(req("GET / HTTP/1.1 extra"), ParseError::BAD_REQUEST); // parte sobrando
}

// ---- versao ----

TEST(HttpParserTest, AceitaHttp10SemHost) {
    ParseResult resultado = HttpParser::parse(req("GET / HTTP/1.0", ""));

    ASSERT_TRUE(resultado.ok());
    EXPECT_EQ(resultado.request->version, "HTTP/1.0");
}

TEST(HttpParserTest, VersaoBemFormadaMasNaoSuportadaEh505) {
    espera_erro(req("GET / HTTP/2.0"), ParseError::VERSION_NOT_SUPPORTED);
    espera_erro(req("GET / HTTP/0.9"), ParseError::VERSION_NOT_SUPPORTED);
    espera_erro(req("GET / HTTP/1.2"), ParseError::VERSION_NOT_SUPPORTED);
}

TEST(HttpParserTest, VersaoMalFormadaEhBadRequest) {
    espera_erro(req("GET / HTTP/2"), ParseError::BAD_REQUEST);
    espera_erro(req("GET / http/1.1"), ParseError::BAD_REQUEST);
    espera_erro(req("GET / HTTP/1.1x"), ParseError::BAD_REQUEST);
    espera_erro(req("GET / HTTP/1.10"), ParseError::BAD_REQUEST);
    espera_erro(req("GET / FTP/1.1"), ParseError::BAD_REQUEST);
}

// ---- alvo da requisicao ----

TEST(HttpParserTest, MantemQueryStringNoCaminho) {
    ParseResult resultado = HttpParser::parse(req("GET /busca?q=a&b=2 HTTP/1.1"));

    ASSERT_TRUE(resultado.ok());
    EXPECT_EQ(resultado.request->path, "/busca?q=a&b=2");
}

TEST(HttpParserTest, AlvoPrecisaComecarComBarra) {
    espera_erro(req("GET index.html HTTP/1.1"), ParseError::BAD_REQUEST);
    espera_erro(req("GET http://x/ HTTP/1.1"), ParseError::BAD_REQUEST);
    espera_erro(req("GET * HTTP/1.1"), ParseError::BAD_REQUEST);
}

TEST(HttpParserTest, AlvoComCaracteresDeControleEhBadRequest) {
    espera_erro(req(std::string("GET /a\0b HTTP/1.1", sizeof("GET /a\0b HTTP/1.1") - 1)), ParseError::BAD_REQUEST);
    espera_erro(req("GET /a\x01z HTTP/1.1"), ParseError::BAD_REQUEST);
    espera_erro(req("GET /a\x7f HTTP/1.1"), ParseError::BAD_REQUEST);
    espera_erro(req("GET /a\tb HTTP/1.1"), ParseError::BAD_REQUEST);
}

TEST(HttpParserTest, AlvoMuitoLongoEh414) {
    const std::string no_limite = "/" + std::string(2047, 'a'); // 2048 bytes
    const std::string acima = "/" + std::string(2048, 'a');     // 2049 bytes

    EXPECT_TRUE(HttpParser::parse(req("GET " + no_limite + " HTTP/1.1")).ok());
    espera_erro(req("GET " + acima + " HTTP/1.1"), ParseError::URI_TOO_LONG);
}

TEST(HttpParserTest, NaoQuebraComEntradaGiganteOuBinaria) {
    // nao deve travar nem ler fora do buffer; o motivo do erro e o que importa
    espera_erro("GET /" + std::string(1'000'000, 'a') + " HTTP/1.1\r\n\r\n", ParseError::URI_TOO_LONG);
    EXPECT_NO_THROW((void)HttpParser::parse(std::string(4096, '\xff')));
    espera_erro(std::string(4096, '\xff') + "\r\n\r\n", ParseError::BAD_REQUEST);
}

// ---- cabecalhos ----

TEST(HttpParserTest, ParseiaCabecalhosComNomeEmMinusculas) {
    ParseResult resultado =
        HttpParser::parse(req("GET / HTTP/1.1", "Host: localhost:8080\r\nUser-Agent: curl/8.0\r\nACCEPT: */*\r\n"));

    ASSERT_TRUE(resultado.ok());
    const auto& headers = resultado.request->headers;
    EXPECT_EQ(headers.size(), 3u);
    EXPECT_EQ(headers.at("host"), "localhost:8080");
    EXPECT_EQ(headers.at("user-agent"), "curl/8.0");
    EXPECT_EQ(headers.at("accept"), "*/*");
}

TEST(HttpParserTest, RemoveEspacosOpcionaisAoRedorDoValor) {
    ParseResult resultado =
        HttpParser::parse(req("GET / HTTP/1.1", "Host: x\r\nA:valor\r\nB:   com espacos \t\r\nC:\r\n"));

    ASSERT_TRUE(resultado.ok());
    const auto& headers = resultado.request->headers;
    EXPECT_EQ(headers.at("a"), "valor");
    EXPECT_EQ(headers.at("b"), "com espacos");
    EXPECT_EQ(headers.at("c"), "");
}

TEST(HttpParserTest, JuntaCabecalhosRepetidosComVirgula) {
    ParseResult resultado =
        HttpParser::parse(req("GET / HTTP/1.1", "Host: x\r\nAccept: text/html\r\nACCEPT: image/png\r\n"));

    ASSERT_TRUE(resultado.ok());
    EXPECT_EQ(resultado.request->headers.at("accept"), "text/html, image/png");
}

TEST(HttpParserTest, IgnoraCorpoAposOsCabecalhos) {
    ParseResult resultado = HttpParser::parse(req("POST / HTTP/1.1") + "corpo qualquer");

    ASSERT_TRUE(resultado.ok());
    EXPECT_EQ(resultado.request->headers.size(), 1u);
}

TEST(HttpParserTest, CabecalhosSemLinhaVaziaFinalSaoBadRequest) {
    espera_erro("GET / HTTP/1.1\r\nHost: x\r\n", ParseError::BAD_REQUEST);
    espera_erro("GET / HTTP/1.1\r\n", ParseError::BAD_REQUEST);
}

TEST(HttpParserTest, CabecalhoMalFormadoEhBadRequest) {
    const std::string linha = "GET / HTTP/1.1";
    espera_erro(req(linha, "Host: x\r\nSemDoisPontos\r\n"), ParseError::BAD_REQUEST);
    espera_erro(req(linha, "Host: x\r\n: valor\r\n"), ParseError::BAD_REQUEST); // nome vazio
    espera_erro(req(linha, "Host: x\r\nB@d: v\r\n"), ParseError::BAD_REQUEST);  // nome nao e token
    espera_erro(req(linha, "Host: x\r\nNome Com Espaco: v\r\n"), ParseError::BAD_REQUEST);
}

TEST(HttpParserTest, EspacoAntesDosDoisPontosEhBadRequest) {
    // vetor classico de request smuggling (RFC 9112, secao 5.1)
    espera_erro(req("GET / HTTP/1.1", "Host: x\r\nContent-Length : 5\r\n"), ParseError::BAD_REQUEST);
}

TEST(HttpParserTest, LinhaDeContinuacaoObsFoldEhBadRequest) {
    espera_erro(req("GET / HTTP/1.1", "Host: x\r\nA: um\r\n dois\r\n"), ParseError::BAD_REQUEST);
    espera_erro(req("GET / HTTP/1.1", "Host: x\r\nA: um\r\n\tdois\r\n"), ParseError::BAD_REQUEST);
}

TEST(HttpParserTest, ValorComNulOuBareCrLfEhBadRequest) {
    const std::string com_nul("Host: x\r\nA: a\0b\r\n", sizeof("Host: x\r\nA: a\0b\r\n") - 1);
    espera_erro(req("GET / HTTP/1.1", com_nul), ParseError::BAD_REQUEST);
    espera_erro(req("GET / HTTP/1.1", "Host: x\r\nA: a\rb\r\n"), ParseError::BAD_REQUEST);
    espera_erro(req("GET / HTTP/1.1", "Host: x\r\nA: a\nb\r\n"), ParseError::BAD_REQUEST);
}

TEST(HttpParserTest, Http11ExigeExatamenteUmHost) {
    espera_erro(req("GET / HTTP/1.1", ""), ParseError::BAD_REQUEST);
    espera_erro(req("GET / HTTP/1.1", "Accept: */*\r\n"), ParseError::BAD_REQUEST);
    espera_erro(req("GET / HTTP/1.1", "Host: a\r\nHost: b\r\n"), ParseError::BAD_REQUEST);
}

TEST(HttpParserTest, LimitaAQuantidadeDeCabecalhos) {
    auto com_n_cabecalhos = [](int n) {
        std::string cabecalhos = "Host: x\r\n";
        for (int i = 1; i < n; ++i) {
            cabecalhos += "X-" + std::to_string(i) + ": v\r\n";
        }
        return req("GET / HTTP/1.1", cabecalhos);
    };

    EXPECT_TRUE(HttpParser::parse(com_n_cabecalhos(64)).ok());
    espera_erro(com_n_cabecalhos(65), ParseError::HEADERS_TOO_LARGE);
}

// ---- Content-Length ----

TEST(HttpParserTest, SemContentLengthOCorpoTemTamanhoZero) {
    EXPECT_EQ(HttpParser::parse(req("GET / HTTP/1.1")).request->content_length, 0u);
}

TEST(HttpParserTest, LeContentLength) {
    auto resultado = HttpParser::parse(req("POST / HTTP/1.1", "Host: x\r\nContent-Length: 42\r\n"));

    ASSERT_TRUE(resultado.ok());
    EXPECT_EQ(resultado.request->content_length, 42u);
}

TEST(HttpParserTest, ContentLengthZeroEValido) {
    auto resultado = HttpParser::parse(req("POST / HTTP/1.1", "Host: x\r\nContent-Length: 0\r\n"));

    ASSERT_TRUE(resultado.ok());
    EXPECT_EQ(resultado.request->content_length, 0u);
}

TEST(HttpParserTest, ContentLengthInvalidoDa400) {
    for (const char* valor : {"abc", "-1", "+5", "1.5", "", "0x10", "5 5", "5, 5", "5,6", "1e3"}) {
        espera_erro(req("POST / HTTP/1.1", std::string("Host: x\r\nContent-Length: ") + valor + "\r\n"),
                    ParseError::BAD_REQUEST);
    }
}

TEST(HttpParserTest, ContentLengthRepetidoDa400) {
    espera_erro(req("POST / HTTP/1.1", "Host: x\r\nContent-Length: 5\r\nContent-Length: 5\r\n"),
                ParseError::BAD_REQUEST);
}

TEST(HttpParserTest, ContentLengthAcimaDoLimiteDa413) {
    auto acima = std::to_string(HttpParser::kMaxBodyLength + 1);
    espera_erro(req("POST / HTTP/1.1", "Host: x\r\nContent-Length: " + acima + "\r\n"), ParseError::PAYLOAD_TOO_LARGE);
    auto no_limite = std::to_string(HttpParser::kMaxBodyLength);
    EXPECT_TRUE(HttpParser::parse(req("POST / HTTP/1.1", "Host: x\r\nContent-Length: " + no_limite + "\r\n")).ok());
}

TEST(HttpParserTest, ContentLengthGigantesco413SemEstourar) {
    // mais digitos que cabem em size_t: nao pode dar overflow nem excecao
    espera_erro(req("POST / HTTP/1.1", "Host: x\r\nContent-Length: 99999999999999999999999999\r\n"),
                ParseError::PAYLOAD_TOO_LARGE);
}

TEST(HttpParserTest, ToStatusDoPayloadTooLarge) {
    EXPECT_EQ(citius::to_status(ParseError::PAYLOAD_TOO_LARGE), citius::StatusCode::PAYLOAD_TOO_LARGE);
}

// ---- Transfer-Encoding ----

TEST(HttpParserTest, TransferEncodingSozinhoDa501) {
    espera_erro(req("POST / HTTP/1.1", "Host: x\r\nTransfer-Encoding: chunked\r\n"), ParseError::NOT_IMPLEMENTED);
}

TEST(HttpParserTest, TransferEncodingComContentLengthDa400) {
    // o caso classico de request smuggling (CL.TE / TE.CL): ambiguo, entao recusamos
    espera_erro(req("POST / HTTP/1.1", "Host: x\r\nContent-Length: 5\r\nTransfer-Encoding: chunked\r\n"),
                ParseError::BAD_REQUEST);
    espera_erro(req("POST / HTTP/1.1", "Host: x\r\nTransfer-Encoding: chunked\r\nContent-Length: 5\r\n"),
                ParseError::BAD_REQUEST);
}

TEST(HttpParserTest, TransferEncodingNaoDependeDeMaiusculas) {
    espera_erro(req("POST / HTTP/1.1", "Host: x\r\ntRaNsFeR-eNcOdInG: identity\r\n"), ParseError::NOT_IMPLEMENTED);
}

TEST(HttpParserTest, TransferEncodingComContentLengthInvalidoContinuaSendo400) {
    espera_erro(req("POST / HTTP/1.1", "Host: x\r\nContent-Length: abc\r\nTransfer-Encoding: chunked\r\n"),
                ParseError::BAD_REQUEST);
}
