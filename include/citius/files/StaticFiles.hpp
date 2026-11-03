#pragma once
#include "citius/http/HttpRequest.hpp"
#include "citius/http/HttpResponse.hpp"
#include <cstddef>
#include <filesystem>

namespace citius {

/// Arquivos de ate este tamanho (64 KB) sao lidos de uma vez para a memoria e o descritor fecha na hora: manter um fd
/// aberto e fazer um laco de leitura para 13 bytes custaria mais que copia-los. Acima disso o arquivo vai em streaming.
inline constexpr std::size_t kInlineStaticFileBytes = 64 * 1024;

/// Responde a uma requisicao com um arquivo de @p root (ex.: "www").
///
/// Arquivos pequenos (ate kInlineStaticFileBytes) vao no corpo em memoria. Os maiores NAO sao lidos para a memoria: a
/// resposta leva o descritor aberto (HttpResponse::set_file_body) e o servidor o envia em pedacos. Por isso nao ha
/// limite de tamanho, e a memoria usada nao depende do tamanho do arquivo. O arquivo
/// e aberto com checagem no proprio descritor (ver open_static_file): nao ha janela para trocar um link depois de o
/// caminho ser conferido.
///
/// Resultados:
/// - 200 com o arquivo e o Content-Type pela extensao;
/// - 400 se o alvo tiver codificacao %XX invalida ou byte nulo;
/// - 404 se o arquivo nao existir, nao for um arquivo comum (diretorio, FIFO...) ou o caminho escapar de @p root
///   (propositalmente o mesmo codigo, para nao revelar o que existe fora de www/);
/// - 403 se o arquivo existir mas nao puder ser lido;
/// - 405 (com "Allow: GET") para metodos diferentes de GET;
/// - 503 (com "Retry-After") se faltarem descritores ou memoria agora;
/// - 500 em outra falha do sistema.
///
/// Nunca lanca por causa da requisicao: todo problema do cliente vira uma resposta de erro.
/// @param root pasta publica; precisa existir.
/// @return a resposta pronta (sem o cabecalho Connection, que e decisao do Server).
[[nodiscard]] HttpResponse serve_static(const std::filesystem::path& root, const HttpRequest& request);

} // namespace citius
