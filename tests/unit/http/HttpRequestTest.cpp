#include "citius/parse/HttpParser.hpp"
#include <gtest/gtest.h>
#include <string>

using citius::HttpParser;

namespace {

// Interpreta uma requisicao com os cabecalhos extras e devolve keep_alive().
bool keep_alive(const std::string& versao, const std::string& extra = "") {
    auto resultado = HttpParser::parse("GET / " + versao + "\r\nHost: x\r\n" + extra + "\r\n");
    EXPECT_TRUE(resultado.ok());
    return resultado.request->keep_alive();
}

} // namespace

TEST(HttpRequestTest, Http11EPersistentePorPadrao) {
    EXPECT_TRUE(keep_alive("HTTP/1.1"));
}

TEST(HttpRequestTest, Http10FechaPorPadrao) {
    EXPECT_FALSE(keep_alive("HTTP/1.0"));
}

TEST(HttpRequestTest, ConnectionCloseFechaEmHttp11) {
    EXPECT_FALSE(keep_alive("HTTP/1.1", "Connection: close\r\n"));
}

TEST(HttpRequestTest, ConnectionKeepAliveMantemEmHttp10) {
    EXPECT_TRUE(keep_alive("HTTP/1.0", "Connection: keep-alive\r\n"));
}

TEST(HttpRequestTest, TokensNaoDependemDeMaiusculas) {
    EXPECT_FALSE(keep_alive("HTTP/1.1", "connection: CLOSE\r\n"));
    EXPECT_TRUE(keep_alive("HTTP/1.0", "CONNECTION: Keep-Alive\r\n"));
}

TEST(HttpRequestTest, ListaDeTokensEEspacosSaoTratados) {
    EXPECT_FALSE(keep_alive("HTTP/1.1", "Connection: upgrade ,  close \r\n"));
    EXPECT_TRUE(keep_alive("HTTP/1.0", "Connection: te, keep-alive\r\n"));
}

TEST(HttpRequestTest, CloseVenceKeepAlive) {
    EXPECT_FALSE(keep_alive("HTTP/1.1", "Connection: keep-alive, close\r\n"));
    EXPECT_FALSE(keep_alive("HTTP/1.1", "Connection: keep-alive\r\nConnection: close\r\n"));
}

TEST(HttpRequestTest, TokenParecidoNaoConta) {
    EXPECT_TRUE(keep_alive("HTTP/1.1", "Connection: closed\r\n"));
    EXPECT_FALSE(keep_alive("HTTP/1.0", "Connection: keep-alive-not\r\n"));
}
