#pragma once
#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <string_view>

namespace citius {

/// Metodos HTTP reconhecidos. UNKNOWN indica metodo nao suportado.
enum class HttpMethod {
    GET,
    POST,
    PUT,
    DELETE,
    UNKNOWN
};

/// @return o nome do metodo como aparece na requisicao ("GET"), ou "?" para UNKNOWN.
[[nodiscard]] constexpr std::string_view method_name(HttpMethod method) noexcept {
    switch (method) {
        case HttpMethod::GET: return "GET";
        case HttpMethod::POST: return "POST";
        case HttpMethod::PUT: return "PUT";
        case HttpMethod::DELETE: return "DELETE";
        case HttpMethod::UNKNOWN: break;
    }
    return "?";
}

/// Requisicao HTTP ja interpretada. Os campos sao copias (nao dependem do buffer original).
struct HttpRequest {
    /// Cabecalhos: nome sempre em minusculas ("content-type"); valor sem espacos nas pontas.
    /// Cabecalhos repetidos sao juntados com ", ". O comparador transparente permite
    /// buscar com std::string_view: headers.find("host").
    using Headers = std::map<std::string, std::string, std::less<>>;

    HttpMethod method = HttpMethod::UNKNOWN;
    std::string path;    ///< alvo da requisicao como veio, incluindo query string, ex.: "/busca?q=1"
    std::string version; ///< "HTTP/1.0" ou "HTTP/1.1"
    Headers headers;
    std::size_t content_length = 0; ///< tamanho do corpo declarado em Content-Length (0 se ausente)
    std::string body;               ///< corpo; o HttpParser nao o preenche, quem le o socket o faz
};

} // namespace citius
