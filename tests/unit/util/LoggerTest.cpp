#include "citius/util/Logger.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <iterator>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

using citius::Logger;
using citius::LogLevel;

namespace {

// Logger de teste: escreve em dois ostringstream em vez de stdout/stderr.
struct LoggerDeTeste {
    explicit LoggerDeTeste(LogLevel nivel = LogLevel::DEBUG) : logger{out, err, nivel} {}

    std::ostringstream out;
    std::ostringstream err;
    Logger logger;
};

} // namespace

TEST(LoggerTest, LinhaTemHoraNivelEMensagem) {
    LoggerDeTeste t;

    t.logger.log(LogLevel::INFO, "GET / -> 200");

    std::regex formato{R"(^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2} \[INFO\] GET / -> 200\n$)"};
    EXPECT_TRUE(std::regex_match(t.out.str(), formato)) << t.out.str();
}

TEST(LoggerTest, CadaNivelTemSeuNome) {
    EXPECT_EQ(citius::level_name(LogLevel::DEBUG), "DEBUG");
    EXPECT_EQ(citius::level_name(LogLevel::INFO), "INFO");
    EXPECT_EQ(citius::level_name(LogLevel::WARN), "WARN");
    EXPECT_EQ(citius::level_name(LogLevel::ERROR), "ERROR");
}

TEST(LoggerTest, DebugEInfoVaoParaStdoutWarnEErrorParaStderr) {
    LoggerDeTeste t;

    t.logger.log(LogLevel::DEBUG, "d");
    t.logger.log(LogLevel::INFO, "i");
    t.logger.log(LogLevel::WARN, "w");
    t.logger.log(LogLevel::ERROR, "e");

    EXPECT_NE(t.out.str().find("[DEBUG] d"), std::string::npos);
    EXPECT_NE(t.out.str().find("[INFO] i"), std::string::npos);
    EXPECT_EQ(t.out.str().find("[WARN]"), std::string::npos);
    EXPECT_NE(t.err.str().find("[WARN] w"), std::string::npos);
    EXPECT_NE(t.err.str().find("[ERROR] e"), std::string::npos);
    EXPECT_EQ(t.err.str().find("[INFO]"), std::string::npos);
}

TEST(LoggerTest, NivelMinimoFiltraOsMenosImportantes) {
    LoggerDeTeste t{LogLevel::WARN};

    t.logger.log(LogLevel::DEBUG, "d");
    t.logger.log(LogLevel::INFO, "i");
    t.logger.log(LogLevel::WARN, "w");
    t.logger.log(LogLevel::ERROR, "e");

    EXPECT_TRUE(t.out.str().empty());
    EXPECT_NE(t.err.str().find("[WARN] w"), std::string::npos);
    EXPECT_NE(t.err.str().find("[ERROR] e"), std::string::npos);
}

TEST(LoggerTest, PadraoEInfo) {
    std::ostringstream out, err;
    Logger logger{out, err};

    EXPECT_EQ(logger.level(), LogLevel::INFO);
    EXPECT_FALSE(logger.enabled(LogLevel::DEBUG));
    EXPECT_TRUE(logger.enabled(LogLevel::INFO));
}

TEST(LoggerTest, MudarONivelEmExecucaoValeNaProximaLinha) {
    LoggerDeTeste t{LogLevel::ERROR};
    t.logger.log(LogLevel::INFO, "antes");

    t.logger.set_level(LogLevel::INFO);
    t.logger.log(LogLevel::INFO, "depois");

    EXPECT_EQ(t.out.str().find("antes"), std::string::npos);
    EXPECT_NE(t.out.str().find("depois"), std::string::npos);
}

TEST(LoggerTest, CaracteresDeControleNaoQuebramALinha) {
    // um valor vindo do cliente com \n poderia forjar uma linha de log falsa (log injection)
    LoggerDeTeste t;

    t.logger.log(LogLevel::INFO, "a\nb\r[ERROR] falso\x01\x7f"
                                 "fim\ttab");

    std::string saida = t.out.str();
    EXPECT_EQ(std::count(saida.begin(), saida.end(), '\n'), 1) << saida;
    EXPECT_EQ(saida.find('\r'), std::string::npos);
    EXPECT_NE(saida.find("a?b?[ERROR] falso??fim\ttab"), std::string::npos) << saida;
}

