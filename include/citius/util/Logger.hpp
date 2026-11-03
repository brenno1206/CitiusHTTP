#pragma once
#include "citius/util/RotatingFile.hpp"
#include <atomic>
#include <cstddef>
#include <filesystem>
#include <iosfwd>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace citius {

/// Gravidade de uma mensagem de log, da menos para a mais importante.
enum class LogLevel {
    DEBUG, ///< detalhe para quem depura (ex.: cada requisicao atendida)
    INFO,  ///< fatos normais da vida do servidor (inicio, parada)
    WARN,  ///< algo estranho, mas o servidor segue (ex.: fila cheia -> 503)
    ERROR  ///< falha que o operador precisa ver (ex.: handler lancou excecao)
};

/// @return o nome do nivel em maiusculas ("WARN"), usado na linha de log.
[[nodiscard]] std::string_view level_name(LogLevel level) noexcept;

/// @return o nivel de nome @p name ("debug", "INFO"...; ignora maiusculas), ou std::nullopt se invalido.
[[nodiscard]] std::optional<LogLevel> parse_log_level(std::string_view name);

/// Tamanho a partir do qual o arquivo de log rotaciona (padrao: 10 MB).
inline constexpr std::size_t kDefaultLogMaxBytes = 10 * 1024 * 1024;

/// Quantos arquivos de log antigos ficam guardados (padrao: 5).
inline constexpr std::size_t kDefaultLogKeep = 5;

/// Escreve linhas "2026-11-03 14:20:01 [INFO] mensagem". DEBUG e INFO vao para o fluxo de saida;
/// WARN e ERROR, para o de erro. Mensagens abaixo do nivel minimo sao descartadas. Com log_to_file(), TODAS as
/// mensagens vao para o arquivo (com rotacao) e o console fica quieto.
///
/// Seguro para varias threads: cada linha e montada fora do lock e escrita de uma vez, entao
/// linhas de threads diferentes nunca se misturam (e nao ha corrida no estado dos fluxos, que o
/// TSan acusou quando varias threads usavam std::cout direto).
class Logger {
public:
    /// @param out   destino de DEBUG/INFO; precisa viver mais que o Logger
    /// @param err   destino de WARN/ERROR; idem (pode ser o mesmo objeto que @p out)
    /// @param level nivel minimo: mensagens mais fracas sao ignoradas
    Logger(std::ostream& out, std::ostream& err, LogLevel level = LogLevel::INFO) noexcept;

    // guarda referencias aos fluxos e um mutex: nao e copiavel nem movivel
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;
    Logger(Logger&&) = delete;
    Logger& operator=(Logger&&) = delete;
    ~Logger() = default;

    /// Passa a escrever num arquivo com rotacao por tamanho (ver RotatingFile) em vez de no console. Se uma escrita no
    /// arquivo falhar (disco cheio, pasta removida...), a linha NAO se perde: vai para o fluxo de erro. Chame na
    /// inicializacao, antes de o servidor atender (pode ser chamado com o Logger em uso, mas nao desfaz).
    /// @param max_bytes tamanho a partir do qual rotaciona; @param keep quantos arquivos antigos guardar.
    /// @throws std::runtime_error se o arquivo nao puder ser aberto; o Logger segue escrevendo no console.
    void log_to_file(const std::filesystem::path& path, std::size_t max_bytes = kDefaultLogMaxBytes,
                     std::size_t keep = kDefaultLogKeep);

    /// @return o Logger do processo, ligado a std::cout/std::cerr. Os testes criam o proprio Logger.
    [[nodiscard]] static Logger& instance();

    /// Muda o nivel minimo; pode ser chamado de qualquer thread, vale a partir da proxima mensagem.
    void set_level(LogLevel level) noexcept {
        m_level.store(level);
    }

    [[nodiscard]] LogLevel level() const noexcept {
        return m_level.load();
    }

    /// @return true se uma mensagem de @p level seria escrita. Use para pular um trabalho caro de
    ///         montar a mensagem (ex.: std::format) quando ela seria descartada.
    [[nodiscard]] bool enabled(LogLevel level) const noexcept {
        return level >= m_level.load();
    }

    /// Escreve @p message no nivel @p level, se habilitado. Caracteres de controle (exceto TAB)
    /// viram '?': um valor vindo do cliente com "\n" nao pode forjar uma linha de log falsa.
    void log(LogLevel level, std::string_view message);

private:
    std::ostream& m_out;
    std::ostream& m_err;
    std::atomic<LogLevel> m_level;
    std::mutex m_mutex;
    std::unique_ptr<RotatingFile> m_file; ///< destino em arquivo, se log_to_file() foi chamado (protegido por m_mutex)
    static_assert(std::atomic<LogLevel>::is_always_lock_free);
};

/// Atalhos para Logger::instance().log(...).
void log_debug(std::string_view message);
void log_info(std::string_view message);
void log_warn(std::string_view message);
void log_error(std::string_view message);

} // namespace citius
