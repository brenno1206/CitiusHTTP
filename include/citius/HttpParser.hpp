#pragma once
#include "HttpRequest.hpp"
#include <string_view>
#include <optional>

namespace citius {

class HttpParser {
public:
    static std::optional<HttpRequest> parse(std::string_view raw_request);
private:
    static HttpMethod parse_method(std::string_view method_str);
};

} // namespace citius