TEST(LoggerTest, MensagemVaziaEValida) {
    LoggerDeTeste t;

    t.logger.log(LogLevel::INFO, "");

    EXPECT_TRUE(std::regex_search(t.out.str(), std::regex{R"(\[INFO\] \n$)"})) << t.out.str();
}

TEST(LoggerTest, ThreadsNaoMisturamLinhas) {
    LoggerDeTeste t;
    constexpr int kThreads = 8;
    constexpr int kLinhas = 200;

    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&t, i] {
            for (int j = 0; j < kLinhas; ++j) {
                t.logger.log(LogLevel::INFO, "thread " + std::to_string(i) + " linha " + std::to_string(j));
            }
        });
    }
    for (auto& thread : threads) thread.join();

    // cada linha precisa estar inteira: hora, nivel e a mensagem completa
    std::regex formato{R"(^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2} \[INFO\] thread \d+ linha \d+$)"};
    std::istringstream entrada{t.out.str()};
    std::string linha;
    int total = 0;
    while (std::getline(entrada, linha)) {
        EXPECT_TRUE(std::regex_match(linha, formato)) << linha;
        ++total;
    }
    EXPECT_EQ(total, kThreads * kLinhas);
}

TEST(LoggerTest, InstanciaGlobalEUnica) {
    EXPECT_EQ(&Logger::instance(), &Logger::instance());
}

// ---- parse_log_level (usado pela linha de comando) ----

TEST(LogLevelTest, ReconheceOsNomesIgnorandoMaiusculas) {
    EXPECT_EQ(citius::parse_log_level("debug"), LogLevel::DEBUG);
    EXPECT_EQ(citius::parse_log_level("INFO"), LogLevel::INFO);
    EXPECT_EQ(citius::parse_log_level("Warn"), LogLevel::WARN);
    EXPECT_EQ(citius::parse_log_level("error"), LogLevel::ERROR);
}

TEST(LogLevelTest, RejeitaNomesInvalidos) {
    for (const char* nome : {"", "trace", "warning", "info ", " info", "inf", "0", "debug\n"}) {
        EXPECT_FALSE(citius::parse_log_level(nome).has_value()) << nome;
    }
}

// ---- log em arquivo com rotacao --------------------------------------------------------------------------------

