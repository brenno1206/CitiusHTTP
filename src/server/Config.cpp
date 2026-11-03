#include "citius/server/Config.hpp"
#include "citius/util/AsciiText.hpp"
#include "citius/util/Logger.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace citius {

namespace {

// Opcoes que recebem valor. "--help" e tratada a parte (nao tem valor).
constexpr std::array<std::string_view, 11> kOptions{
    "--port",      "--root",     "--max-connections", "--max-requests", "--read-timeout-ms", "--idle-timeout-ms",
    "--log-level", "--log-file", "--log-max-bytes",   "--log-keep",     "--config"};

// Tamanho maximo do arquivo de configuracao: uma configuracao de verdade tem poucas linhas; um arquivo enorme e engano
// (ou tentativa de fazer o servidor ler algo que nao e configuracao).
constexpr std::size_t kMaxConfigFileBytes = 64 * 1024;

// Aceita so digitos: from_chars nao tolera espaco nem '+', e recusa '-' em tipo sem sinal.
// Sem a checagem de 'ptr', "80x" passaria como 80.
std::optional<std::uint64_t> parse_uint(std::string_view text) {
    std::uint64_t value = 0;
    auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || ptr != text.data() + text.size()) return std::nullopt;
    return value;
}

// Le um inteiro de @p name em [min, max] para @p out; devolve a mensagem de erro ou std::nullopt.
std::optional<std::string> read_number(std::string_view name, std::string_view text, std::uint64_t min,
                                       std::uint64_t max, std::uint64_t& out) {
    auto value = parse_uint(text);
    if (!value || *value < min || *value > max) {
        return std::format("valor invalido para {}: '{}' (esperado um inteiro de {} a {})", name, text, min, max);
    }
    out = *value;
    return std::nullopt;
}

// Uma opcao numerica: o intervalo aceito e como gravar o valor. Acrescentar uma opcao numerica e acrescentar uma linha
// aqui (e o nome em kOptions), nao um ramo novo numa cadeia de if/else.
struct NumberOption {
    std::string_view name;
    std::uint64_t min;
    std::uint64_t max;
    void (*set)(Config&, std::uint64_t);
};

constexpr std::array<NumberOption, 7> kNumberOptions{{
    {"--port", 0, 65535, [](Config& c, std::uint64_t v) { c.port = static_cast<std::uint16_t>(v); }},
    {"--max-connections", 1, 1'000'000,
     [](Config& c, std::uint64_t v) { c.max_connections = static_cast<std::size_t>(v); }},
    {"--max-requests", 1, 1'000'000, [](Config& c, std::uint64_t v) { c.max_requests = static_cast<std::size_t>(v); }},
    {"--read-timeout-ms", 1, 3'600'000,
     [](Config& c, std::uint64_t v) { c.read_timeout = std::chrono::milliseconds(v); }},
    {"--idle-timeout-ms", 1, 3'600'000,
     [](Config& c, std::uint64_t v) { c.idle_timeout = std::chrono::milliseconds(v); }},
    {"--log-max-bytes", 1024, 1'073'741'824,
     [](Config& c, std::uint64_t v) { c.log_max_bytes = static_cast<std::size_t>(v); }}, // 1 KiB a 1 GiB
    {"--log-keep", 0, 1000, [](Config& c, std::uint64_t v) { c.log_keep = static_cast<std::size_t>(v); }},
}};

// Opcoes de texto (caminho e nivel de log).
std::optional<std::string> apply_text(Config& config, std::string_view name, std::string_view value) {
    if (name == "--log-level") {
        auto level = parse_log_level(value);
        if (!level) return std::format("valor invalido para {}: '{}' (use debug, info, warn ou error)", name, value);
        config.log_level = *level;
        return std::nullopt;
    }
    if (value.empty()) return std::format("valor invalido para {}: o caminho esta vazio", name);
    if (name == "--root") config.root = value;
    if (name == "--log-file") config.log_file = std::filesystem::path{value};
    return std::nullopt;
}

// Aplica "name = value" em @p config. @return a mensagem de erro, ou std::nullopt se deu certo.
std::optional<std::string> apply(Config& config, std::string_view name, std::string_view value) {
    const auto numeric = std::ranges::find(kNumberOptions, name, &NumberOption::name);
    if (numeric == kNumberOptions.end()) return apply_text(config, name, value);

    std::uint64_t number = 0;
    if (auto error = read_number(name, value, numeric->min, numeric->max, number)) return error;
    numeric->set(config, number);
    return std::nullopt;
}

std::string_view trim(std::string_view text) {
    constexpr std::string_view kBlanks = " \t\r";
    const std::size_t first = text.find_first_not_of(kBlanks);
    if (first == std::string_view::npos) return {};
    return text.substr(first, text.find_last_not_of(kBlanks) - first + 1);
}

