#pragma once
#include "citius/HttpRequest.hpp"
#include "citius/HttpResponse.hpp"
#include <cstddef>
#include <filesystem>

namespace citius {

/// Tamanho maximo de arquivo que serve_static() carrega na memoria (10 MB).
inline constexpr std::size_t kMaxStaticFileBytes = 10 * 1024 * 1024;

/// Responde a uma requisicao com um arquivo de @p root (ex.: "www").
///
/// Resultados:
/// - 200 com o arquivo e o Content-Type pela extensao;
/// - 400 se o alvo tiver codificacao %XX invalida ou byte nulo;
/// - 404 se o arquivo nao existir, for um diretorio ou o caminho escapar de @p root
///   (propositalmente o mesmo codigo, para nao revelar o que existe fora de www/);
/// - 403 se o arquivo existir mas nao puder ser lido;
/// - 405 (com "Allow: GET") para metodos diferentes de GET;
/// - 500 se o arquivo for maior que @p max_file_bytes (ele e lido inteiro para a memoria).
///
/// Nunca lanca por causa da requisicao: todo problema do cliente vira uma resposta de erro.
/// @param root pasta publica; precisa existir.
/// @return a resposta pronta (sem o cabecalho Connection, que e decisao do Server).
[[nodiscard]] HttpResponse serve_static(const std::filesystem::path& root, const HttpRequest& request,
                                        std::size_t max_file_bytes = kMaxStaticFileBytes);

} // namespace citius