namespace {

namespace fs = std::filesystem;

std::string ler_arquivo(const fs::path& path) {
    std::ifstream in{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

// Pasta temporaria por teste; remove tudo no fim.
struct PastaDeLog {
    PastaDeLog() {
        dir = fs::temp_directory_path() / ("citius_logfile_" + std::to_string(::getpid()) + "_" +
                                           ::testing::UnitTest::GetInstance()->current_test_info()->name());
        fs::remove_all(dir);
        fs::create_directories(dir);
        arquivo = dir / "app.log";
    }
    ~PastaDeLog() {
        std::error_code ignorado;
        fs::remove_all(dir, ignorado);
    }
    PastaDeLog(const PastaDeLog&) = delete;
    PastaDeLog& operator=(const PastaDeLog&) = delete;

    fs::path dir;
    fs::path arquivo;
};

} // namespace

TEST(LoggerArquivoTest, TodosOsNiveisVaoParaOArquivoENadaParaOConsole) {
    PastaDeLog pasta;
    LoggerDeTeste t;
    t.logger.log_to_file(pasta.arquivo);

    t.logger.log(LogLevel::DEBUG, "d");
    t.logger.log(LogLevel::INFO, "i");
    t.logger.log(LogLevel::WARN, "w");
    t.logger.log(LogLevel::ERROR, "e");

    const std::string conteudo = ler_arquivo(pasta.arquivo);
    for (const char* nivel : {"[DEBUG] d", "[INFO] i", "[WARN] w", "[ERROR] e"}) {
        EXPECT_NE(conteudo.find(nivel), std::string::npos) << nivel;
    }
    EXPECT_TRUE(t.out.str().empty()); // com arquivo, o console fica quieto (o servidor roda em segundo plano)
    EXPECT_TRUE(t.err.str().empty());
}

TEST(LoggerArquivoTest, LinhaDoArquivoTemOMesmoFormatoDoConsole) {
    PastaDeLog pasta;
    LoggerDeTeste t;
    t.logger.log_to_file(pasta.arquivo);

    t.logger.log(LogLevel::INFO, "GET / -> 200");

    std::regex formato{R"(^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2} \[INFO\] GET / -> 200\n$)"};
    EXPECT_TRUE(std::regex_match(ler_arquivo(pasta.arquivo), formato));
}

TEST(LoggerArquivoTest, NivelMinimoEControleDeLinhaValemParaOArquivo) {
    PastaDeLog pasta;
    LoggerDeTeste t{LogLevel::WARN};
    t.logger.log_to_file(pasta.arquivo);

    t.logger.log(LogLevel::INFO, "abaixo do nivel");
    t.logger.log(LogLevel::ERROR, "falha\nforjada [INFO] linha falsa");

    const std::string conteudo = ler_arquivo(pasta.arquivo);
    EXPECT_EQ(conteudo.find("abaixo do nivel"), std::string::npos);
    EXPECT_NE(conteudo.find("falha?forjada [INFO] linha falsa"), std::string::npos); // "\n" do cliente virou "?"
    EXPECT_EQ(std::count(conteudo.begin(), conteudo.end(), '\n'), 1);                // uma linha so
}

TEST(LoggerArquivoTest, RotacionaPeloTamanho) {
    PastaDeLog pasta;
    LoggerDeTeste t;
    t.logger.log_to_file(pasta.arquivo, 200, 2);

    for (int i = 0; i < 20; ++i) t.logger.log(LogLevel::INFO, "mensagem numero " + std::to_string(i));

    EXPECT_TRUE(fs::exists(pasta.arquivo));
    EXPECT_TRUE(fs::exists(fs::path(pasta.arquivo.string() + ".1")));
    EXPECT_TRUE(fs::exists(fs::path(pasta.arquivo.string() + ".2")));
    EXPECT_FALSE(fs::exists(fs::path(pasta.arquivo.string() + ".3")));
    EXPECT_LE(fs::file_size(pasta.arquivo), 200u + 60u); // o corrente nunca passa muito do limite
}

TEST(LoggerArquivoTest, ArquivoInvalidoLancaEOLoggerContinuaNoConsole) {
    PastaDeLog pasta;
    LoggerDeTeste t;

    EXPECT_THROW(t.logger.log_to_file(pasta.dir / "nao-existe" / "app.log"), std::runtime_error);

    t.logger.log(LogLevel::INFO, "ainda no console");
    EXPECT_NE(t.out.str().find("ainda no console"), std::string::npos);
}

// Nunca perder uma linha em silencio: se o arquivo falhar, a mensagem vai para o stderr.
TEST(LoggerArquivoTest, FalhaNaEscritaCaiParaOStderr) {
    PastaDeLog pasta;
    LoggerDeTeste t;
    t.logger.log_to_file(pasta.arquivo, 10, 1);
    t.logger.log(LogLevel::INFO, "primeira"); // cabe no arquivo
    ASSERT_TRUE(t.err.str().empty());

    fs::remove_all(pasta.dir); // a pasta some: a proxima rotacao nao consegue recriar o arquivo
    t.logger.log(LogLevel::ERROR, "perdida?");

    EXPECT_NE(t.err.str().find("perdida?"), std::string::npos);
}

TEST(LoggerArquivoTest, VariasThreadsNaoMisturamLinhas) {
    PastaDeLog pasta;
    LoggerDeTeste t;
    t.logger.log_to_file(pasta.arquivo, 100 * 1024 * 1024, 1); // sem rotacao no teste

    constexpr int kThreads = 8;
    constexpr int kLinhas = 250;
    std::vector<std::thread> threads;
    for (int th = 0; th < kThreads; ++th) {
        threads.emplace_back([&t, th] {
            for (int i = 0; i < kLinhas; ++i)
                t.logger.log(LogLevel::INFO, "thread " + std::to_string(th) + " linha " + std::to_string(i));
        });
    }
    for (auto& th : threads) th.join();

    std::istringstream linhas{ler_arquivo(pasta.arquivo)};
    std::regex formato{R"(^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2} \[INFO\] thread \d+ linha \d+$)"};
    int total = 0;
    for (std::string linha; std::getline(linhas, linha);) {
        EXPECT_TRUE(std::regex_match(linha, formato)) << linha;
        ++total;
    }
    EXPECT_EQ(total, kThreads * kLinhas);
}
