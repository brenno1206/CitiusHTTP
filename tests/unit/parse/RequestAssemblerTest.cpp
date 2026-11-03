#include "citius/parse/RequestAssembler.hpp"
#include <gtest/gtest.h>
#include <stdexcept>
#include <string>
#include <string_view>

using citius::AssembleStatus;
using citius::HttpMethod;
using citius::RequestAssembler;
using citius::StatusCode;

namespace {

constexpr std::string_view kGet = "GET /a HTTP/1.1\r\nHost: x\r\n\r\n";

std::string post(std::string_view body) {
    return "POST /p HTTP/1.1\r\nHost: x\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" +
           std::string(body);
}

} // namespace

TEST(RequestAssemblerTest, ComecaPedindoMaisBytesESemNadaGuardado) {
    RequestAssembler assembler;

    EXPECT_EQ(assembler.feed(""), AssembleStatus::NEED_MORE);
    EXPECT_FALSE(assembler.has_buffered_data());
}

TEST(RequestAssemblerTest, RequisicaoInteiraDeUmaVez) {
    RequestAssembler assembler;

    ASSERT_EQ(assembler.feed(kGet), AssembleStatus::COMPLETE);
    auto request = assembler.take_request();

    EXPECT_EQ(request.method, HttpMethod::GET);
    EXPECT_EQ(request.path, "/a");
    EXPECT_FALSE(assembler.has_buffered_data());
}

TEST(RequestAssemblerTest, UmByteDeCadaVez) {
    RequestAssembler assembler;

    for (std::size_t i = 0; i + 1 < kGet.size(); ++i) {
        ASSERT_EQ(assembler.feed(kGet.substr(i, 1)), AssembleStatus::NEED_MORE) << "byte " << i;
    }
    EXPECT_EQ(assembler.feed(kGet.substr(kGet.size() - 1)), AssembleStatus::COMPLETE);
    EXPECT_EQ(assembler.take_request().path, "/a");
}

TEST(RequestAssemblerTest, DelimitadorPartidoEntreDuasLeituras) {
    RequestAssembler assembler;

    ASSERT_EQ(assembler.feed("GET /a HTTP/1.1\r\nHost: x\r\n\r"), AssembleStatus::NEED_MORE);
    EXPECT_EQ(assembler.feed("\n"), AssembleStatus::COMPLETE);
}

TEST(RequestAssemblerTest, PedacoDeRequisicaoContaComoDadoGuardado) {
    RequestAssembler assembler;

    ASSERT_EQ(assembler.feed("GET /a HT"), AssembleStatus::NEED_MORE);
    EXPECT_TRUE(assembler.has_buffered_data());
}

TEST(RequestAssemblerTest, CorpoNaMesmaLeituraDaCabeca) {
    RequestAssembler assembler;

    ASSERT_EQ(assembler.feed(post("hello")), AssembleStatus::COMPLETE);
    auto request = assembler.take_request();

    EXPECT_EQ(request.method, HttpMethod::POST);
    EXPECT_EQ(request.content_length, 5u);
    EXPECT_EQ(request.body, "hello");
}

TEST(RequestAssemblerTest, CorpoChegandoAosPoucos) {
    RequestAssembler assembler;
    const std::string raw = post("hello");
    const std::size_t head = raw.size() - 5;

    ASSERT_EQ(assembler.feed(raw.substr(0, head)), AssembleStatus::NEED_MORE);
    ASSERT_EQ(assembler.feed(raw.substr(head, 3)), AssembleStatus::NEED_MORE);
    EXPECT_TRUE(assembler.has_buffered_data());
    ASSERT_EQ(assembler.feed(raw.substr(head + 3)), AssembleStatus::COMPLETE);
    EXPECT_EQ(assembler.take_request().body, "hello");
}

TEST(RequestAssemblerTest, ContentLengthZeroCompletaSemCorpo) {
    RequestAssembler assembler;

    ASSERT_EQ(assembler.feed("POST /p HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n"), AssembleStatus::COMPLETE);
    EXPECT_TRUE(assembler.take_request().body.empty());
}

