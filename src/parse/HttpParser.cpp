#include "citius/parse/HttpParser.hpp"
#include "citius/http/HttpRequest.hpp"
#include "citius/http/HttpSyntax.hpp"
#include "citius/http/StatusCode.hpp"
#include "citius/util/AsciiText.hpp"
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
    if (text == "GET")
        method = HttpMethod::GET;
    else if (text == "POST")
        method = HttpMethod::POST;
    else if (text == "PUT")
        method = HttpMethod::PUT;
    else if (text == "DELETE")
        method = HttpMethod::DELETE;
    else
        return ParseError::NOT_IMPLEMENTED;
    return std::nullopt;
}

// HTTP-version = "HTTP/" DIGIT "." DIGIT; 400 se estiver fora do formato, 505 se nao for 1.0/1.1
std::optional<ParseError> validate_version(std::string_view version) {
    constexpr std::string_view kPrefix = "HTTP/";
    if (!version.starts_with(kPrefix)) return ParseError::BAD_REQUEST;

    std::string_view const numbers = version.substr(kPrefix.size());
    bool const well_formed = numbers.size() == 3 && (std::isdigit(static_cast<unsigned char>(numbers[0])) != 0) &&
                             numbers[1] == '.' && (std::isdigit(static_cast<unsigned char>(numbers[2])) != 0);
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
            auto const u = static_cast<unsigned char>(c);
            return u <= 0x20 || u == 0x7f;
        })) {
        return ParseError::BAD_REQUEST;
    }
    return std::nullopt;
}

std::optional<ParseError> parse_request_line(std::string_view line, HttpRequest& request) {
    // metodo SP alvo SP versao (RFC 9112, secao 3); a versao ocupa o resto da linha,
    // entao um espaco sobrando faz a validacao da versao falhar
    std::size_t const first_space = line.find(' ');
    if (first_space == std::string_view::npos) return ParseError::BAD_REQUEST;
    std::size_t const second_space = line.find(' ', first_space + 1);
    if (second_space == std::string_view::npos) return ParseError::BAD_REQUEST;

    std::string_view const method = line.substr(0, first_space);
    std::string_view const target = line.substr(first_space + 1, second_space - first_space - 1);
    std::string_view const version = line.substr(second_space + 1);

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

    std::size_t const colon = line.find(':');
    if (colon == std::string_view::npos) return ParseError::BAD_REQUEST;

    // is_token rejeita nome vazio, espaco antes dos ":" (usado em request smuggling) e linha que
    // comeca com espaco/HTAB ("obs-fold", continuacao da anterior, proibida em requisicoes)
    std::string_view const name = line.substr(0, colon);
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

// Transfer-Encoding (RFC 9112, secoes 6.1 e 6.3). So "chunked" sozinho e suportado. Esta e uma fronteira de seguranca
// (request smuggling: dois servidores em fila discordando de onde a mensagem termina), entao qualquer ambiguidade e
// 400:
//   - junto com Content-Length (o caso classico CL.TE / TE.CL);
//   - em HTTP/1.0 (nao existe la);
//   - lista com item vazio, "chunked" repetido, ou "chunked" que nao e o ultimo.
// Outras codificacoes que nao conhecemos (gzip, identity...) sao 501.
std::optional<ParseError> parse_transfer_encoding(HttpRequest& request) {
    auto const it = request.headers.find("transfer-encoding");
    if (it == request.headers.end()) return std::nullopt;
    if (request.version == "HTTP/1.0" || request.headers.contains("content-length")) return ParseError::BAD_REQUEST;

    std::vector<std::string> codings;
    std::string_view rest = it->second;
    while (true) {
        const std::size_t comma = rest.find(',');
        codings.push_back(to_lower_ascii(trim_ows(rest.substr(0, comma))));
        if (comma == std::string_view::npos) break;
        rest.remove_prefix(comma + 1);
    }

    if (std::ranges::any_of(codings, [](const std::string& c) { return c.empty(); })) return ParseError::BAD_REQUEST;
    const auto chunked_count = static_cast<std::size_t>(std::ranges::count(codings, "chunked"));
    if (chunked_count > 1) return ParseError::BAD_REQUEST;
    if (chunked_count == 1 && codings.back() != "chunked") return ParseError::BAD_REQUEST;
    if (codings.size() == 1 && chunked_count == 1) {
        request.chunked = true;
        return std::nullopt;
    }
    return ParseError::NOT_IMPLEMENTED;
}

// Expect (RFC 9110, secao 10.1.1). So "100-continue" e conhecido; outro valor e 417. Em HTTP/1.0 o servidor o ignora.
std::optional<ParseError> parse_expect(HttpRequest& request) {
    auto const it = request.headers.find("expect");
    if (it == request.headers.end() || request.version == "HTTP/1.0") return std::nullopt;
    if (!iequals(trim_ows(it->second), "100-continue")) return ParseError::EXPECTATION_FAILED;
    request.expect_continue = true;
    return std::nullopt;
}

// Content-Length = 1*DIGIT (RFC 9110, secao 8.6). Valores repetidos chegam juntados com ", " e
// por isso caem na regra "so digitos".
std::optional<ParseError> parse_content_length(const HttpRequest::Headers& headers, std::size_t& length) {
    auto const it = headers.find("content-length");
    if (it == headers.end()) return std::nullopt;

    const std::string& text = it->second;
    if (text.empty() || !std::ranges::all_of(text, [](char c) { return c >= '0' && c <= '9'; })) {
        return ParseError::BAD_REQUEST;
    }

    // acumula digito a digito parando assim que passa do limite: nunca estoura size_t
    std::size_t value = 0;
    for (char const c : text) {
        value = (value * 10) + static_cast<std::size_t>(c - '0');
        if (value > HttpParser::kMaxBodyLength) return ParseError::PAYLOAD_TOO_LARGE;
    }
    length = value;
    return std::nullopt;
}

} // namespace

