#include "citius/StaticPath.hpp"
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <string>

namespace fs = std::filesystem;
using citius::decode_target_path;
using citius::resolve_static_path;

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

// ---------- resolve_static_path ----------

class ResolveStaticPathTest : public ::testing::Test {
protected:
    void SetUp() override {
        base = fs::temp_directory_path() /
               ("citius_staticpath_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) + "_" +
                ::testing::UnitTest::GetInstance()->current_test_info()->name());
        fs::remove_all(base);
        fs::create_directories(base / "www" / "sub");
        touch(base / "www" / "index.html");
        touch(base / "www" / "style.css");
        touch(base / "www" / "sub" / "index.html");
        touch(base / "secret.txt"); // fora da raiz publica
        std::error_code ec;
        fs::create_symlink(base / "secret.txt", base / "www" / "link.txt", ec);
        root = base / "www";
        canonical_root = fs::canonical(root); // no macOS /var e link para /private/var
    }

    void TearDown() override { fs::remove_all(base); }

    static void touch(const fs::path& path) { std::ofstream{path} << "x"; }

    fs::path base;
    fs::path root;
    fs::path canonical_root;
};

TEST_F(ResolveStaticPathTest, RaizViraIndexHtml) {
    EXPECT_EQ(resolve_static_path(root, "/"), canonical_root / "index.html");
}

TEST_F(ResolveStaticPathTest, BarraFinalViraIndexDoDiretorio) {
    EXPECT_EQ(resolve_static_path(root, "/sub/"), canonical_root / "sub" / "index.html");
}

TEST_F(ResolveStaticPathTest, ArquivoExistente) {
    EXPECT_EQ(resolve_static_path(root, "/style.css"), canonical_root / "style.css");
}

TEST_F(ResolveStaticPathTest, ArquivoInexistenteDentroDaRaizResolve) {
    // quem decide 404 e quem le o arquivo; aqui so garantimos que esta dentro da raiz
    EXPECT_EQ(resolve_static_path(root, "/nada.txt"), canonical_root / "nada.txt");
}

TEST_F(ResolveStaticPathTest, RaizComBarraFinalFunciona) {
    EXPECT_EQ(resolve_static_path(root.string() + "/", "/style.css"), canonical_root / "style.css");
}

TEST_F(ResolveStaticPathTest, BarrasDuplicadasNaoViramCaminhoAbsoluto) {
    // "//etc/passwd" nao pode ser lido como /etc/passwd: vira www/etc/passwd
    EXPECT_EQ(resolve_static_path(root, "//etc/passwd"), canonical_root / "etc" / "passwd");
    EXPECT_EQ(resolve_static_path(root, "/./style.css"), canonical_root / "style.css");
}

TEST_F(ResolveStaticPathTest, RejeitaPontoPonto) {
    EXPECT_FALSE(resolve_static_path(root, "/../secret.txt"));
    EXPECT_FALSE(resolve_static_path(root, "/sub/../../secret.txt"));
    EXPECT_FALSE(resolve_static_path(root, "/sub/../style.css")); // conservador: nenhum ".." e aceito
    EXPECT_FALSE(resolve_static_path(root, "/.."));
    EXPECT_FALSE(resolve_static_path(root, "/../www/style.css"));
}

TEST_F(ResolveStaticPathTest, RejeitaPontoPontoCodificado) {
    for (const char* target : {"/%2e%2e/secret.txt", "/%2E%2E/secret.txt", "/%2e%2e%2fsecret.txt",
                               "/sub/%2e%2e/%2e%2e/secret.txt", "/..%2fsecret.txt"}) {
        auto decoded = decode_target_path(target);
        ASSERT_TRUE(decoded) << target;
        EXPECT_FALSE(resolve_static_path(root, *decoded)) << target;
    }
}

TEST_F(ResolveStaticPathTest, PercentDuploFicaDentroDaRaiz) {
    auto decoded = decode_target_path("/%252e%252e/secret.txt");
    ASSERT_TRUE(decoded);
    EXPECT_EQ(resolve_static_path(root, *decoded), canonical_root / "%2e%2e" / "secret.txt");
}

TEST_F(ResolveStaticPathTest, RejeitaNuloEEntradaSemBarra) {
    EXPECT_FALSE(resolve_static_path(root, std::string("/a\0b", 4)));
    EXPECT_FALSE(resolve_static_path(root, ""));
    EXPECT_FALSE(resolve_static_path(root, "style.css"));
}

TEST_F(ResolveStaticPathTest, SymlinkParaForaDaRaizERejeitado) {
    if (!fs::is_symlink(root / "link.txt")) {
        GTEST_SKIP() << "sistema nao permitiu criar symlink";
    }
    EXPECT_FALSE(resolve_static_path(root, "/link.txt"));
}

TEST_F(ResolveStaticPathTest, CaminhoGiganteNaoQuebra) {
    std::string huge = "/" + std::string(100000, 'a');
    auto result = resolve_static_path(root, huge);
    // ou recusa, ou (se o SO aceitar) fica dentro da raiz; nunca lanca nem escapa
    if (result) {
        EXPECT_EQ(result->parent_path(), canonical_root);
    }
}
