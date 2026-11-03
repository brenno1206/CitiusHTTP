#include "citius/files/Mime.hpp"
#include <algorithm>
#include <array>
#include <cctype>

namespace citius {

namespace {

struct MimeEntry {
    std::string_view extension;
    std::string_view type;
};

// Texto leva charset explicito: sem ele o navegador adivinha a codificacao
constexpr std::array kMimeTable = std::to_array<MimeEntry>({
    {".html", "text/html; charset=utf-8"},
    {".css", "text/css; charset=utf-8"},
    {".js", "text/javascript; charset=utf-8"},
    {".json", "application/json"},
    {".png", "image/png"},
    {".jpg", "image/jpeg"},
    {".jpeg", "image/jpeg"},
    {".svg", "image/svg+xml"},
    {".ico", "image/x-icon"},
    {".txt", "text/plain; charset=utf-8"},
});

} // namespace

std::string_view mime_type(std::string_view extension) noexcept {
    // compara sem alocar (a funcao e noexcept); a tabela ja esta em minusculas
    auto const equals_ignore_case = [](std::string_view a, std::string_view b) {
        return std::ranges::equal(a, b, [](char x, char y) {
            return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
        });
    };

    for (const MimeEntry& entry : kMimeTable) {
        if (equals_ignore_case(extension, entry.extension)) {
            return entry.type;
        }
    }
    return "application/octet-stream";
}

} // namespace citius
