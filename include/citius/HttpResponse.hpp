#pragma once
#include "citius/StatusCode.hpp"
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace citius {

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

    /// @return a resposta completa: linha de status, cabecalhos, Content-Length, linha vazia e corpo.
    [[nodiscard]] std::string serialize() const;

private:
    StatusCode m_status;
    std::vector<std::pair<std::string, std::string>> m_headers;
    std::string m_body;
};

} // namespace citius
