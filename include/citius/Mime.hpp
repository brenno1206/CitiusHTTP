#pragma once
#include <string_view>

namespace citius {

/// Descobre o tipo MIME (valor do cabecalho Content-Type) a partir da extensao do arquivo.
/// @param extension extensao com o ponto, como devolve std::filesystem::path::extension()
///        (".html"); maiusculas e minusculas sao equivalentes.
/// @return o tipo MIME, ou "application/octet-stream" se a extensao for vazia ou desconhecida.
///         O retorno aponta para texto estatico: pode ser guardado a vontade.
[[nodiscard]] std::string_view mime_type(std::string_view extension) noexcept;

} // namespace citius
