#pragma once
#include "citius/server/Server.hpp"
#include "citius/util/Logger.hpp"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace citius {

/// Tudo que o operador pode ajustar na linha de comando. Os valores iniciais sao os padroes.
struct Config {
    std::uint16_t port = 8080;                            ///< 0 = o SO escolhe uma porta livre
    std::filesystem::path root = "www";                   ///< pasta dos arquivos publicos
    std::size_t max_requests = kMaxRequestsPerConnection; ///< requisicoes por conexao keep-alive antes de fecha-la
    std::size_t max_connections = kDefaultMaxConnections; ///< conexoes abertas ao mesmo tempo; acima, 503
    std::chrono::milliseconds read_timeout = kDefaultReadTimeout; ///< requisicao inteira; estourou, 408
    std::chrono::milliseconds idle_timeout = kDefaultIdleTimeout; ///< keep-alive parado; estourou, fecha
    LogLevel log_level = LogLevel::INFO;
};

/// O que fazer com a linha de comando interpretada.
enum class ParseStatus {
    OK,     ///< ConfigResult::config esta valida: subir o servidor
    HELP,   ///< o operador pediu --help: mostrar config_usage() e sair com sucesso
    INVALID ///< argumento invalido: mostrar ConfigResult::error e sair com falha
};

/// Resultado de parse_config. Argumento invalido e rotina (digitacao do operador), nao excecao.
struct ConfigResult {
    ParseStatus status = ParseStatus::OK;
    Config config;     ///< so use quando status == OK
    std::string error; ///< so com INVALID: mensagem para o operador, citando a opcao
};

/// Interpreta os argumentos do programa (sem o argv[0]). Formas aceitas: "--port 8080" e
/// "--port=8080"; opcao repetida vale a ultima; "--help"/"-h" pede o texto de uso (a menos que um
/// argumento anterior ja seja invalido). Numeros so com digitos e dentro de faixas razoaveis
/// (porta 0-65535, conexoes 1-1000000, timeouts 1 ms-1 h). Nunca lanca excecao.
[[nodiscard]] ConfigResult parse_config(std::span<const std::string_view> args);

/// @return o texto de ajuda: uso, opcoes e padroes. @p program e o nome do executavel.
[[nodiscard]] std::string config_usage(std::string_view program);

} // namespace citius
