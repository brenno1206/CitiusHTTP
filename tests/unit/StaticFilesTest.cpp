#include "citius/StaticFiles.hpp"
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;
using citius::HttpMethod;
using citius::HttpRequest;
using citius::HttpResponse;
using citius::StatusCode;

namespace {

HttpRequest get(std::string target, HttpMethod method = HttpMethod::GET) {
    HttpRequest request;
    request.method = method;
    request.path = std::move(target);
    request.version = "HTTP/1.1";
    return request;
}

bool contem(const std::string& texto, const std::string& trecho) {
    return texto.find(trecho) != std::string::npos;
}

} // namespace

class StaticFilesTest : public ::testing::Test {
protected:
    void SetUp() override {
        base = fs::temp_directory_path() /
               ("citius_staticfiles_" + std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
        fs::remove_all(base);
        root = base / "www";
        fs::create_directories(root / "sub");
        fs::create_directories(root / "vazio");
        escrever(root / "index.html", "<h1>oi</h1>");
        escrever(root / "style.css", "body{}");
        escrever(root / "dados.xyz", std::string("a\0b\xff", 4));
        escrever(root / "zero.txt", "");
        escrever(root / "sub" / "index.html", "sub");
        escrever(base / "secret.txt", "segredo");
    }

    void TearDown() override {
        fs::permissions(root / "bloqueado.txt", fs::perms::owner_all, fs::perm_options::add, ignorado);
        fs::remove_all(base);
    }

    static void escrever(const fs::path& path, const std::string& conteudo) {
        std::ofstream{path, std::ios::binary} << conteudo;
    }

    fs::path base;
    fs::path root;
    std::error_code ignorado;
};

TEST_F(StaticFilesTest, RaizServeIndexHtml) {
    HttpResponse response = citius::serve_static(root, get("/"));
    EXPECT_EQ(response.status(), StatusCode::OK);
    std::string bytes = response.serialize();
    EXPECT_TRUE(contem(bytes, "Content-Type: text/html; charset=utf-8\r\n"));
    EXPECT_TRUE(contem(bytes, "Content-Length: 11\r\n"));
    EXPECT_TRUE(bytes.ends_with("\r\n\r\n<h1>oi</h1>"));
}

TEST_F(StaticFilesTest, ServeCssComTipoCerto) {
    HttpResponse response = citius::serve_static(root, get("/style.css?v=2"));
    EXPECT_EQ(response.status(), StatusCode::OK);
    EXPECT_TRUE(contem(response.serialize(), "Content-Type: text/css; charset=utf-8\r\n"));
}

TEST_F(StaticFilesTest, PreservaBytesBinariosEExtensaoDesconhecida) {
    HttpResponse response = citius::serve_static(root, get("/dados.xyz"));
    EXPECT_EQ(response.status(), StatusCode::OK);
    std::string bytes = response.serialize();
    EXPECT_TRUE(contem(bytes, "Content-Type: application/octet-stream\r\n"));
    EXPECT_TRUE(bytes.ends_with(std::string("\r\n\r\na\0b\xff", 8)));
}

TEST_F(StaticFilesTest, ArquivoVazioDa200) {
    HttpResponse response = citius::serve_static(root, get("/zero.txt"));
    EXPECT_EQ(response.status(), StatusCode::OK);
    EXPECT_TRUE(contem(response.serialize(), "Content-Length: 0\r\n"));
}

TEST_F(StaticFilesTest, IndexDeSubdiretorio) {
    EXPECT_EQ(citius::serve_static(root, get("/sub/")).status(), StatusCode::OK);
}

TEST_F(StaticFilesTest, ArquivoInexistenteDa404) {
    EXPECT_EQ(citius::serve_static(root, get("/nada.html")).status(), StatusCode::NOT_FOUND);
}

TEST_F(StaticFilesTest, DiretorioDa404) {
    EXPECT_EQ(citius::serve_static(root, get("/sub")).status(), StatusCode::NOT_FOUND);
    EXPECT_EQ(citius::serve_static(root, get("/vazio/")).status(), StatusCode::NOT_FOUND); // sem index.html
}

TEST_F(StaticFilesTest, PathTraversalDa404ESemVazarOConteudo) {
    for (const char* alvo : {"/../secret.txt", "/%2e%2e/secret.txt", "/%2E%2E%2Fsecret.txt", "/sub/../../secret.txt"}) {
        HttpResponse response = citius::serve_static(root, get(alvo));
        EXPECT_EQ(response.status(), StatusCode::NOT_FOUND) << alvo;
        EXPECT_FALSE(contem(response.serialize(), "segredo")) << alvo;
    }
}

TEST_F(StaticFilesTest, SymlinkParaForaDa404) {
    std::error_code ec;
    fs::create_symlink(base / "secret.txt", root / "link.txt", ec);
    if (ec)
        GTEST_SKIP() << "sem symlink";
    HttpResponse response = citius::serve_static(root, get("/link.txt"));
    EXPECT_EQ(response.status(), StatusCode::NOT_FOUND);
    EXPECT_FALSE(contem(response.serialize(), "segredo"));
}

TEST_F(StaticFilesTest, PercentMalformadoDa400) {
    EXPECT_EQ(citius::serve_static(root, get("/%zz")).status(), StatusCode::BAD_REQUEST);
    EXPECT_EQ(citius::serve_static(root, get("/a%00b")).status(), StatusCode::BAD_REQUEST);
}

TEST_F(StaticFilesTest, MetodoQueNaoEGetDa405ComAllow) {
    for (HttpMethod metodo : {HttpMethod::POST, HttpMethod::PUT, HttpMethod::DELETE}) {
        HttpResponse response = citius::serve_static(root, get("/", metodo));
        EXPECT_EQ(response.status(), StatusCode::METHOD_NOT_ALLOWED);
        EXPECT_TRUE(contem(response.serialize(), "Allow: GET\r\n"));
    }
}

TEST_F(StaticFilesTest, ArquivoMaiorQueOLimiteDa500) {
    HttpResponse response = citius::serve_static(root, get("/index.html"), 5); // 11 bytes > 5
    EXPECT_EQ(response.status(), StatusCode::INTERNAL_SERVER_ERROR);
    EXPECT_EQ(citius::serve_static(root, get("/index.html"), 11).status(), StatusCode::OK); // limite inclusivo
}

TEST_F(StaticFilesTest, ArquivoSemPermissaoDa403) {
    if (::geteuid() == 0)
        GTEST_SKIP() << "root le qualquer arquivo";
    escrever(root / "bloqueado.txt", "x");
    fs::permissions(root / "bloqueado.txt", fs::perms::none);
    EXPECT_EQ(citius::serve_static(root, get("/bloqueado.txt")).status(), StatusCode::FORBIDDEN);
}
