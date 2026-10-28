#pragma once
#include <string>
enum class HttpMethod {
    GET,
    POST,
    PUT,
    DELETE,
    UNKNOWN
};

struct HttpRequest {
    HttpMethod method;
    std::string path;
    std::string version;
};