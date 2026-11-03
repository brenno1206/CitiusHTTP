#include "citius/server/Config.hpp"
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <initializer_list>
#include <string>
#include <string_view>
#include <unistd.h>
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
    EXPECT_EQ(r.config.max_requests, citius::kMaxRequestsPerConnection);
    EXPECT_EQ(r.config.read_timeout, citius::kDefaultReadTimeout);
    EXPECT_EQ(r.config.idle_timeout, citius::kDefaultIdleTimeout);
    EXPECT_EQ(r.config.log_level, LogLevel::INFO);
    EXPECT_TRUE(r.error.empty());
}

TEST(ConfigTest, LeTodasAsOpcoesComValorSeparado) {
    ConfigResult r = parse({"--port", "9090", "--root", "/tmp/site", "--max-connections", "16", "--max-requests", "25",
                            "--read-timeout-ms", "2000", "--idle-timeout-ms", "300", "--log-level", "debug"});

    ASSERT_EQ(r.status, ParseStatus::OK) << r.error;
    EXPECT_EQ(r.config.port, 9090);
    EXPECT_EQ(r.config.root, "/tmp/site");
    EXPECT_EQ(r.config.max_connections, 16u);
    EXPECT_EQ(r.config.max_requests, 25u);
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
    EXPECT_EQ(parse({"--port", "0"}).config.port, 0); // 0 = o SO escolhe
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
    EXPECT_EQ(parse({"--max-requests", "1"}).status, ParseStatus::OK);
    espera_erro({"--max-requests", "0"}, "--max-requests");
    espera_erro({"--max-requests", "1000001"}, "--max-requests");
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

    for (const char* opcao : {"citius_server", "--port", "--root", "--max-connections", "--max-requests",
                              "--read-timeout-ms", "--idle-timeout-ms", "--log-level", "--help"}) {
        EXPECT_NE(uso.find(opcao), std::string::npos) << opcao;
    }
}

// ---- opcoes de log -----------------------------------------------------------------------------------------------

TEST(ConfigTest, LogEmArquivoVemDesligadoPorPadrao) {
    ConfigResult r = parse({});

    EXPECT_FALSE(r.config.log_file.has_value());
    EXPECT_EQ(r.config.log_max_bytes, citius::kDefaultLogMaxBytes);
    EXPECT_EQ(r.config.log_keep, citius::kDefaultLogKeep);
}

TEST(ConfigTest, LeAsOpcoesDeLogEmArquivo) {
    ConfigResult r = parse({"--log-file", "/var/log/citius.log", "--log-max-bytes", "2048", "--log-keep", "3"});

    ASSERT_EQ(r.status, ParseStatus::OK) << r.error;
    ASSERT_TRUE(r.config.log_file.has_value());
    EXPECT_EQ(*r.config.log_file, "/var/log/citius.log");
    EXPECT_EQ(r.config.log_max_bytes, 2048u);
    EXPECT_EQ(r.config.log_keep, 3u);
}

TEST(ConfigTest, LimitesDoLogEmArquivo) {
    espera_erro({"--log-file", ""}, "--log-file");
    EXPECT_EQ(parse({"--log-max-bytes", "1024"}).status, ParseStatus::OK);
    espera_erro({"--log-max-bytes", "1023"}, "--log-max-bytes");
    EXPECT_EQ(parse({"--log-max-bytes", "1073741824"}).status, ParseStatus::OK); // 1 GiB
    espera_erro({"--log-max-bytes", "1073741825"}, "--log-max-bytes");
    EXPECT_EQ(parse({"--log-keep", "0"}).status, ParseStatus::OK); // 0 = nao guardar antigos
    espera_erro({"--log-keep", "1001"}, "--log-keep");
}

// ---- arquivo de configuracao (--config) -------------------------------------------------------------------------

namespace {

// Escreve um arquivo de configuracao temporario (removido no destrutor) e devolve o caminho.
class ArquivoDeConfig {
public:
    explicit ArquivoDeConfig(const std::string& conteudo)
        : m_path(std::filesystem::temp_directory_path() /
                 ("citius_cfg_" + std::to_string(::getpid()) + "_" +
                  ::testing::UnitTest::GetInstance()->current_test_info()->name() + ".conf")) {
        std::ofstream{m_path, std::ios::binary} << conteudo;
    }
    ~ArquivoDeConfig() {
        std::error_code ignorado;
        std::filesystem::remove(m_path, ignorado);
    }
    ArquivoDeConfig(const ArquivoDeConfig&) = delete;
    ArquivoDeConfig& operator=(const ArquivoDeConfig&) = delete;

