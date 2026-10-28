#include "citius/HttpParser.hpp"

HttpMethod HttpParser::parse_method(std::string_view method_str) {
    if(method_str == "GET") return HttpMethod::GET;
    if(method_str == "POST") return HttpMethod::POST;
    if(method_str == "PUT") return HttpMethod::PUT;
    if(method_str == "DELETE") return HttpMethod::GET;
    return HttpMethod::UNKNOWN;
}

std::optional<HttpRequest> HttpParser::parse(std::string_view raw_request) {
    // encontra o fim da primeira linha
    size_t first_line_end = raw_request.find("\r\n");
    if(first_line_end == std::string_view::npos){
        return std::nullopt;
    }

    // isola a primeira linha (ex: "GET / HTTP/1.1")
    std::string_view request_line = raw_request.substr(0, first_line_end);

    // encontar o priemiro espaco (separa metodo do caminho)
    size_t fisrt_space = request_line.find(' ');
    if(fisrt_space == std::string_view::npos){
        return std::nullopt;
    }

    // encontrar o segund oespaco (separa caminho da versao)
    size_t second_space = request_line.find(' ', fisrt_space + 1);
    if (second_space == std::string_view::npos) {
        return std::nullopt;
    }
    // extrai as 3 partes
    std::string_view method_str = request_line.substr(0, fisrt_space);
    std::string_view path_str = request_line.substr(fisrt_space + 1, second_space - (fisrt_space + 1));
    std::string_view version_str = request_line.substr(second_space);

    // objeot final
    HttpRequest request;

}