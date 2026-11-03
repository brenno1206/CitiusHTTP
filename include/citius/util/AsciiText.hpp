#pragma once
#include <algorithm>
#include <string>
#include <string_view>

namespace citius {

/// @return true se @p text tem algum caractere de controle (NUL, CR, LF...) alem do HTAB, ou DEL.
/// Usado para impedir injecao de linhas em cabecalhos e em linhas de log.
[[nodiscard]] inline bool has_control_chars(std::string_view text) {
    return std::ranges::any_of(text, [](char c) {
        auto const u = static_cast<unsigned char>(c);
        return (u < 0x20 && c != '\t') || u == 0x7f;
    });
}

/// @return @p c em minuscula se for letra ASCII (tolower da <cctype> depende do locale).
[[nodiscard]] constexpr char to_lower_ascii(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

/// @return copia de @p text com as letras ASCII em minusculas.
[[nodiscard]] inline std::string to_lower_ascii(std::string_view text) {
    std::string lower(text);
    std::ranges::transform(lower, lower.begin(), [](char c) { return to_lower_ascii(c); });
    return lower;
}

/// @return true se @p a e @p b sao iguais ignorando maiusculas/minusculas ASCII (nomes de cabecalho, de nivel de log).
[[nodiscard]] inline bool iequals(std::string_view a, std::string_view b) {
    return std::ranges::equal(a, b, [](char x, char y) { return to_lower_ascii(x) == to_lower_ascii(y); });
}

} // namespace citius
