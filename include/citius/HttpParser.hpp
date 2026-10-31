#pragma once
#include "HttpRequest.hpp"
#include <string_view>
#include <optional>

namespace citius {

/// Interpreta texto HTTP bruto. Nao conhece sockets: recebe texto e devolve estruturas.
class HttpParser {
public:
    /// Interpreta a linha de requisicao ("GET /caminho HTTP/1.1") do inicio de @p raw_request.
    /// A entrada vem do cliente e e tratada como hostil: nunca lanca excecao.
    /// @return a requisicao, ou std::nullopt se a linha estiver incompleta (sem "\r\n"),
    ///         malformada ou com metodo desconhecido. Os campos sao copiados, entao
    ///         @p raw_request pode ser descartado depois.
    [[nodiscard]] static std::optional<HttpRequest> parse(std::string_view raw_request);

private:
    static HttpMethod parse_method(std::string_view method_str);
};

} // namespace citius
