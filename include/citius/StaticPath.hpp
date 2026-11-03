#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace citius {

/// Extrai o caminho do alvo da requisicao ("/a%20b.css?v=1" -> "/a b.css"): descarta a
/// query string e decodifica sequencias %XX (uma unica vez, entao "%252e" vira "%2e").
/// A query nao e validada.
/// @param target alvo como veio na linha de requisicao (HttpRequest::path).
/// @return o caminho decodificado (copia), ou std::nullopt se nao comecar com '/',
///         tiver %XX malformado ou resultar em byte nulo. Nao resolve "..": isso e do resolve_static_path().
[[nodiscard]] std::optional<std::string> decode_target_path(std::string_view target);

/// Converte um caminho ja decodificado em um caminho de arquivo garantidamente dentro de @p root.
/// "/" e qualquer caminho terminado em '/' apontam para o index.html daquele diretorio.
///
/// Recusa (std::nullopt) caminhos sem '/' inicial, com byte nulo, com qualquer segmento ".."
/// e os que, depois de resolver symlinks, escapem da raiz. Barras repetidas e segmentos "."
/// sao ignorados, entao "//etc/passwd" vira "<root>/etc/passwd" (nunca o /etc/passwd real).
/// Nao exige que o arquivo exista: quem le o arquivo decide o 404.
///
/// Ha uma janela entre esta checagem e a abertura do arquivo (alguem pode trocar um symlink
/// nesse intervalo, TOCTOU); aceitavel aqui porque quem escreve em www/ ja e confiavel.
/// @return o caminho canonico (copia).
[[nodiscard]] std::optional<std::filesystem::path> resolve_static_path(const std::filesystem::path& root,
                                                                       std::string_view decoded_path);

} // namespace citius
