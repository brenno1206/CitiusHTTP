#include "citius/Config.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <format>
#include <optional>

namespace citius {

namespace {

// Opcoes que recebem valor. "--help" e tratada a parte (nao tem valor).
constexpr std::array<std::string_view, 6> kOptions{
    "--port", "--root", "--max-connections", "--read-timeout-ms", "--idle-timeout-ms", "--log-level"};

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

// Aplica "name = value" em @p config. @return a mensagem de erro, ou std::nullopt se deu certo.
std::optional<std::string> apply(Config& config, std::string_view name, std::string_view value) {
    std::uint64_t number = 0;
    std::optional<std::string> error;

    if (name == "--port") {
        if ((error = read_number(name, value, 0, 65535, number))) return error;
        config.port = static_cast<std::uint16_t>(number);
    } else if (name == "--root") {
        if (value.empty()) return std::format("valor invalido para {}: o caminho esta vazio", name);
        config.root = value;
    } else if (name == "--max-connections") {
        if ((error = read_number(name, value, 1, 1'000'000, number))) return error;
        config.max_connections = static_cast<std::size_t>(number);
    } else if (name == "--read-timeout-ms") {
        if ((error = read_number(name, value, 1, 3'600'000, number))) return error;
        config.read_timeout = std::chrono::milliseconds(number);
    } else if (name == "--idle-timeout-ms") {
        if ((error = read_number(name, value, 1, 3'600'000, number))) return error;
        config.idle_timeout = std::chrono::milliseconds(number);
    } else if (name == "--log-level") {
        auto level = parse_log_level(value);
        if (!level) return std::format("valor invalido para {}: '{}' (use debug, info, warn ou error)", name, value);
        config.log_level = *level;
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

ConfigResult parse_config(std::span<const std::string_view> args) {
    ConfigResult result;

    for (std::size_t i = 0; i < args.size(); ++i) {
        std::string_view arg = args[i];
        if (arg == "--help" || arg == "-h") {
            result.status = ParseStatus::HELP;
            return result;
        }
        if (!arg.starts_with("--")) return invalid(std::format("argumento inesperado: '{}'", arg));

        std::string_view name = arg;
        std::string_view value;
        bool has_value = false;
        if (std::size_t const eq = arg.find('='); eq != std::string_view::npos) {
            name = arg.substr(0, eq);
            value = arg.substr(eq + 1);
            has_value = true;
        }

        if (std::ranges::find(kOptions, name) == kOptions.end()) {
            return invalid(std::format("opcao desconhecida: '{}'", name));
        }
        if (!has_value) {
            // outra opcao no lugar do valor ("--port --root x") quase sempre e esquecimento
            if (i + 1 >= args.size() || args[i + 1].starts_with("--")) {
                return invalid(std::format("falta o valor de {}", name));
            }
            value = args[++i];
        }
        if (auto error = apply(result.config, name, value)) return invalid(std::move(*error));
    }
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
        "  --read-timeout-ms N   prazo para a requisicao chegar inteira; estourou, 408 (padrao {})\n"
        "  --idle-timeout-ms N   espera maxima da proxima requisicao numa conexao keep-alive (padrao {})\n"
        "  --log-level NIVEL     debug, info, warn ou error (padrao {})\n"
        "  -h, --help            mostra esta ajuda\n",
        program, defaults.port, defaults.root.string(), defaults.max_connections, defaults.read_timeout.count(),
        defaults.idle_timeout.count(), "info");
}

} // namespace citius
