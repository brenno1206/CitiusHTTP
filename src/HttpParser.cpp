#include "citius/HttpParser.hpp"
#include <cstddef>

namespace citius {

HttpMethod HttpParser::parse_method(std::string_view method_str) {
    if(method_str == "GET") return HttpMethod::GET;
    if(method_str == "POST") return HttpMethod::POST;
    if(method_str == "PUT") return HttpMethod::PUT;
    if(method_str == "DELETE") return HttpMethod::DELETE;
    return HttpMethod::UNKNOWN;
}

std::optional<HttpRequest> HttpParser::parse(std::string_view raw_request) {
    std::size_t first_line_end = raw_request.find("\r\n");
    if(first_line_end == std::string_view::npos){
        return std::nullopt;
    }

    std::string_view request_line = raw_request.substr(0, first_line_end);

    // formato: metodo SP caminho SP versao (RFC 9112, secao 3)
    std::size_t first_space = request_line.find(' ');
    if(first_space == std::string_view::npos){
        return std::nullopt;
    }

    std::size_t second_space = request_line.find(' ', first_space + 1);
    if (second_space == std::string_view::npos) {
        return std::nullopt;
    }
    std::string_view method_str = request_line.substr(0, first_space);
    std::string_view path_str = request_line.substr(first_space + 1, second_space - (first_space + 1));
    std::string_view version_str = request_line.substr(second_space + 1);

    // entrada do cliente e hostil: rejeita metodo desconhecido e partes vazias
    HttpMethod method = parse_method(method_str);
    if(method == HttpMethod::UNKNOWN || path_str.empty() || version_str.empty()){
        return std::nullopt;
    }

    HttpRequest request;
    request.method = method;
    request.path = std::string(path_str);
    request.version = std::string(version_str);
    return request;
}

} // namespace citius
