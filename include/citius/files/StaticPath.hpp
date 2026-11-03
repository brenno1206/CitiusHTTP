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

/// @return o caminho canonico de @p root (symlinks resolvidos, sem barra final), ou std::nullopt se nao der para
/// resolve-lo.
[[nodiscard]] std::optional<std::filesystem::path> canonical_static_root(const std::filesystem::path& root);

/// Monta, so por texto (sem tocar no disco), o caminho de arquivo que um alvo ja decodificado designa dentro de @p
/// root.
/// "/" e qualquer caminho terminado em '/' apontam para o index.html daquele diretorio.
///
/// Recusa (std::nullopt) caminhos sem '/' inicial, com byte nulo ou com qualquer segmento "..". Barras repetidas e
/// segmentos "." sao ignorados, entao "//etc/passwd" vira "<root>/etc/passwd" (nunca o /etc/passwd real). Nao exige que
/// o arquivo exista: quem o abre decide o 404.
///
/// NAO resolve symlinks nem garante que o resultado fique dentro da raiz (um link dentro de www/ pode apontar para
/// fora): isso e feito por open_static_file(), conferindo o caminho REAL do arquivo ja aberto. Ficar so no texto e o
/// que torna esta funcao barata (nenhuma chamada de sistema) e imune a TOCTOU (nao ha nada a trocar entre "conferir" e
/// "usar").
/// @return @p root / caminho relativo (copia).
[[nodiscard]] std::optional<std::filesystem::path> lexical_static_path(const std::filesystem::path& root,
                                                                       std::string_view decoded_path);

} // namespace citius
