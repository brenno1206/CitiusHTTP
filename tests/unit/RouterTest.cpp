#include "citius/Router.hpp"
#include <gtest/gtest.h>
#include <stdexcept>
#include <string>

using citius::HttpMethod;
using citius::HttpRequest;
using citius::HttpResponse;
using citius::Router;
using citius::StatusCode;

namespace {

HttpRequest requisicao(HttpMethod method, std::string path) {
    HttpRequest request;
    request.method = method;
    request.path = std::move(path);
    request.version = "HTTP/1.1";
    return request;
}

// Handler que responde 200 com um texto fixo, para sabermos qual rota atendeu.
Router::Handler responde(std::string texto) {
    return [texto = std::move(texto)](const HttpRequest&) {
        HttpResponse response{StatusCode::OK};
        response.set_body(texto);
        return response;
    };
}

// O corpo e o que vem depois da linha vazia na resposta serializada.
std::string corpo_de(const HttpResponse& response) {
    std::string bytes = response.serialize();
    return bytes.substr(bytes.find("\r\n\r\n") + 4);
}

} // namespace

TEST(RouterTest, RotaRegistradaChamaOHandler) {
    Router router;
    router.get("/ping", responde("pong"));

    HttpResponse response = router.route(requisicao(HttpMethod::GET, "/ping"));

    EXPECT_EQ(response.status(), StatusCode::OK);
    EXPECT_EQ(corpo_de(response), "pong");
}

TEST(RouterTest, HandlerRecebeARequisicao) {
    Router router;
    router.get("/eco", [](const HttpRequest& request) {
        HttpResponse response{StatusCode::OK};
        response.set_body(request.path);
        return response;
    });

    HttpResponse response = router.route(requisicao(HttpMethod::GET, "/eco?x=1"));

    EXPECT_EQ(corpo_de(response), "/eco?x=1"); // o handler ve o alvo original, com a query
}

TEST(RouterTest, QueryStringNaoAtrapalhaOCasamento) {
    Router router;
    router.get("/ping", responde("pong"));

    EXPECT_EQ(router.route(requisicao(HttpMethod::GET, "/ping?x=1&y=2")).status(), StatusCode::OK);
    EXPECT_EQ(router.route(requisicao(HttpMethod::GET, "/ping?")).status(), StatusCode::OK);
}

TEST(RouterTest, CasamentoEExatoEDiferenciaMaiusculas) {
    Router router;
    router.get("/ping", responde("pong"));

    EXPECT_EQ(router.route(requisicao(HttpMethod::GET, "/ping/")).status(), StatusCode::NOT_FOUND);
    EXPECT_EQ(router.route(requisicao(HttpMethod::GET, "/pin")).status(), StatusCode::NOT_FOUND);
    EXPECT_EQ(router.route(requisicao(HttpMethod::GET, "/PING")).status(), StatusCode::NOT_FOUND);
    EXPECT_EQ(router.route(requisicao(HttpMethod::GET, "/ping/x")).status(), StatusCode::NOT_FOUND);
}

TEST(RouterTest, CaminhoInexistenteSemFallbackDa404) {
    Router router;
    router.get("/ping", responde("pong"));

    EXPECT_EQ(router.route(requisicao(HttpMethod::GET, "/outro")).status(), StatusCode::NOT_FOUND);
}

TEST(RouterTest, RoteadorVazioDa404) {
    Router router;

    EXPECT_EQ(router.route(requisicao(HttpMethod::GET, "/")).status(), StatusCode::NOT_FOUND);
}

TEST(RouterTest, MetodoDiferenteNoMesmoCaminhoDa405ComAllow) {
    Router router;
    router.get("/ping", responde("pong"));

    HttpResponse response = router.route(requisicao(HttpMethod::POST, "/ping"));

    EXPECT_EQ(response.status(), StatusCode::METHOD_NOT_ALLOWED);
    EXPECT_NE(response.serialize().find("Allow: GET\r\n"), std::string::npos);
}

TEST(RouterTest, AllowListaTodosOsMetodosDoCaminho) {
    Router router;
    router.get("/recurso", responde("g"));
    router.post("/recurso", responde("p"));
    router.del("/recurso", responde("d"));

    HttpResponse response = router.route(requisicao(HttpMethod::PUT, "/recurso"));

    EXPECT_EQ(response.status(), StatusCode::METHOD_NOT_ALLOWED);
    EXPECT_NE(response.serialize().find("Allow: GET, POST, DELETE\r\n"), std::string::npos);
}

