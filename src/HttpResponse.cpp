#include "citius/HttpResponse.hpp"
#include "citius/HttpSyntax.hpp"
#include <algorithm>

namespace citius {

HttpResponse HttpResponse::make_error(StatusCode status) {
    HttpResponse response{status};
    response.set_body(std::to_string(static_cast<int>(status)) + " " + std::string(reason_phrase(status)) + "\n");
    // o cliente pode ter mandado lixo: nao vale tentar manter a conexao aberta depois de um erro
    (void)response.set_header("Connection", "close");  // valores constantes e validos: nao falha
    return response;
}

bool HttpResponse::set_header(std::string_view name, std::string_view value) {
    if (!is_token(name) || has_control_chars(value) || iequals(name, "Content-Length")) {
        return false;
    }

    auto existing = std::ranges::find_if(m_headers, [&](const auto& header) { return iequals(header.first, name); });
    if (existing != m_headers.end()) {
        existing->second = std::string(value);
    } else {
        m_headers.emplace_back(std::string(name), std::string(value));
    }
    return true;
}

void HttpResponse::set_body(std::string body, std::string_view content_type) {
    m_body = std::move(body);
    if (!content_type.empty()) {
        (void)set_header("Content-Type", content_type);  // so falha com content_type invalido: o cabecalho fica de fora
    }
}

std::string HttpResponse::serialize() const {
    std::string raw = "HTTP/1.1 ";
    raw += std::to_string(static_cast<int>(m_status));
    raw += ' ';
    raw += reason_phrase(m_status);
    raw += "\r\n";

    for (const auto& [name, value] : m_headers) {
        raw += name;
        raw += ": ";
        raw += value;
        raw += "\r\n";
    }
    raw += "Content-Length: ";
    raw += std::to_string(m_body.size());
    raw += "\r\n\r\n";
    raw += m_body;
    return raw;
}

} // namespace citius
