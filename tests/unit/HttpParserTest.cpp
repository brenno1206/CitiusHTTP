#include "citius/HttpParser.hpp"
#include <gtest/gtest.h>
#include <string>

using citius::HttpMethod;
using citius::HttpParser;

TEST(HttpParserTest, ParseaRequisicaoGetValida) {
    auto request = HttpParser::parse("GET /index.html HTTP/1.1\r\nHost: localhost\r\n\r\n");

    ASSERT_TRUE(request.has_value());
    EXPECT_EQ(request->method, HttpMethod::GET);
    EXPECT_EQ(request->path, "/index.html");
    EXPECT_EQ(request->version, "HTTP/1.1");
}

TEST(HttpParserTest, ReconheceTodosOsMetodosConhecidos) {
    EXPECT_EQ(HttpParser::parse("GET / HTTP/1.1\r\n")->method, HttpMethod::GET);
    EXPECT_EQ(HttpParser::parse("POST / HTTP/1.1\r\n")->method, HttpMethod::POST);
    EXPECT_EQ(HttpParser::parse("PUT / HTTP/1.1\r\n")->method, HttpMethod::PUT);
    EXPECT_EQ(HttpParser::parse("DELETE / HTTP/1.1\r\n")->method, HttpMethod::DELETE);
}

TEST(HttpParserTest, VersaoNaoIncluiOEspaco) {
    auto request = HttpParser::parse("GET / HTTP/1.1\r\n");

    ASSERT_TRUE(request.has_value());
    EXPECT_EQ(request->version, "HTTP/1.1");
}

TEST(HttpParserTest, RejeitaMetodoDesconhecido) {
    EXPECT_FALSE(HttpParser::parse("FOO / HTTP/1.1\r\n").has_value());
    EXPECT_FALSE(HttpParser::parse("get / HTTP/1.1\r\n").has_value());
}

TEST(HttpParserTest, RejeitaEntradaVaziaOuSemFimDeLinha) {
    EXPECT_FALSE(HttpParser::parse("").has_value());
    EXPECT_FALSE(HttpParser::parse("GET / HTTP/1.1").has_value());
}

TEST(HttpParserTest, RejeitaLinhaDeRequisicaoMalformada) {
    EXPECT_FALSE(HttpParser::parse("GET\r\n").has_value()); 
    EXPECT_FALSE(HttpParser::parse("GET /\r\n").has_value()); 
    EXPECT_FALSE(HttpParser::parse("GET  HTTP/1.1\r\n").has_value());  // caminho vazio
    EXPECT_FALSE(HttpParser::parse("GET / \r\n").has_value());         // versao vazia
}

TEST(HttpParserTest, NaoQuebraComEntradaGigante) {
    std::string raw = "GET /" + std::string(1'000'000, 'a') + " HTTP/1.1\r\n";
    auto request = HttpParser::parse(raw);

    ASSERT_TRUE(request.has_value());
    EXPECT_EQ(request->path.size(), 1'000'001u);
}

TEST(HttpParserTest, NaoQuebraComBytesNulosEBinarios) {
    std::string raw("GET /a\0b HTTP/1.1\r\n", 20);
    // nao deve travar nem ler fora do buffer; o resultado em si e decisao do parser
    EXPECT_NO_THROW((void)HttpParser::parse(raw));
    EXPECT_NO_THROW((void)HttpParser::parse(std::string(4096, '\xff')));
}
