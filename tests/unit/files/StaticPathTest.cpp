#include "citius/files/StaticPath.hpp"
#include <filesystem>
#include <gtest/gtest.h>
#include <string>

namespace fs = std::filesystem;
using citius::decode_target_path;

// ---------- decode_target_path ----------

TEST(DecodeTargetPathTest, CaminhoSimplesPassaIgual) {
    EXPECT_EQ(decode_target_path("/a/b.css"), "/a/b.css");
    EXPECT_EQ(decode_target_path("/"), "/");
}

TEST(DecodeTargetPathTest, RemoveQueryString) {
    EXPECT_EQ(decode_target_path("/style.css?v=1"), "/style.css");
    EXPECT_EQ(decode_target_path("/?x=%zz"), "/"); // a query nao e decodificada nem validada
}

TEST(DecodeTargetPathTest, DecodificaPercentMaiusculoEMinusculo) {
    EXPECT_EQ(decode_target_path("/%73tyle.css"), "/style.css");
    EXPECT_EQ(decode_target_path("/a%2fb"), "/a/b");
    EXPECT_EQ(decode_target_path("/%2E%2e"), "/..");
}

TEST(DecodeTargetPathTest, DecodificaUmaVezSo) {
    // %25 = '%': o resultado "%2e" e um nome de arquivo literal, nao deve virar '.'
    EXPECT_EQ(decode_target_path("/%252e"), "/%2e");
}

TEST(DecodeTargetPathTest, RejeitaPercentInvalido) {
    EXPECT_FALSE(decode_target_path("/%zz"));
    EXPECT_FALSE(decode_target_path("/%4"));
    EXPECT_FALSE(decode_target_path("/%"));
    EXPECT_FALSE(decode_target_path("/a%g1"));
}

TEST(DecodeTargetPathTest, RejeitaByteNulo) {
    EXPECT_FALSE(decode_target_path("/a%00b"));
    EXPECT_FALSE(decode_target_path(std::string("/a\0b", 4)));
}

TEST(DecodeTargetPathTest, ExigeBarraInicial) {
    EXPECT_FALSE(decode_target_path(""));
    EXPECT_FALSE(decode_target_path("index.html"));
    EXPECT_FALSE(decode_target_path("*"));
    EXPECT_FALSE(decode_target_path("http://x/y"));
}

// ---------- lexical_static_path ----------
// So texto: nao toca no disco, entao nao precisa de arquivos reais. Symlinks que escapam da raiz sao tratados (e
// testados) em open_static_file, que confere o caminho REAL do arquivo ja aberto.

using citius::lexical_static_path;

namespace {
const fs::path kRaiz = "/srv/www"; // nao precisa existir
}

TEST(LexicalStaticPathTest, RaizViraIndexHtml) {
    EXPECT_EQ(lexical_static_path(kRaiz, "/"), kRaiz / "index.html");
}

TEST(LexicalStaticPathTest, BarraFinalViraIndexDoDiretorio) {
    EXPECT_EQ(lexical_static_path(kRaiz, "/sub/"), kRaiz / "sub" / "index.html");
}

TEST(LexicalStaticPathTest, ArquivoSimplesESubdiretorio) {
    EXPECT_EQ(lexical_static_path(kRaiz, "/style.css"), kRaiz / "style.css");
    EXPECT_EQ(lexical_static_path(kRaiz, "/sub/pagina.html"), kRaiz / "sub" / "pagina.html");
}

TEST(LexicalStaticPathTest, NaoExigeQueOArquivoExista) {
    EXPECT_EQ(lexical_static_path(kRaiz, "/nada.txt"), kRaiz / "nada.txt");
}

TEST(LexicalStaticPathTest, RaizRelativaOuComBarraFinalFunciona) {
    EXPECT_EQ(lexical_static_path("www", "/style.css"), fs::path("www") / "style.css");
    EXPECT_EQ(lexical_static_path(kRaiz.string() + "/", "/style.css"), kRaiz / "style.css");
}

TEST(LexicalStaticPathTest, BarrasDuplicadasNaoViramCaminhoAbsoluto) {
    // "//etc/passwd" nao pode ser lido como /etc/passwd: vira <raiz>/etc/passwd
    EXPECT_EQ(lexical_static_path(kRaiz, "//etc/passwd"), kRaiz / "etc" / "passwd");
    EXPECT_EQ(lexical_static_path(kRaiz, "/./style.css"), kRaiz / "style.css");
}

TEST(LexicalStaticPathTest, RejeitaPontoPonto) {
    EXPECT_FALSE(lexical_static_path(kRaiz, "/../secret.txt"));
    EXPECT_FALSE(lexical_static_path(kRaiz, "/sub/../../secret.txt"));
    EXPECT_FALSE(lexical_static_path(kRaiz, "/sub/../style.css")); // conservador: nenhum ".." e aceito
    EXPECT_FALSE(lexical_static_path(kRaiz, "/.."));
    EXPECT_FALSE(lexical_static_path(kRaiz, "/../www/style.css"));
}

TEST(LexicalStaticPathTest, RejeitaPontoPontoCodificado) {
    for (const char* target : {"/%2e%2e/secret.txt", "/%2E%2E/secret.txt", "/%2e%2e%2fsecret.txt",
                               "/sub/%2e%2e/%2e%2e/secret.txt", "/..%2fsecret.txt"}) {
        auto decoded = decode_target_path(target);
        ASSERT_TRUE(decoded) << target;
        EXPECT_FALSE(lexical_static_path(kRaiz, *decoded)) << target;
    }
}

TEST(LexicalStaticPathTest, PercentDuploFicaDentroDaRaiz) {
    auto decoded = decode_target_path("/%252e%252e/secret.txt");
    ASSERT_TRUE(decoded);
    EXPECT_EQ(lexical_static_path(kRaiz, *decoded), kRaiz / "%2e%2e" / "secret.txt");
}

TEST(LexicalStaticPathTest, RejeitaNuloEEntradaSemBarra) {
    EXPECT_FALSE(lexical_static_path(kRaiz, std::string("/a\0b", 4)));
    EXPECT_FALSE(lexical_static_path(kRaiz, ""));
    EXPECT_FALSE(lexical_static_path(kRaiz, "style.css"));
}

TEST(LexicalStaticPathTest, CaminhoGiganteNaoQuebra) {
    std::string huge = "/" + std::string(100000, 'a');

    auto result = lexical_static_path(kRaiz, huge);

    // nunca lanca nem escapa: fica diretamente dentro da raiz
    ASSERT_TRUE(result);
    EXPECT_EQ(result->parent_path(), kRaiz);
}
