#include "citius/Config.hpp"
#include <gtest/gtest.h>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

using citius::Config;
using citius::ConfigResult;
using citius::LogLevel;
using citius::ParseStatus;

namespace {

ConfigResult parse(std::initializer_list<std::string_view> args) {
    std::vector<std::string_view> v{args};
    return citius::parse_config(v);
}

// Verifica que a linha de comando foi recusada e que a mensagem cita @p trecho.
void espera_erro(std::initializer_list<std::string_view> args, const std::string& trecho) {
    ConfigResult r = parse(args);
    EXPECT_EQ(r.status, ParseStatus::INVALID) << (args.size() ? std::string(*args.begin()) : "");
    EXPECT_NE(r.error.find(trecho), std::string::npos) << "erro: " << r.error;
}

} // namespace

TEST(ConfigTest, SemArgumentosUsaOsPadroes) {
    ConfigResult r = parse({});

    ASSERT_EQ(r.status, ParseStatus::OK);
    EXPECT_EQ(r.config.port, 8080);
    EXPECT_EQ(r.config.root, "www");
    EXPECT_EQ(r.config.max_connections, citius::kDefaultMaxConnections);
    EXPECT_EQ(r.config.read_timeout, citius::kDefaultReadTimeout);
    EXPECT_EQ(r.config.idle_timeout, citius::kDefaultIdleTimeout);
    EXPECT_EQ(r.config.log_level, LogLevel::INFO);
    EXPECT_TRUE(r.error.empty());
}

TEST(ConfigTest, LeTodasAsOpcoesComValorSeparado) {
    ConfigResult r = parse({"--port", "9090", "--root", "/tmp/site", "--max-connections", "16",
                            "--read-timeout-ms", "2000", "--idle-timeout-ms", "300", "--log-level", "debug"});

    ASSERT_EQ(r.status, ParseStatus::OK) << r.error;
    EXPECT_EQ(r.config.port, 9090);
    EXPECT_EQ(r.config.root, "/tmp/site");
    EXPECT_EQ(r.config.max_connections, 16u);
    EXPECT_EQ(r.config.read_timeout, std::chrono::milliseconds(2000));
    EXPECT_EQ(r.config.idle_timeout, std::chrono::milliseconds(300));
    EXPECT_EQ(r.config.log_level, LogLevel::DEBUG);
}

TEST(ConfigTest, AceitaFormaComIgual) {
    ConfigResult r = parse({"--port=9090", "--root=site", "--log-level=WARN"});

    ASSERT_EQ(r.status, ParseStatus::OK) << r.error;
    EXPECT_EQ(r.config.port, 9090);
    EXPECT_EQ(r.config.root, "site");
    EXPECT_EQ(r.config.log_level, LogLevel::WARN);
}

TEST(ConfigTest, OpcaoRepetidaVenceAUltima) {
    ConfigResult r = parse({"--port", "1", "--port", "2"});

    ASSERT_EQ(r.status, ParseStatus::OK);
    EXPECT_EQ(r.config.port, 2);
}

TEST(ConfigTest, PortaNosLimites) {
    EXPECT_EQ(parse({"--port", "0"}).config.port, 0);  // 0 = o SO escolhe
    EXPECT_EQ(parse({"--port", "65535"}).config.port, 65535);
    espera_erro({"--port", "65536"}, "--port");
    espera_erro({"--port", "99999999999999999999"}, "--port");
}

TEST(ConfigTest, NumerosInvalidosSaoRecusados) {
    for (const char* valor : {"abc", "-1", "+80", "", " 80", "80 ", "80x", "8.0", "0x50", "1e3"}) {
        ConfigResult r = parse({"--port", valor});
        EXPECT_EQ(r.status, ParseStatus::INVALID) << "valor: '" << valor << "'";
    }
}

TEST(ConfigTest, LimitesDeConexoesETimeouts) {
    EXPECT_EQ(parse({"--max-connections", "1"}).status, ParseStatus::OK);
    espera_erro({"--max-connections", "0"}, "--max-connections");
    espera_erro({"--max-connections", "1000001"}, "--max-connections");
    // as opcoes do thread pool deixaram de existir com o event loop
    espera_erro({"--threads", "4"}, "--threads");
    espera_erro({"--max-queued", "16"}, "--max-queued");
    espera_erro({"--read-timeout-ms", "0"}, "--read-timeout-ms");
    espera_erro({"--read-timeout-ms", "3600001"}, "--read-timeout-ms");
    espera_erro({"--idle-timeout-ms", "0"}, "--idle-timeout-ms");
    EXPECT_EQ(parse({"--idle-timeout-ms", "3600000"}).status, ParseStatus::OK);
}

TEST(ConfigTest, NivelDeLogInvalido) {
    espera_erro({"--log-level", "trace"}, "--log-level");
    espera_erro({"--log-level", ""}, "--log-level");
}

TEST(ConfigTest, RaizVaziaERecusada) {
    espera_erro({"--root", ""}, "--root");
    espera_erro({"--root="}, "--root");
}

TEST(ConfigTest, FaltandoValor) {
    espera_erro({"--port"}, "--port");
    espera_erro({"--root"}, "--root");
    // o proximo argumento e outra opcao, nao um valor
    espera_erro({"--port", "--root", "x"}, "--port");
}

TEST(ConfigTest, OpcaoDesconhecida) {
    espera_erro({"--porta", "80"}, "--porta");
    espera_erro({"--nada"}, "--nada");
    espera_erro({"-p", "80"}, "-p");
}

TEST(ConfigTest, ArgumentoSoltoERecusado) {
    espera_erro({"8080"}, "8080");
    espera_erro({"--port", "80", "extra"}, "extra");
}

TEST(ConfigTest, HelpPedeAUso) {
    EXPECT_EQ(parse({"--help"}).status, ParseStatus::HELP);
    EXPECT_EQ(parse({"-h"}).status, ParseStatus::HELP);
    EXPECT_EQ(parse({"--port", "9090", "--help"}).status, ParseStatus::HELP);
}

TEST(ConfigTest, ErroAntesDoHelpVence) {
    EXPECT_EQ(parse({"--port", "abc", "--help"}).status, ParseStatus::INVALID);
}

TEST(ConfigTest, HelpComValorNaoExiste) {
    EXPECT_EQ(parse({"--help=1"}).status, ParseStatus::INVALID);
}

TEST(ConfigTest, TextoDeUsoCitaTodasAsOpcoes) {
    std::string uso = citius::config_usage("citius_server");

    for (const char* opcao : {"citius_server", "--port", "--root", "--max-connections", "--read-timeout-ms",
                              "--idle-timeout-ms", "--log-level", "--help"}) {
        EXPECT_NE(uso.find(opcao), std::string::npos) << opcao;
    }
}
