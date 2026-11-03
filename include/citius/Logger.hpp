#pragma once
#include <atomic>
#include <iosfwd>
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

/// Escreve linhas "2026-11-03 14:20:01 [INFO] mensagem". DEBUG e INFO vao para o fluxo de saida;
/// WARN e ERROR, para o de erro. Mensagens abaixo do nivel minimo sao descartadas.
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

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    /// @return o Logger do processo, ligado a std::cout/std::cerr. Os testes criam o proprio Logger.
    [[nodiscard]] static Logger& instance();

    /// Muda o nivel minimo; pode ser chamado de qualquer thread, vale a partir da proxima mensagem.
    void set_level(LogLevel level) noexcept { m_level.store(level); }

    [[nodiscard]] LogLevel level() const noexcept { return m_level.load(); }

    /// @return true se uma mensagem de @p level seria escrita. Use para pular um trabalho caro de
    ///         montar a mensagem (ex.: std::format) quando ela seria descartada.
    [[nodiscard]] bool enabled(LogLevel level) const noexcept { return level >= m_level.load(); }

    /// Escreve @p message no nivel @p level, se habilitado. Caracteres de controle (exceto TAB)
    /// viram '?': um valor vindo do cliente com "\n" nao pode forjar uma linha de log falsa.
    void log(LogLevel level, std::string_view message);

private:
    std::ostream& m_out;
    std::ostream& m_err;
    std::atomic<LogLevel> m_level;
    std::mutex m_mutex;
    static_assert(std::atomic<LogLevel>::is_always_lock_free);
};

/// Atalhos para Logger::instance().log(...).
void log_debug(std::string_view message);
void log_info(std::string_view message);
void log_warn(std::string_view message);
void log_error(std::string_view message);

} // namespace citius
