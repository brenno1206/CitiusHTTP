#pragma once
#include <string>

namespace citius {

/// Metodos HTTP reconhecidos. UNKNOWN indica metodo nao suportado.
enum class HttpMethod {
    GET,
    POST,
    PUT,
    DELETE,
    UNKNOWN
};

/// Requisicao HTTP ja interpretada. Os campos sao copias (nao dependem do buffer original).
struct HttpRequest {
    HttpMethod method = HttpMethod::UNKNOWN;
    std::string path;    ///< alvo da requisicao, ex.: "/index.html"
    std::string version; ///< ex.: "HTTP/1.1"
};

} // namespace citius
