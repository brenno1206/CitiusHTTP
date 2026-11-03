#include "citius/HttpParser.hpp"
#include "citius/HttpResponse.hpp"
#include "citius/StatusCode.hpp"
#include <gtest/gtest.h>
#include <string>

using citius::HttpResponse;
using citius::ParseError;
using citius::StatusCode;

// ---- StatusCode ----

TEST(StatusCodeTest, FrasesPadraoDosCodigosUsados) {
    EXPECT_EQ(citius::reason_phrase(StatusCode::OK), "OK");
    EXPECT_EQ(citius::reason_phrase(StatusCode::BAD_REQUEST), "Bad Request");
    EXPECT_EQ(citius::reason_phrase(StatusCode::NOT_FOUND), "Not Found");
    EXPECT_EQ(citius::reason_phrase(StatusCode::METHOD_NOT_ALLOWED), "Method Not Allowed");
    EXPECT_EQ(citius::reason_phrase(StatusCode::PAYLOAD_TOO_LARGE), "Content Too Large");
    EXPECT_EQ(citius::reason_phrase(StatusCode::URI_TOO_LONG), "URI Too Long");
    EXPECT_EQ(citius::reason_phrase(StatusCode::HEADERS_TOO_LARGE), "Request Header Fields Too Large");
    EXPECT_EQ(citius::reason_phrase(StatusCode::INTERNAL_SERVER_ERROR), "Internal Server Error");
    EXPECT_EQ(citius::reason_phrase(StatusCode::NOT_IMPLEMENTED), "Not Implemented");
    EXPECT_EQ(citius::reason_phrase(StatusCode::SERVICE_UNAVAILABLE), "Service Unavailable");
    EXPECT_EQ(citius::reason_phrase(StatusCode::VERSION_NOT_SUPPORTED), "HTTP Version Not Supported");
}

TEST(StatusCodeTest, ValorForaDoEnumNaoQuebra) {
    EXPECT_EQ(citius::reason_phrase(static_cast<StatusCode>(999)), "Unknown");
}

TEST(StatusCodeTest, ParseErrorViraOStatusCorreto) {
    EXPECT_EQ(citius::to_status(ParseError::BAD_REQUEST), StatusCode::BAD_REQUEST);
    EXPECT_EQ(citius::to_status(ParseError::NOT_IMPLEMENTED), StatusCode::NOT_IMPLEMENTED);
    EXPECT_EQ(citius::to_status(ParseError::URI_TOO_LONG), StatusCode::URI_TOO_LONG);
    EXPECT_EQ(citius::to_status(ParseError::VERSION_NOT_SUPPORTED), StatusCode::VERSION_NOT_SUPPORTED);
    EXPECT_EQ(citius::to_status(ParseError::HEADERS_TOO_LARGE), StatusCode::HEADERS_TOO_LARGE);
}

// ---- HttpResponse ----

TEST(HttpResponseTest, RespostaVaziaTemContentLengthZero) {
    HttpResponse response;  // 200 por padrao

    EXPECT_EQ(response.serialize(), "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
}

TEST(HttpResponseTest, ContentLengthSaiDoCorpo) {
    HttpResponse response{StatusCode::OK};
    response.set_body("Hello Citius!", "text/plain");

    EXPECT_EQ(response.serialize(),
              "HTTP/1.1 200 OK\r\n"
              "Content-Type: text/plain\r\n"
              "Content-Length: 13\r\n"
              "\r\n"
              "Hello Citius!");
}

TEST(HttpResponseTest, ContentLengthContaBytesENaoCaracteres) {
    HttpResponse response;
    response.set_body("ol\xc3\xa1");  // "olá" em UTF-8: 3 caracteres, 4 bytes

    EXPECT_NE(response.serialize().find("Content-Length: 4\r\n"), std::string::npos);
}

TEST(HttpResponseTest, CorpoBinarioComNulEPreservado) {
    HttpResponse response;
    const std::string corpo("a\0b\r\n\r\nc", 8);
    response.set_body(corpo);

    const std::string raw = response.serialize();
    EXPECT_NE(raw.find("Content-Length: 8\r\n"), std::string::npos);
    EXPECT_EQ(raw.substr(raw.size() - 8), corpo);
}

TEST(HttpResponseTest, SetBodyTrocaCorpoEContentLength) {
    HttpResponse response;
    response.set_body("longo longo longo");
    response.set_body("curto");

    const std::string raw = response.serialize();
    EXPECT_NE(raw.find("Content-Length: 5\r\n"), std::string::npos);
    EXPECT_EQ(raw.find("longo"), std::string::npos);
}

TEST(HttpResponseTest, SerializaStatusDeErro) {
    HttpResponse response{StatusCode::NOT_FOUND};

    EXPECT_EQ(response.serialize().rfind("HTTP/1.1 404 Not Found\r\n", 0), 0u);
}

TEST(HttpResponseTest, CabecalhosSaemNaOrdemDeInsercao) {
    HttpResponse response;
    ASSERT_TRUE(response.set_header("X-B", "2"));
    ASSERT_TRUE(response.set_header("X-A", "1"));

    EXPECT_EQ(response.serialize(), "HTTP/1.1 200 OK\r\nX-B: 2\r\nX-A: 1\r\nContent-Length: 0\r\n\r\n");
}

TEST(HttpResponseTest, SetHeaderSubstituiIgnorandoMaiusculas) {
    HttpResponse response;
    ASSERT_TRUE(response.set_header("Connection", "keep-alive"));
    ASSERT_TRUE(response.set_header("CONNECTION", "close"));

    EXPECT_EQ(response.serialize(), "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
}

TEST(HttpResponseTest, ContentLengthNaoPodeSerDefinidoPeloChamador) {
    HttpResponse response;
    response.set_body("abc");

    EXPECT_FALSE(response.set_header("Content-Length", "999"));
    EXPECT_FALSE(response.set_header("content-length", "999"));
    EXPECT_NE(response.serialize().find("Content-Length: 3\r\n"), std::string::npos);
    EXPECT_EQ(response.serialize().find("999"), std::string::npos);
}

TEST(HttpResponseTest, RejeitaInjecaoDeCabecalho) {
    // response splitting: um valor vindo do cliente nao pode abrir novas linhas
    HttpResponse response;

    EXPECT_FALSE(response.set_header("Location", "/ok\r\nSet-Cookie: roubado=1"));
    EXPECT_FALSE(response.set_header("Location", "/ok\nX: y"));
    EXPECT_FALSE(response.set_header("Location", std::string("/a\0b", 4)));
    EXPECT_FALSE(response.set_header("X-Bad\r\nName", "v"));
    EXPECT_FALSE(response.set_header("", "v"));
    EXPECT_FALSE(response.set_header("Nome Com Espaco", "v"));
    EXPECT_EQ(response.serialize(), "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
}

TEST(HttpResponseTest, AceitaValorVazioEComHtab) {
    HttpResponse response;

    EXPECT_TRUE(response.set_header("X-Vazio", ""));
    EXPECT_TRUE(response.set_header("X-Tab", "a\tb"));
}

TEST(HttpResponseTest, MakeErrorMontaCorpoEmTextoComTamanhoCerto) {
    HttpResponse response = HttpResponse::make_error(StatusCode::NOT_FOUND);

    EXPECT_EQ(response.serialize(),
              "HTTP/1.1 404 Not Found\r\n"
              "Content-Type: text/plain; charset=utf-8\r\n"
              "Connection: close\r\n"
              "Content-Length: 14\r\n"
              "\r\n"
              "404 Not Found\n");
}