    [[nodiscard]] std::string path() const {
        return m_path.string();
    }

private:
    std::filesystem::path m_path;
};

} // namespace

TEST(ConfigArquivoTest, ArquivoDefineAsOpcoes) {
    ArquivoDeConfig arquivo{
        "port = 9000\nroot = /srv/site\nmax-connections = 500\nmax-requests = 50\nread-timeout-ms = 2000\n"
        "idle-timeout-ms = 700\nlog-level = debug\nlog-file = /tmp/citius.log\nlog-max-bytes = 4096\nlog-keep = 2\n"};

    ConfigResult r = parse({"--config", arquivo.path()});

    ASSERT_EQ(r.status, ParseStatus::OK) << r.error;
    EXPECT_EQ(r.config.port, 9000);
    EXPECT_EQ(r.config.root, "/srv/site");
    EXPECT_EQ(r.config.max_connections, 500u);
    EXPECT_EQ(r.config.max_requests, 50u);
    EXPECT_EQ(r.config.read_timeout, std::chrono::milliseconds(2000));
    EXPECT_EQ(r.config.idle_timeout, std::chrono::milliseconds(700));
    EXPECT_EQ(r.config.log_level, LogLevel::DEBUG);
    ASSERT_TRUE(r.config.log_file.has_value());
    EXPECT_EQ(*r.config.log_file, "/tmp/citius.log");
    EXPECT_EQ(r.config.log_max_bytes, 4096u);
    EXPECT_EQ(r.config.log_keep, 2u);
}

TEST(ConfigArquivoTest, AceitaFormaComIgual) {
    ArquivoDeConfig arquivo{"port = 9001\n"};

    ConfigResult r = parse({std::string("--config=") + arquivo.path()});

    ASSERT_EQ(r.status, ParseStatus::OK) << r.error;
    EXPECT_EQ(r.config.port, 9001);
}

TEST(ConfigArquivoTest, ComentariosLinhasEmBrancoEspacosECrLf) {
    ArquivoDeConfig arquivo{"# configuracao do servidor\r\n\r\n   # comentario indentado\r\nport=9002\r\n   root    =  "
                            "  /com espaco/pasta   \r\n\t\n"};

    ConfigResult r = parse({"--config", arquivo.path()});

    ASSERT_EQ(r.status, ParseStatus::OK) << r.error;
    EXPECT_EQ(r.config.port, 9002);
    EXPECT_EQ(r.config.root, "/com espaco/pasta"); // o valor e o resto da linha, sem as pontas
}

TEST(ConfigArquivoTest, ArquivoVazioMantemOsPadroes) {
    ArquivoDeConfig arquivo{""};

    ConfigResult r = parse({"--config", arquivo.path()});

    ASSERT_EQ(r.status, ParseStatus::OK) << r.error;
    EXPECT_EQ(r.config.port, 8080);
}

TEST(ConfigArquivoTest, LinhaDeComandoVenceOArquivo) {
    ArquivoDeConfig arquivo{"port = 9000\nroot = /do/arquivo\n"};

    ConfigResult r = parse({"--config", arquivo.path(), "--port", "9100"});

    ASSERT_EQ(r.status, ParseStatus::OK) << r.error;
    EXPECT_EQ(r.config.port, 9100);          // a linha de comando venceu
    EXPECT_EQ(r.config.root, "/do/arquivo"); // o que ela nao mexeu vem do arquivo
}

TEST(ConfigArquivoTest, LinhaDeComandoVenceMesmoQuandoVemAntesDoConfig) {
    ArquivoDeConfig arquivo{"port = 9000\n"};

    ConfigResult r = parse({"--port", "9100", "--config", arquivo.path()});

    ASSERT_EQ(r.status, ParseStatus::OK) << r.error;
    EXPECT_EQ(r.config.port, 9100); // a ordem dos argumentos nao muda quem tem prioridade
}

TEST(ConfigArquivoTest, ChaveRepetidaNoArquivoValeAUltima) {
    ArquivoDeConfig arquivo{"port = 9000\nport = 9003\n"};

    EXPECT_EQ(parse({"--config", arquivo.path()}).config.port, 9003);
}