TEST(RequestAssemblerTest, CorpoGrandeEmPedacos) {
    RequestAssembler assembler;
    const std::string body(100 * 1024, 'x');
    const std::string raw = post(body);

    AssembleStatus status = AssembleStatus::NEED_MORE;
    for (std::size_t i = 0; i < raw.size(); i += 4096) {
        status = assembler.feed(std::string_view{raw}.substr(i, 4096));
    }

    ASSERT_EQ(status, AssembleStatus::COMPLETE);
    EXPECT_EQ(assembler.take_request().body, body);
}

TEST(RequestAssemblerTest, RequisicoesEncadeadasNumaLeituraSo) {
    RequestAssembler assembler;
    const std::string raw = std::string(kGet) + "GET /b HTTP/1.1\r\nHost: x\r\n\r\n";

    ASSERT_EQ(assembler.feed(raw), AssembleStatus::COMPLETE);
    EXPECT_EQ(assembler.take_request().path, "/a");
    EXPECT_TRUE(assembler.has_buffered_data()); // a segunda ja esta aqui

    ASSERT_EQ(assembler.feed(""), AssembleStatus::COMPLETE); // sem ler nada novo
    EXPECT_EQ(assembler.take_request().path, "/b");

    EXPECT_EQ(assembler.feed(""), AssembleStatus::NEED_MORE);
    EXPECT_FALSE(assembler.has_buffered_data());
}

TEST(RequestAssemblerTest, SobraDepoisDoCorpoPertenceAProximaRequisicao) {
    RequestAssembler assembler;

    ASSERT_EQ(assembler.feed(post("abc") + "GET /b HT"), AssembleStatus::COMPLETE);
    EXPECT_EQ(assembler.take_request().body, "abc");

    EXPECT_EQ(assembler.feed(""), AssembleStatus::NEED_MORE);
    EXPECT_TRUE(assembler.has_buffered_data());
    ASSERT_EQ(assembler.feed("TP/1.1\r\nHost: x\r\n\r\n"), AssembleStatus::COMPLETE);
    EXPECT_EQ(assembler.take_request().path, "/b");
}

TEST(RequestAssemblerTest, BytesExtrasComRequisicaoPendenteNaoMudamOResultado) {
    RequestAssembler assembler;

    ASSERT_EQ(assembler.feed(kGet), AssembleStatus::COMPLETE);
    EXPECT_EQ(assembler.feed("GET /b"), AssembleStatus::COMPLETE); // ainda nao a retiramos
    EXPECT_EQ(assembler.take_request().path, "/a");
}

TEST(RequestAssemblerTest, KeepAliveVemDosCabecalhos) {
    RequestAssembler assembler;

    ASSERT_EQ(assembler.feed("GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"), AssembleStatus::COMPLETE);
    EXPECT_FALSE(assembler.take_request().keep_alive());
}

// ---- limites e erros -------------------------------------------------------------------------

TEST(RequestAssemblerTest, CabecaSemFimPassandoDoLimiteDa431) {
    RequestAssembler assembler{100};

    EXPECT_EQ(assembler.feed(std::string(100, 'a')), AssembleStatus::NEED_MORE); // exatamente no limite
    ASSERT_EQ(assembler.feed("a"), AssembleStatus::ERROR);
    EXPECT_EQ(assembler.error_status(), StatusCode::HEADERS_TOO_LARGE);
}

TEST(RequestAssemblerTest, CabecaCompletaMasMaiorQueOLimiteDa431) {
    RequestAssembler assembler{100};
    const std::string head = "GET /a HTTP/1.1\r\nHost: x\r\nX-Pad: " + std::string(100, 'p') + "\r\n\r\n";

    ASSERT_EQ(assembler.feed(head), AssembleStatus::ERROR); // chegou inteira de uma vez e mesmo assim passa
    EXPECT_EQ(assembler.error_status(), StatusCode::HEADERS_TOO_LARGE);
}

TEST(RequestAssemblerTest, CabecaExatamenteNoLimiteEAceita) {
    RequestAssembler assembler{kGet.size()};

    EXPECT_EQ(assembler.feed(kGet), AssembleStatus::COMPLETE);
}

TEST(RequestAssemblerTest, CabecaUmByteAcimaDoLimiteDa431) {
    RequestAssembler assembler{kGet.size() - 1};

    EXPECT_EQ(assembler.feed(kGet), AssembleStatus::ERROR);
}

