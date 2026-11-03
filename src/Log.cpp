#include "citius/Log.hpp"
#include <iostream>
#include <mutex>

namespace citius {

namespace {

// Um unico mutex para stdout e stderr: quando os dois vao ao mesmo terminal, as linhas tambem nao se misturam
std::mutex g_log_mutex;

} // namespace

void log_info(std::string_view line) {
    std::lock_guard lock{g_log_mutex};
    std::cout << line << '\n';
}

void log_error(std::string_view line) {
    std::lock_guard lock{g_log_mutex};
    std::cerr << line << '\n';
}

} // namespace citius