TEST(ConfigArquivoTest, ArquivoInexistenteDaErroCitandoOCaminho) {
    espera_erro({"--config", "/nao/existe/citius.conf"}, "/nao/existe/citius.conf");
}

TEST(ConfigArquivoTest, ConfigSemValor) {
    espera_erro({"--config"}, "falta o valor de --config");
    espera_erro({"--config", "--port", "80"}, "falta o valor de --config");
}

TEST(ConfigArquivoTest, ErrosCitamArquivoELinha) {
    ArquivoDeConfig arquivo{"port = 9000\nfoo = 1\n"};

    ConfigResult r = parse({"--config", arquivo.path()});

    ASSERT_EQ(r.status, ParseStatus::INVALID);
    EXPECT_NE(r.error.find(arquivo.path() + ":2:"), std::string::npos) << r.error;
    EXPECT_NE(r.error.find("foo"), std::string::npos) << r.error;
}

TEST(ConfigArquivoTest, LinhaSemIgualDaErroNaLinhaCerta) {
    ArquivoDeConfig arquivo{"# ok\nport = 9000\nlixo sem igual\n"};

    ConfigResult r = parse({"--config", arquivo.path()});

    ASSERT_EQ(r.status, ParseStatus::INVALID);
    EXPECT_NE(r.error.find(arquivo.path() + ":3:"), std::string::npos) << r.error;
}

TEST(ConfigArquivoTest, ValorInvalidoDaErroNaLinhaCerta) {
    ArquivoDeConfig arquivo{"port = 9000\nmax-connections = muitas\n"};

    ConfigResult r = parse({"--config", arquivo.path()});

    ASSERT_EQ(r.status, ParseStatus::INVALID);
    EXPECT_NE(r.error.find(arquivo.path() + ":2:"), std::string::npos) << r.error;
    EXPECT_NE(r.error.find("max-connections"), std::string::npos) << r.error;
}

TEST(ConfigArquivoTest, ChaveVaziaOuSoComIgualDaErro) {
    ArquivoDeConfig arquivo{"= 9000\n"};

    EXPECT_EQ(parse({"--config", arquivo.path()}).status, ParseStatus::INVALID);
}

TEST(ConfigArquivoTest, ConfigDentroDoArquivoNaoPode) {
    ArquivoDeConfig arquivo{"config = /outro.conf\n"};

    ConfigResult r = parse({"--config", arquivo.path()});

    ASSERT_EQ(r.status, ParseStatus::INVALID);
    EXPECT_NE(r.error.find("config"), std::string::npos) << r.error; // evitaria laco e inclusoes encadeadas
}

TEST(ConfigArquivoTest, CaractereDeControleNaLinhaDaErro) {
    ArquivoDeConfig arquivo{std::string("root = /pasta\x01ruim\n")};

    EXPECT_EQ(parse({"--config", arquivo.path()}).status, ParseStatus::INVALID);
}

TEST(ConfigArquivoTest, ArquivoGrandeDemaisEhRecusado) {
    ArquivoDeConfig arquivo{"# " + std::string(70 * 1024, 'x') + "\n"}; // 70 KB: acima do limite de 64 KB

    ConfigResult r = parse({"--config", arquivo.path()});

    ASSERT_EQ(r.status, ParseStatus::INVALID);
    EXPECT_NE(r.error.find("grande demais"), std::string::npos) << r.error;
}

TEST(ConfigArquivoTest, HelpFuncionaMesmoComConfigQuebrado) {
    // o arquivo so e lido depois de varrer a linha de comando: --help nunca fica escondido por um erro de arquivo
    ConfigResult r = parse({"--config", "/nao/existe.conf", "--help"});

    EXPECT_EQ(r.status, ParseStatus::HELP);
}

TEST(ConfigArquivoTest, ErroNaLinhaDeComandoApareceAntesDoArquivo) {
    espera_erro({"--port", "abc", "--config", "/nao/existe.conf"}, "--port");
}

TEST(ConfigArquivoTest, UsoCitaConfigELogEmArquivo) {
    const std::string uso = citius::config_usage("citius_server");

    for (const char* opcao : {"--config", "--log-file", "--log-max-bytes", "--log-keep"}) {
        EXPECT_NE(uso.find(opcao), std::string::npos) << opcao;
    }
}