StatusCode to_status(ParseError error) noexcept {
    switch (error) {
    case ParseError::BAD_REQUEST:
        return StatusCode::BAD_REQUEST;
    case ParseError::NOT_IMPLEMENTED:
        return StatusCode::NOT_IMPLEMENTED;
    case ParseError::URI_TOO_LONG:
        return StatusCode::URI_TOO_LONG;
    case ParseError::EXPECTATION_FAILED:
        return StatusCode::EXPECTATION_FAILED;
    case ParseError::VERSION_NOT_SUPPORTED:
        return StatusCode::VERSION_NOT_SUPPORTED;
    case ParseError::HEADERS_TOO_LARGE:
        return StatusCode::HEADERS_TOO_LARGE;
    case ParseError::PAYLOAD_TOO_LARGE:
        return StatusCode::PAYLOAD_TOO_LARGE;
    }
    return StatusCode::BAD_REQUEST; // valor fora do enum: tratar como requisicao invalida
}

ParseResult HttpParser::parse(std::string_view raw_head) {
    std::size_t const line_end = raw_head.find(kCrlf);
    if (line_end == std::string_view::npos) return ParseResult::failure(ParseError::BAD_REQUEST);

    HttpRequest request;
    if (auto error = parse_request_line(raw_head.substr(0, line_end), request)) {
        return ParseResult::failure(*error);
    }

    std::size_t host_count = 0;
    std::size_t header_lines = 0;
    std::size_t pos = line_end + kCrlf.size();
    while (true) {
        std::size_t const end = raw_head.find(kCrlf, pos);
        // sem linha vazia final a cabeca esta incompleta (o RequestAssembler garante que existe)
        if (end == std::string_view::npos) return ParseResult::failure(ParseError::BAD_REQUEST);

        std::string_view const line = raw_head.substr(pos, end - pos);
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
    if (auto error = parse_transfer_encoding(request)) return ParseResult::failure(*error);
    if (!request.chunked) {
        if (auto error = parse_content_length(request.headers, request.content_length)) {
            return ParseResult::failure(*error);
        }
    }
    if (auto error = parse_expect(request)) return ParseResult::failure(*error);
    return ParseResult::success(std::move(request));
}

} // namespace citius
