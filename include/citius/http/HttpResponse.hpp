#pragma once
#include "citius/http/StatusCode.hpp"
#include "citius/util/FileDescriptor.hpp"
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace citius {

/// Corpo servido direto de um arquivo aberto, sem passar pela memoria (ver HttpResponse::set_file_body).
struct FileBody {
    /// Compartilhado, para a HttpResponse continuar copiavel. Quem envia o arquivo le com pread (posicao explicita),
    /// entao compartilhar o descritor e seguro.
    std::shared_ptr<const FileDescriptor> file;
    std::uint64_t size = 0; ///< bytes a enviar (o tamanho do arquivo quando foi aberto)
};

/// Resposta HTTP/1.1 em construcao. serialize() gera os bytes a enviar.
///
/// O Content-Length nunca e definido pelo chamador: serialize() sempre o calcula a partir
/// do corpo, entao ele nao pode ficar errado.
class HttpResponse {
public:
    explicit HttpResponse(StatusCode status = StatusCode::OK) : m_status(status) {}

    /// Resposta de erro pronta: corpo em texto ("404 Not Found\n") e "Connection: close".
    [[nodiscard]] static HttpResponse make_error(StatusCode status);

    [[nodiscard]] StatusCode status() const noexcept {
        return m_status;
    }
    void set_status(StatusCode status) noexcept {
        m_status = status;
    }

    /// Define um cabecalho, substituindo o anterior de mesmo nome (sem diferenciar maiusculas).
    /// Os cabecalhos saem na ordem em que foram inseridos pela primeira vez.
    ///
    /// Recusa (sem alterar nada) nomes que nao sejam token, valores com caracteres de controle
    /// (impede injecao de linhas / "response splitting" quando o valor vem do cliente) e o
    /// proprio Content-Length, que e calculado.
    /// @return false se o cabecalho foi recusado.
    [[nodiscard]] bool set_header(std::string_view name, std::string_view value);

    /// Define o corpo (bytes quaisquer, inclusive NUL) e, se @p content_type nao for vazio,
    /// o cabecalho Content-Type.
    void set_body(std::string body, std::string_view content_type = "text/plain; charset=utf-8");

    /// Define como corpo o conteudo de um arquivo ja aberto, que o servidor envia em pedacos (memoria constante, sem
    /// limite de tamanho). Substitui um corpo definido por set_body(). @p size e o que sera enviado e vira o
    /// Content-Length: se o arquivo encolher durante o envio a conexao e encerrada (nao da para cumprir o que se
    /// prometeu).
    /// @param file o arquivo aberto para leitura; a resposta passa a ser dona dele.
    void set_file_body(FileDescriptor file, std::uint64_t size, std::string_view content_type);

    /// @return o corpo de arquivo, se houver (set_file_body); std::nullopt para corpo em memoria.
    [[nodiscard]] const std::optional<FileBody>& file_body() const noexcept {
        return m_file;
    }

    /// @return a resposta completa: linha de status, cabecalhos, Content-Length, linha vazia e corpo. Com corpo de
    ///         arquivo, so a CABECA (o Content-Length e o tamanho do arquivo): os bytes do arquivo seguem depois, lidos
    ///         de file_body() por quem envia a resposta.
    [[nodiscard]] std::string serialize() const;

private:
    StatusCode m_status;
    std::vector<std::pair<std::string, std::string>> m_headers;
    std::string m_body;
    std::optional<FileBody> m_file;
};

} // namespace citius
