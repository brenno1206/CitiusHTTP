#include "citius/files/StaticFiles.hpp"
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <string>
#include <sys/stat.h>
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

// A resposta inteira como iria para o cliente: a cabeca (serialize) mais, se o corpo for um arquivo, o conteudo dele.
std::string tudo(const HttpResponse& response) {
    std::string bytes = response.serialize();
    if (const auto& corpo = response.file_body()) {
        std::string conteudo(corpo->size, '\0');
        const ssize_t n = ::pread(corpo->file->get(), conteudo.data(), conteudo.size(), 0);
        EXPECT_EQ(n, static_cast<ssize_t>(corpo->size));
        bytes += conteudo;
    }
    return bytes;
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
    std::string bytes = tudo(response);
    EXPECT_TRUE(contem(bytes, "Content-Type: text/html; charset=utf-8\r\n"));
    EXPECT_TRUE(contem(bytes, "Content-Length: 11\r\n"));
    EXPECT_TRUE(bytes.ends_with("\r\n\r\n<h1>oi</h1>"));
}

TEST_F(StaticFilesTest, ServeCssComTipoCerto) {
    HttpResponse response = citius::serve_static(root, get("/style.css?v=2"));
    EXPECT_EQ(response.status(), StatusCode::OK);
    EXPECT_TRUE(contem(tudo(response), "Content-Type: text/css; charset=utf-8\r\n"));
}

TEST_F(StaticFilesTest, PreservaBytesBinariosEExtensaoDesconhecida) {
    HttpResponse response = citius::serve_static(root, get("/dados.xyz"));
    EXPECT_EQ(response.status(), StatusCode::OK);
    std::string bytes = tudo(response);
    EXPECT_TRUE(contem(bytes, "Content-Type: application/octet-stream\r\n"));
    EXPECT_TRUE(bytes.ends_with(std::string("\r\n\r\na\0b\xff", 8)));
}

TEST_F(StaticFilesTest, ArquivoVazioDa200) {
    HttpResponse response = citius::serve_static(root, get("/zero.txt"));
    EXPECT_EQ(response.status(), StatusCode::OK);
    EXPECT_TRUE(contem(tudo(response), "Content-Length: 0\r\n"));
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
        EXPECT_FALSE(contem(tudo(response), "segredo")) << alvo;
    }
}

TEST_F(StaticFilesTest, SymlinkParaForaDa404) {
    std::error_code ec;
    fs::create_symlink(base / "secret.txt", root / "link.txt", ec);
    if (ec) GTEST_SKIP() << "sem symlink";
    HttpResponse response = citius::serve_static(root, get("/link.txt"));
    EXPECT_EQ(response.status(), StatusCode::NOT_FOUND);
    EXPECT_FALSE(contem(tudo(response), "segredo"));
}

TEST_F(StaticFilesTest, PercentMalformadoDa400) {
    EXPECT_EQ(citius::serve_static(root, get("/%zz")).status(), StatusCode::BAD_REQUEST);
    EXPECT_EQ(citius::serve_static(root, get("/a%00b")).status(), StatusCode::BAD_REQUEST);
}

TEST_F(StaticFilesTest, MetodoQueNaoEGetDa405ComAllow) {
    for (HttpMethod metodo : {HttpMethod::POST, HttpMethod::PUT, HttpMethod::DELETE}) {
        HttpResponse response = citius::serve_static(root, get("/", metodo));
        EXPECT_EQ(response.status(), StatusCode::METHOD_NOT_ALLOWED);
        EXPECT_TRUE(contem(tudo(response), "Allow: GET\r\n"));
    }
}

TEST_F(StaticFilesTest, ArquivoPequenoVaiEmMemoriaESemFdAberto) {
    HttpResponse response = citius::serve_static(root, get("/index.html"));

    EXPECT_FALSE(response.file_body().has_value());
    EXPECT_TRUE(response.serialize().ends_with("Content-Length: 11\r\n\r\n<h1>oi</h1>"));
}

TEST_F(StaticFilesTest, ArquivoAcimaDoLimiteInlineVaiComoCorpoDeArquivo) {
    const std::string medio(citius::kInlineStaticFileBytes + 1, 'm');
    escrever(root / "medio.bin", medio);

    HttpResponse response = citius::serve_static(root, get("/medio.bin"));

    ASSERT_TRUE(response.file_body().has_value());
    EXPECT_EQ(response.file_body()->size, medio.size());
    // a serializacao so tem a cabeca: o conteudo sai do descritor, em pedacos
    EXPECT_TRUE(response.serialize().ends_with("Content-Length: " + std::to_string(medio.size()) + "\r\n\r\n"));
}

TEST_F(StaticFilesTest, ArquivoNoLimiteInlineAindaVaiEmMemoria) {
    escrever(root / "limite.bin", std::string(citius::kInlineStaticFileBytes, 'l'));

    EXPECT_FALSE(citius::serve_static(root, get("/limite.bin")).file_body().has_value());
}

TEST_F(StaticFilesTest, ArquivoVazioNaoUsaCorpoDeArquivo) {
    EXPECT_FALSE(citius::serve_static(root, get("/zero.txt")).file_body().has_value());
}

// Antes o arquivo era lido inteiro para a memoria e havia um limite de 10 MB (500 acima dele).
TEST_F(StaticFilesTest, ArquivoGrandeNaoTemLimiteDeTamanho) {
    const std::string grande(12 * 1024 * 1024, 'g'); // 12 MB: passava do antigo limite
    escrever(root / "grande.bin", grande);

    HttpResponse response = citius::serve_static(root, get("/grande.bin"));

    EXPECT_EQ(response.status(), StatusCode::OK);
    ASSERT_TRUE(response.file_body().has_value());
    EXPECT_EQ(response.file_body()->size, grande.size());
}

TEST_F(StaticFilesTest, FifoDa404SemTravar) {
    ASSERT_EQ(::mkfifo((root / "pipe").c_str(), 0600), 0);

    EXPECT_EQ(citius::serve_static(root, get("/pipe")).status(), StatusCode::NOT_FOUND);
}

TEST_F(StaticFilesTest, ArquivoSemPermissaoDa403) {
    if (::geteuid() == 0) GTEST_SKIP() << "root le qualquer arquivo";
    escrever(root / "bloqueado.txt", "x");
    fs::permissions(root / "bloqueado.txt", fs::perms::none);
    EXPECT_EQ(citius::serve_static(root, get("/bloqueado.txt")).status(), StatusCode::FORBIDDEN);
}
