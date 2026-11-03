#include "citius/files/StaticFiles.hpp"
#include "citius/files/Mime.hpp"
#include "citius/files/StaticPath.hpp"
#include "citius/http/HttpRequest.hpp"
#include "citius/http/HttpResponse.hpp"
#include "citius/http/StatusCode.hpp"
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

namespace citius {

HttpResponse serve_static(const std::filesystem::path& root, const HttpRequest& request, std::size_t max_file_bytes) {
    namespace fs = std::filesystem;

    if (request.method != HttpMethod::GET) {
        HttpResponse response = HttpResponse::make_error(StatusCode::METHOD_NOT_ALLOWED);
        (void)response.set_header("Allow", "GET"); // a RFC 9110 exige o Allow em 405
        return response;
    }

    std::optional<std::string> decoded = decode_target_path(request.path);
    if (!decoded) {
        return HttpResponse::make_error(StatusCode::BAD_REQUEST);
    }
    std::optional<fs::path> path = resolve_static_path(root, *decoded);
    if (!path) {
        return HttpResponse::make_error(StatusCode::NOT_FOUND);
    }

    // is_regular_file segue symlinks, mas o caminho ja e canonico (resolve_static_path)
    std::error_code ec;
    if (!fs::is_regular_file(*path, ec)) {
        return HttpResponse::make_error(StatusCode::NOT_FOUND);
    }
    std::uintmax_t size = fs::file_size(*path, ec);
    if (ec) {
        return HttpResponse::make_error(StatusCode::NOT_FOUND);
    }
    // checa antes de ler: sem isso um arquivo gigante em www/ esgotaria a memoria
    if (size > max_file_bytes) {
        return HttpResponse::make_error(StatusCode::INTERNAL_SERVER_ERROR);
    }

    // binary: sem traducao de fim de linha; o arquivo vai byte a byte
    std::ifstream file{*path, std::ios::binary};
    if (!file) {
        // o arquivo existe (checamos acima), entao a causa provavel e permissao
        return HttpResponse::make_error(StatusCode::FORBIDDEN);
    }
    std::string body{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};

    HttpResponse response{StatusCode::OK};
    response.set_body(std::move(body), mime_type(path->extension().string()));
    return response;
}

} // namespace citius