// Le o arquivo de configuracao e aplica cada "chave = valor" em @p config. @return a mensagem de erro (com
// arquivo:linha) ou std::nullopt se deu certo.
std::optional<std::string> load_config_file(const std::string& path, Config& config) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        return std::format("nao foi possivel ler o arquivo de configuracao '{}'", path);
    }
    std::ifstream in{path, std::ios::binary};
    if (!in) return std::format("nao foi possivel ler o arquivo de configuracao '{}'", path);

    std::string text(kMaxConfigFileBytes + 1, '\0'); // um byte a mais para saber se passou do limite
    in.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<std::size_t>(in.gcount()));
    if (text.size() > kMaxConfigFileBytes) {
        return std::format("arquivo de configuracao grande demais (maximo {} bytes): '{}'", kMaxConfigFileBytes, path);
    }

    std::size_t line_number = 0;
    std::string_view rest{text};
    while (!rest.empty()) {
        const std::size_t newline = rest.find('\n');
        const std::string_view line = trim(rest.substr(0, newline));
        rest = newline == std::string_view::npos ? std::string_view{} : rest.substr(newline + 1);
        ++line_number;
        if (line.empty() || line.front() == '#') continue;

        const std::string where = std::format("{}:{}: ", path, line_number);
        if (has_control_chars(line)) return where + "a linha tem caractere de controle";
        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos) return where + "esperado 'chave = valor'";
        const std::string_view key = trim(line.substr(0, eq));
        const std::string_view value = trim(line.substr(eq + 1));
        if (key.empty()) return where + "falta a chave antes do '='";
        if (key == "config") return where + "'config' nao pode ser usado dentro do arquivo de configuracao";

        const std::string name = "--" + std::string(key);
        if (std::ranges::find(kOptions, name) == kOptions.end())
            return where + std::format("opcao desconhecida: '{}'", key);
        if (auto error = apply(config, name, value)) return where + *error;
    }
    return std::nullopt;
}

ConfigResult invalid(std::string message) {
    ConfigResult result;
    result.status = ParseStatus::INVALID;
    result.error = std::move(message);
    return result;
}

} // namespace

// Separa o argumento @p args[i] em nome e valor ("--port=80", ou "--port" seguido do proximo argumento, que entao e
// consumido: @p i avanca). @return a mensagem de erro, ou std::nullopt se deu certo.
std::optional<std::string> split_option(std::span<const std::string_view> args, std::size_t& i, std::string_view& name,
                                        std::string_view& value) {
    const std::string_view arg = args[i];
    if (!arg.starts_with("--")) return std::format("argumento inesperado: '{}'", arg);

    name = arg;
    bool has_value = false;
    if (const std::size_t eq = arg.find('='); eq != std::string_view::npos) {
        name = arg.substr(0, eq);
        value = arg.substr(eq + 1);
        has_value = true;
    }
    if (std::ranges::find(kOptions, name) == kOptions.end()) return std::format("opcao desconhecida: '{}'", name);
    if (!has_value) {
        // outra opcao no lugar do valor ("--port --root x") quase sempre e esquecimento
        if (i + 1 >= args.size() || args[i + 1].starts_with("--")) return std::format("falta o valor de {}", name);
        value = args[++i];
    }
    return std::nullopt;
}

ConfigResult parse_config(std::span<const std::string_view> args) {
    ConfigResult result;
    // A linha de comando e varrida primeiro: o arquivo so entra depois e a linha de comando e reaplicada por cima, o
    // que da a precedencia "padroes < arquivo < linha de comando" qualquer que seja a ordem dos argumentos.
    std::vector<std::pair<std::string, std::string>> from_cli;
    std::optional<std::string> config_path;
    Config scratch; // so valida cada valor no lugar em que ele aparece (os erros apontam o argumento certo)

    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--help" || args[i] == "-h") {
            result.status = ParseStatus::HELP;
            return result;
        }
        std::string_view name;
        std::string_view value;
        if (auto error = split_option(args, i, name, value)) return invalid(std::move(*error));

        if (name == "--config") {
            if (value.empty()) return invalid("valor invalido para --config: o caminho esta vazio");
            config_path = std::string(value);
        } else if (auto error = apply(scratch, name, value)) {
            return invalid(std::move(*error));
        } else {
            from_cli.emplace_back(std::string(name), std::string(value));
        }
    }

    if (config_path) {
        if (auto error = load_config_file(*config_path, result.config)) return invalid(std::move(*error));
    }
    for (const auto& [name, value] : from_cli) (void)apply(result.config, name, value); // ja validados acima
    return result;
}

std::string config_usage(std::string_view program) {
    const Config defaults;
    return std::format(
        "Uso: {} [opcoes]\n"
        "\n"
        "Opcoes (cada uma aceita \"--opcao valor\" ou \"--opcao=valor\"):\n"
        "  --port N              porta TCP, 0 a 65535; 0 = o SO escolhe (padrao {})\n"
        "  --root PASTA          pasta dos arquivos publicos (padrao {})\n"
        "  --max-connections N   conexoes abertas ao mesmo tempo, 1 a 1000000; acima disso, 503 (padrao {})\n"
        "  --max-requests N      requisicoes por conexao keep-alive antes de fecha-la, 1 a 1000000 (padrao {})\n"
        "  --read-timeout-ms N   prazo para a requisicao chegar inteira; estourou, 408 (padrao {})\n"
        "  --idle-timeout-ms N   espera maxima da proxima requisicao numa conexao keep-alive (padrao {})\n"
        "  --log-level NIVEL     debug, info, warn ou error (padrao {})\n"
        "  --log-file ARQUIVO    escreve o log neste arquivo, com rotacao, em vez de no console (padrao: console)\n"
        "  --log-max-bytes N     tamanho a partir do qual o arquivo de log rotaciona, 1024 a 1073741824 (padrao {})\n"
        "  --log-keep N          arquivos de log antigos guardados, 0 a 1000 (padrao {})\n"
        "  --config ARQUIVO      le as opcoes de um arquivo \"chave = valor\" (a linha de comando vence)\n"
        "  -h, --help            mostra esta ajuda\n",
        program, defaults.port, defaults.root.string(), defaults.max_connections, defaults.max_requests,
        defaults.read_timeout.count(), defaults.idle_timeout.count(), "info", defaults.log_max_bytes,
        defaults.log_keep);
}

} // namespace citius