TEST(RouterTest, MetodoDesconhecidoNumaRotaExistenteDa405) {
    Router router;
    router.get("/ping", responde("pong"));

    EXPECT_EQ(router.route(requisicao(HttpMethod::UNKNOWN, "/ping")).status(), StatusCode::METHOD_NOT_ALLOWED);
}

TEST(RouterTest, CadaMetodoTemSeuHandler) {
    Router router;
    router.get("/r", responde("get"));
    router.post("/r", responde("post"));
    router.put("/r", responde("put"));
    router.del("/r", responde("delete"));

    EXPECT_EQ(corpo_de(router.route(requisicao(HttpMethod::GET, "/r"))), "get");
    EXPECT_EQ(corpo_de(router.route(requisicao(HttpMethod::POST, "/r"))), "post");
    EXPECT_EQ(corpo_de(router.route(requisicao(HttpMethod::PUT, "/r"))), "put");
    EXPECT_EQ(corpo_de(router.route(requisicao(HttpMethod::DELETE, "/r"))), "delete");
}

TEST(RouterTest, RegistrarDeNovoSubstituiOHandler) {
    Router router;
    router.get("/ping", responde("velho"));
    router.get("/ping", responde("novo"));

    EXPECT_EQ(corpo_de(router.route(requisicao(HttpMethod::GET, "/ping"))), "novo");
}

TEST(RouterTest, FallbackAtendeCaminhoSemRota) {
    Router router;
    router.get("/ping", responde("pong"));
    router.set_fallback(responde("fallback"));

    EXPECT_EQ(corpo_de(router.route(requisicao(HttpMethod::GET, "/index.html"))), "fallback");
    EXPECT_EQ(corpo_de(router.route(requisicao(HttpMethod::GET, "/ping"))), "pong");
}

TEST(RouterTest, FallbackNaoSubstituiO405DeUmaRotaExistente) {
    Router router;
    router.get("/ping", responde("pong"));
    router.set_fallback(responde("fallback"));

    EXPECT_EQ(router.route(requisicao(HttpMethod::POST, "/ping")).status(), StatusCode::METHOD_NOT_ALLOWED);
}

TEST(RouterTest, FallbackRecebeOMetodoQueNaoTemRotaNenhuma) {
    Router router;
    router.set_fallback(responde("fallback"));

    EXPECT_EQ(corpo_de(router.route(requisicao(HttpMethod::DELETE, "/qualquer"))), "fallback");
}

TEST(RouterTest, RaizPodeSerUmaRota) {
    Router router;
    router.get("/", responde("inicio"));

    EXPECT_EQ(corpo_de(router.route(requisicao(HttpMethod::GET, "/"))), "inicio");
    EXPECT_EQ(corpo_de(router.route(requisicao(HttpMethod::GET, "/?a=b"))), "inicio");
}

TEST(RouterTest, CaminhosHostisNaoQuebram) {
    Router router;
    router.get("/ping", responde("pong"));

    EXPECT_EQ(router.route(requisicao(HttpMethod::GET, "")).status(), StatusCode::NOT_FOUND);
    EXPECT_EQ(router.route(requisicao(HttpMethod::GET, "?")).status(), StatusCode::NOT_FOUND);
    EXPECT_EQ(router.route(requisicao(HttpMethod::GET, "/../../etc/passwd")).status(), StatusCode::NOT_FOUND);
    EXPECT_EQ(router.route(requisicao(HttpMethod::GET, std::string("/ping\0x", 7))).status(), StatusCode::NOT_FOUND);
    EXPECT_EQ(router.route(requisicao(HttpMethod::GET, "/" + std::string(100000, 'a'))).status(),
              StatusCode::NOT_FOUND);
}

TEST(RouterTest, RegistrarCaminhoInvalidoLancaExcecao) {
    Router router;

    EXPECT_THROW(router.get("", responde("x")), std::invalid_argument);
    EXPECT_THROW(router.get("ping", responde("x")), std::invalid_argument);      // sem '/' inicial
    EXPECT_THROW(router.get("/ping?x=1", responde("x")), std::invalid_argument); // query nao faz parte da rota
}

TEST(RouterTest, RegistrarHandlerVazioLancaExcecao) {
    Router router;

    EXPECT_THROW(router.get("/ping", Router::Handler{}), std::invalid_argument);
}
