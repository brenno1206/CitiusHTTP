#include "citius/util/Logger.hpp"
#include "citius/util/AsciiText.hpp"
#include <array>
#include <chrono>
#include <ctime>
#include <iostream>

namespace citius {

namespace {

// "2026-11-03 14:20:01" na hora local. localtime_r (POSIX) e a versao thread-safe de localtime.
std::string timestamp() {
    std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm local{};
    localtime_r(&now, &local);
    std::array<char, 32> buffer{};
    std::size_t const size = std::strftime(buffer.data(), buffer.size(), "%Y-%m-%d %H:%M:%S", &local);
    return {buffer.data(), size};
}

} // namespace

std::string_view level_name(LogLevel level) noexcept {
    switch (level) {
    case LogLevel::DEBUG:
        return "DEBUG";
    case LogLevel::INFO:
        return "INFO";
    case LogLevel::WARN:
        return "WARN";
    case LogLevel::ERROR:
        return "ERROR";
    }
    return "?";
}

std::optional<LogLevel> parse_log_level(std::string_view name) {
    for (LogLevel level : {LogLevel::DEBUG, LogLevel::INFO, LogLevel::WARN, LogLevel::ERROR}) {
        if (iequals(name, level_name(level))) return level;
    }
    return std::nullopt;
}

Logger::Logger(std::ostream& out, std::ostream& err, LogLevel level) noexcept
    : m_out(out), m_err(err), m_level(level) {}

Logger& Logger::instance() {
    static Logger logger{std::cout, std::cerr};
    return logger;
}

void Logger::log(LogLevel level, std::string_view message) {
    if (!enabled(level)) return;

    std::string line = timestamp();
    line += " [";
    line += level_name(level);
    line += "] ";
    for (char const c : message) {
        auto const u = static_cast<unsigned char>(c);
        line += ((u < 0x20 && c != '\t') || u == 0x7f) ? '?' : c;
    }
    line += '\n';

    std::scoped_lock const lock{m_mutex};
    // flush a cada linha: com stdout redirecionado para arquivo ele e totalmente bufferizado, e
    // um log que so aparece quando o buffer enche (ou o servidor cai) nao serve para investigar
    std::ostream& stream = level >= LogLevel::WARN ? m_err : m_out;
    stream << line << std::flush;
}

void log_debug(std::string_view message) {
    Logger::instance().log(LogLevel::DEBUG, message);
}
void log_info(std::string_view message) {
    Logger::instance().log(LogLevel::INFO, message);
}
void log_warn(std::string_view message) {
    Logger::instance().log(LogLevel::WARN, message);
}
void log_error(std::string_view message) {
    Logger::instance().log(LogLevel::ERROR, message);
}

} // namespace citius
