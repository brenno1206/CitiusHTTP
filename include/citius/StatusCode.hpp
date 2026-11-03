#pragma once
#include <string_view>

namespace citius {

/// Codigos de status HTTP que o servidor usa (RFC 9110, secao 15).
enum class StatusCode {
    OK = 200,
    BAD_REQUEST = 400,
    FORBIDDEN = 403,
    NOT_FOUND = 404,
    METHOD_NOT_ALLOWED = 405,
    PAYLOAD_TOO_LARGE = 413,
    URI_TOO_LONG = 414,
    HEADERS_TOO_LARGE = 431,
    INTERNAL_SERVER_ERROR = 500,
    NOT_IMPLEMENTED = 501,
    VERSION_NOT_SUPPORTED = 505
};

/// @return a frase padrao do status ("Not Found"), ou "Unknown" para valor fora do enum.
[[nodiscard]] constexpr std::string_view reason_phrase(StatusCode status) noexcept {
    switch (status) {
        case StatusCode::OK: return "OK";
        case StatusCode::BAD_REQUEST: return "Bad Request";
        case StatusCode::FORBIDDEN: return "Forbidden";
        case StatusCode::NOT_FOUND: return "Not Found";
        case StatusCode::METHOD_NOT_ALLOWED: return "Method Not Allowed";
        case StatusCode::PAYLOAD_TOO_LARGE: return "Content Too Large";
        case StatusCode::URI_TOO_LONG: return "URI Too Long";
        case StatusCode::HEADERS_TOO_LARGE: return "Request Header Fields Too Large";
        case StatusCode::INTERNAL_SERVER_ERROR: return "Internal Server Error";
        case StatusCode::NOT_IMPLEMENTED: return "Not Implemented";
        case StatusCode::VERSION_NOT_SUPPORTED: return "HTTP Version Not Supported";
    }
    return "Unknown";
}

} // namespace citius
