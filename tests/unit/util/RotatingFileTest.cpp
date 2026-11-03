#include "citius/util/RotatingFile.hpp"
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <iterator>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;
using citius::RotatingFile;

namespace {

std::string ler(const fs::path& path) {
    std::ifstream in{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

} // namespace

class RotatingFileTest : public ::testing::Test {
protected:
    void SetUp() override {
        dir = fs::temp_directory_path() / ("citius_rotating_" + std::to_string(::getpid()) + "_" +
                                           ::testing::UnitTest::GetInstance()->current_test_info()->name());
        fs::remove_all(dir);
        fs::create_directories(dir);
        path = dir / "app.log";
    }

    void TearDown() override {
        std::error_code ignorado;
        fs::remove_all(dir, ignorado);
    }

    [[nodiscard]] fs::path numerado(int n) const {
        return fs::path(path.string() + "." + std::to_string(n));
    }

    fs::path dir;
    fs::path path;
};

TEST_F(RotatingFileTest, CriaOArquivoEAcrescentaLinhas) {
    RotatingFile file{path, 1024, 3};

    EXPECT_TRUE(file.write("primeira\n"));
    EXPECT_TRUE(file.write("segunda\n"));

    EXPECT_EQ(ler(path), "primeira\nsegunda\n");
}

TEST_F(RotatingFileTest, AcrescentaAoQueJaExistia) {
    {
        std::ofstream{path} << "antigo\n";
    }

    RotatingFile file{path, 1024, 3};
    EXPECT_TRUE(file.write("novo\n"));

    EXPECT_EQ(ler(path), "antigo\nnovo\n"); // nao apaga o que ja estava la
}

TEST_F(RotatingFileTest, ConteudoPreexistenteContaParaOLimite) {
    {
        std::ofstream{path} << std::string(90, 'a');
    } // ja quase no limite de 100

    RotatingFile file{path, 100, 3};
    EXPECT_TRUE(file.write(std::string(20, 'b')));

    EXPECT_EQ(ler(numerado(1)), std::string(90, 'a')); // o antigo foi rotacionado antes de escrever
    EXPECT_EQ(ler(path), std::string(20, 'b'));
}

TEST_F(RotatingFileTest, RotacionaQuandoAProximaLinhaEstouraOLimite) {
    RotatingFile file{path, 30, 3};

    EXPECT_TRUE(file.write("0123456789\n")); // 11
    EXPECT_TRUE(file.write("abcdefghij\n")); // 22
    EXPECT_TRUE(file.write("KLMNOPQRST\n")); // 33 > 30: rotaciona antes desta

    EXPECT_EQ(ler(numerado(1)), "0123456789\nabcdefghij\n");
    EXPECT_EQ(ler(path), "KLMNOPQRST\n");
}

TEST_F(RotatingFileTest, NaoPartaUmaLinhaAoMeio) {
    RotatingFile file{path, 25, 3};
    for (int i = 0; i < 6; ++i) EXPECT_TRUE(file.write("linha-" + std::to_string(i) + "-xx\n")); // 10 bytes cada

    // toda linha esta inteira em algum arquivo: nenhum termina sem "\n" e nenhum comeca no meio
    for (const fs::path& p : {path, numerado(1), numerado(2)}) {
        const std::string conteudo = ler(p);
        if (conteudo.empty()) continue;
        EXPECT_TRUE(conteudo.starts_with("linha-")) << p;
        EXPECT_TRUE(conteudo.ends_with("\n")) << p;
    }
}

TEST_F(RotatingFileTest, GuardaNoMaximoOsArquivosAntigosPedidos) {
    RotatingFile file{path, 10, 2};
    for (int i = 0; i < 8; ++i)
        EXPECT_TRUE(file.write("linha" + std::to_string(i) + "\n")); // 6 bytes: 1 por arquivo + sobra

    EXPECT_TRUE(fs::exists(path));
    EXPECT_TRUE(fs::exists(numerado(1)));
    EXPECT_TRUE(fs::exists(numerado(2)));
    EXPECT_FALSE(fs::exists(numerado(3))); // o mais velho foi apagado
}

TEST_F(RotatingFileTest, OsMaisAntigosVaoParaNumerosMaiores) {
    RotatingFile file{path, 10, 3};
    for (int i = 0; i < 4; ++i) EXPECT_TRUE(file.write("linha" + std::to_string(i) + "\n"));

    // cada linha ocupa um arquivo: a mais recente no corrente e a mais velha com o maior numero
    EXPECT_EQ(ler(path), "linha3\n");
    EXPECT_EQ(ler(numerado(1)), "linha2\n");
    EXPECT_EQ(ler(numerado(2)), "linha1\n");
    EXPECT_EQ(ler(numerado(3)), "linha0\n");
}

TEST_F(RotatingFileTest, KeepZeroNaoGuardaAntigosSoRecomeca) {
    RotatingFile file{path, 10, 0};
    EXPECT_TRUE(file.write("linha0\n"));
    EXPECT_TRUE(file.write("linha1\n"));

    EXPECT_EQ(ler(path), "linha1\n");
    EXPECT_FALSE(fs::exists(numerado(1)));
}

TEST_F(RotatingFileTest, LinhaMaiorQueOLimiteEscritaMesmoAssimSemLacoInfinito) {
    RotatingFile file{path, 10, 2};

    EXPECT_TRUE(file.write(std::string(100, 'x') + "\n"));
    EXPECT_TRUE(file.write("y\n")); // rotaciona e escreve normalmente

    EXPECT_EQ(ler(numerado(1)), std::string(100, 'x') + "\n");
    EXPECT_EQ(ler(path), "y\n");
}

TEST_F(RotatingFileTest, PastaInexistenteLancaNaCriacao) {
    EXPECT_THROW((RotatingFile{dir / "nao-existe" / "app.log", 100, 2}), std::runtime_error);
}

TEST_F(RotatingFileTest, ExposeCaminhoELimites) {
    RotatingFile file{path, 4096, 7};

    EXPECT_EQ(file.path(), path);
    EXPECT_EQ(file.max_bytes(), 4096u);
    EXPECT_EQ(file.keep(), 7u);
}