TEST(RequestAssemblerTest, CabecaComPadraoDeLimiteDe8KB) {
    RequestAssembler assembler;

    ASSERT_EQ(assembler.feed(std::string(9000, 'a')), AssembleStatus::ERROR);
    EXPECT_EQ(assembler.error_status(), StatusCode::HEADERS_TOO_LARGE);
}

TEST(RequestAssemblerTest, RequisicaoMalformadaDa400) {
    RequestAssembler assembler;

    ASSERT_EQ(assembler.feed("LIXO\r\n\r\n"), AssembleStatus::ERROR);
    EXPECT_EQ(assembler.error_status(), StatusCode::BAD_REQUEST);
}

TEST(RequestAssemblerTest, MetodoDesconhecidoDa501) {
    RequestAssembler assembler;

    ASSERT_EQ(assembler.feed("PATCH / HTTP/1.1\r\nHost: x\r\n\r\n"), AssembleStatus::ERROR);
    EXPECT_EQ(assembler.error_status(), StatusCode::NOT_IMPLEMENTED);
}

TEST(RequestAssemblerTest, VersaoNaoSuportadaDa505) {
    RequestAssembler assembler;

    ASSERT_EQ(assembler.feed("GET / HTTP/2.0\r\nHost: x\r\n\r\n"), AssembleStatus::ERROR);
    EXPECT_EQ(assembler.error_status(), StatusCode::VERSION_NOT_SUPPORTED);
}

TEST(RequestAssemblerTest, CorpoGrandeDemaisDa413SemEsperarOCorpo) {
    RequestAssembler assembler;

    // so a cabeca: o erro sai sem precisar receber 2 MB
    ASSERT_EQ(assembler.feed("POST /p HTTP/1.1\r\nHost: x\r\nContent-Length: 2097152\r\n\r\n"), AssembleStatus::ERROR);
    EXPECT_EQ(assembler.error_status(), StatusCode::PAYLOAD_TOO_LARGE);
}

TEST(RequestAssemblerTest, TransferEncodingComContentLengthDa400) {
    RequestAssembler assembler;
    const std::string raw = "POST /p HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\nTransfer-Encoding: chunked\r\n\r\nabc";

    ASSERT_EQ(assembler.feed(raw), AssembleStatus::ERROR);
    EXPECT_EQ(assembler.error_status(), StatusCode::BAD_REQUEST);
}

TEST(RequestAssemblerTest, DepoisDoErroContinuaEmErro) {
    RequestAssembler assembler;
    ASSERT_EQ(assembler.feed("LIXO\r\n\r\n"), AssembleStatus::ERROR);

    // depois de um erro nao se sabe onde a proxima requisicao comeca: nada mais e interpretado
    EXPECT_EQ(assembler.feed(kGet), AssembleStatus::ERROR);
    EXPECT_EQ(assembler.error_status(), StatusCode::BAD_REQUEST);
}

TEST(RequestAssemblerTest, ErroNaSegundaRequisicaoEncadeada) {
    RequestAssembler assembler;
    ASSERT_EQ(assembler.feed(std::string(kGet) + "LIXO\r\n\r\n"), AssembleStatus::COMPLETE);
    (void)assembler.take_request();

    ASSERT_EQ(assembler.feed(""), AssembleStatus::ERROR);
    EXPECT_EQ(assembler.error_status(), StatusCode::BAD_REQUEST);
}

TEST(RequestAssemblerTest, TakeRequestSemRequisicaoCompletaLanca) {
    RequestAssembler assembler;
    ASSERT_EQ(assembler.feed("GET /a"), AssembleStatus::NEED_MORE);

    EXPECT_THROW((void)assembler.take_request(), std::logic_error);
}

TEST(RequestAssemblerTest, ErrorStatusSemErroLanca) {
    RequestAssembler assembler;

    EXPECT_THROW((void)assembler.error_status(), std::logic_error);
}

TEST(RequestAssemblerTest, ContentLengthSemOCorpoFicaEsperando) {
    RequestAssembler assembler;

    ASSERT_EQ(assembler.feed("POST /p HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nab"), AssembleStatus::NEED_MORE);
    EXPECT_EQ(assembler.feed("c"), AssembleStatus::NEED_MORE);
}
