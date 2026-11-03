#pragma once
#include "citius/util/AsciiText.hpp" // iequals, to_lower_ascii, has_control_chars: usados junto com as regras abaixo
#include <algorithm>
#include <cctype>
#include <string_view>

namespace citius {

/// @return true se @p c e um "tchar" da RFC 9110 (secao 5.6.2): letra, digito ou um de !#$%&'*+-.^_`|~
[[nodiscard]] inline bool is_tchar(char c) {
    return (std::isalnum(static_cast<unsigned char>(c)) != 0) ||
           std::string_view("!#$%&'*+-.^_`|~").find(c) != std::string_view::npos;
}

/// @return true se @p text e um token valido (nao vazio, so tchars): serve de nome de cabecalho e de metodo.
[[nodiscard]] inline bool is_token(std::string_view text) {
    return !text.empty() && std::ranges::all_of(text, is_tchar);
}

} // namespace citius
