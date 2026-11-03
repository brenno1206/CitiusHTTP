#include "citius/HttpParser.hpp"
#include "citius/HttpSyntax.hpp"
#include <algorithm>
#include <cctype>
#include <string>
#include <utility>

namespace citius {

namespace {

constexpr std::string_view kCrlf = "\r\n";

// SP e HTAB: o "espaco opcional" (OWS) que a RFC permite ao redor do valor
bool is_ows(char c) {
    return c == ' ' || c == '\t';
}

std::string_view trim_ows(std::string_view text) {
    while (!text.empty() && is_ows(text.front())) text.remove_prefix(1);
    while (!text.empty() && is_ows(text.back())) text.remove_suffix(1);
    return text;
}

// Os parse_* devolvem o erro, ou std::nullopt quando deu certo.

std::optional<ParseError> parse_method(std::string_view text, HttpMethod& method) {
    if (!is_token(text)) return ParseError::BAD_REQUEST;
    if (text == "GET") method = HttpMethod::GET;
    else if (text == "POST") method = HttpMethod::POST;
    else if (text == "PUT") method = HttpMethod::PUT;
    else if (text == "DELETE") method = HttpMethod::DELETE;
    else return ParseError::NOT_IMPLEMENTED;
    return std::nullopt;
}

// HTTP-version = "HTTP/" DIGIT "." DIGIT; 400 se estiver fora do formato, 505 se nao for 1.0/1.1
std::optional<ParseError> validate_version(std::string_view version) {
    constexpr std::string_view kPrefix = "HTTP/";
    if (!version.starts_with(kPrefix)) return ParseError::BAD_REQUEST;

    std::string_view numbers = version.substr(kPrefix.size());
    bool well_formed = numbers.size() == 3 && std::isdigit(static_cast<unsigned char>(numbers[0])) &&
                       numbers[1] == '.' && std::isdigit(static_cast<unsigned char>(numbers[2]));
    if (!well_formed) return ParseError::BAD_REQUEST;

    if (version != "HTTP/1.1" && version != "HTTP/1.0") return ParseError::VERSION_NOT_SUPPORTED;
    return std::nullopt;
}

// Aceitamos so a forma de origem ("/caminho?query"); absoluta ("http://...") e "*" ficam de fora
std::optional<ParseError> validate_target(std::string_view target) {
    if (target.empty() || target.front() != '/') return ParseError::BAD_REQUEST;
    // checa o tamanho antes de varrer os bytes: o alvo pode ter megabytes
    if (target.size() > HttpParser::kMaxTargetLength) return ParseError::URI_TOO_LONG;
    if (std::ranges::any_of(target, [](char c) {
            auto u = static_cast<unsigned char>(c);
            return u <= 0x20 || u == 0x7f;
        })) {
        return ParseError::BAD_REQUEST;
    }
    return std::nullopt;
}

std::optional<ParseError> parse_request_line(std::string_view line, HttpRequest& request) {
    // metodo SP alvo SP versao (RFC 9112, secao 3); a versao ocupa o resto da linha,
    // entao um espaco sobrando faz a validacao da versao falhar
    std::size_t first_space = line.find(' ');
    if (first_space == std::string_view::npos) return ParseError::BAD_REQUEST;
    std::size_t second_space = line.find(' ', first_space + 1);
    if (second_space == std::string_view::npos) return ParseError::BAD_REQUEST;

    std::string_view method = line.substr(0, first_space);
    std::string_view target = line.substr(first_space + 1, second_space - first_space - 1);
    std::string_view version = line.substr(second_space + 1);

    if (auto error = parse_method(method, request.method)) return error;
    if (auto error = validate_version(version)) return error;
    if (auto error = validate_target(target)) return error;

    request.path = std::string(target);
    request.version = std::string(version);
    return std::nullopt;
}

std::optional<ParseError> parse_header_line(std::string_view line, HttpRequest::Headers& headers,
                                            std::size_t& host_count) {
    if (has_control_chars(line)) return ParseError::BAD_REQUEST;

    std::size_t colon = line.find(':');
    if (colon == std::string_view::npos) return ParseError::BAD_REQUEST;

    // is_token rejeita nome vazio, espaco antes dos ":" (usado em request smuggling) e linha que
    // comeca com espaco/HTAB ("obs-fold", continuacao da anterior, proibida em requisicoes)
    std::string_view name = line.substr(0, colon);
    if (!is_token(name)) return ParseError::BAD_REQUEST;

    std::string lower_name = to_lower_ascii(name);
    std::string_view value = trim_ows(line.substr(colon + 1));
    if (lower_name == "host") ++host_count;

    auto [it, inserted] = headers.try_emplace(std::move(lower_name), value);
    if (!inserted) {
        it->second += ", ";
        it->second += value;
    }
    return std::nullopt;
}

// Content-Length = 1*DIGIT (RFC 9110, secao 8.6). Valores repetidos chegam juntados com ", " e
// por isso caem na regra "so digitos".
std::optional<ParseError> parse_content_length(const HttpRequest::Headers& headers, std::size_t& length) {
    auto it = headers.find("content-length");
    if (it == headers.end()) return std::nullopt;

    const std::string& text = it->second;
    if (text.empty() || !std::ranges::all_of(text, [](char c) { return c >= '0' && c <= '9'; })) {
        return ParseError::BAD_REQUEST;
    }

    // acumula digito a digito parando assim que passa do limite: nunca estoura size_t
    std::size_t value = 0;
    for (char c : text) {
        value = value * 10 + static_cast<std::size_t>(c - '0');
        if (value > HttpParser::kMaxBodyLength) return ParseError::PAYLOAD_TOO_LARGE;
    }
    length = value;
    return std::nullopt;
}

} // namespace

StatusCode to_status(ParseError error) noexcept {
    switch (error) {
        case ParseError::BAD_REQUEST: return StatusCode::BAD_REQUEST;
        case ParseError::NOT_IMPLEMENTED: return StatusCode::NOT_IMPLEMENTED;
        case ParseError::URI_TOO_LONG: return StatusCode::URI_TOO_LONG;
        case ParseError::VERSION_NOT_SUPPORTED: return StatusCode::VERSION_NOT_SUPPORTED;
        case ParseError::HEADERS_TOO_LARGE: return StatusCode::HEADERS_TOO_LARGE;
        case ParseError::PAYLOAD_TOO_LARGE: return StatusCode::PAYLOAD_TOO_LARGE;
    }
    return StatusCode::BAD_REQUEST;  // valor fora do enum: tratar como requisicao invalida
}

ParseResult HttpParser::parse(std::string_view raw_head) {
    std::size_t line_end = raw_head.find(kCrlf);
    if (line_end == std::string_view::npos) return ParseResult::failure(ParseError::BAD_REQUEST);

    HttpRequest request;
    if (auto error = parse_request_line(raw_head.substr(0, line_end), request)) {
        return ParseResult::failure(*error);
    }

    std::size_t host_count = 0;
    std::size_t header_lines = 0;
    std::size_t pos = line_end + kCrlf.size();
    while (true) {
        std::size_t end = raw_head.find(kCrlf, pos);
        // sem linha vazia final a cabeca esta incompleta (o read_request_head garante que existe)
        if (end == std::string_view::npos) return ParseResult::failure(ParseError::BAD_REQUEST);

        std::string_view line = raw_head.substr(pos, end - pos);
        pos = end + kCrlf.size();
        if (line.empty()) break;

        if (++header_lines > kMaxHeaderCount) return ParseResult::failure(ParseError::HEADERS_TOO_LARGE);
        if (auto error = parse_header_line(line, request.headers, host_count)) {
            return ParseResult::failure(*error);
        }
    }

    // RFC 9112, secao 3.2: em HTTP/1.1 o servidor deve responder 400 sem Host, ou com Host repetido
    if (request.version == "HTTP/1.1" && host_count != 1) {
        return ParseResult::failure(ParseError::BAD_REQUEST);
    }
    if (auto error = parse_content_length(request.headers, request.content_length)) {
        return ParseResult::failure(*error);
    }
    return ParseResult::success(std::move(request));
}

} // namespace citius
