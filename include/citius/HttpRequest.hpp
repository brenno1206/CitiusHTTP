#pragma once
#include <string>

namespace citius {

enum class HttpMethod {
    GET,
    POST,
    PUT,
    DELETE,
    UNKNOWN
};

struct HttpRequest {
    HttpMethod method = HttpMethod::UNKNOWN;
    std::string path;
    std::string version;
};

} // namespace citius
