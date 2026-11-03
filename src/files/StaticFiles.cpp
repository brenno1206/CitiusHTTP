#include "citius/files/StaticFiles.hpp"
#include "citius/files/Mime.hpp"
#include "citius/files/StaticOpen.hpp"
#include "citius/files/StaticPath.hpp"
#include "citius/http/HttpRequest.hpp"
#include "citius/http/HttpResponse.hpp"
#include "citius/http/StatusCode.hpp"
#include "citius/util/FileDescriptor.hpp"
#include <cerrno>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <sys/types.h> // IWYU pragma: keep (ssize_t)
#include <unistd.h>
#include <utility>

namespace citius {

namespace {

HttpResponse error_for(OpenStatus status) {
    switch (status) {
    case OpenStatus::FORBIDDEN:
        return HttpResponse::make_error(StatusCode::FORBIDDEN);
    case OpenStatus::UNAVAILABLE: {
        HttpResponse busy = HttpResponse::make_error(StatusCode::SERVICE_UNAVAILABLE);
        (void)busy.set_header("Retry-After", "1");
        return busy;
    }
    case OpenStatus::ERROR:
        return HttpResponse::make_error(StatusCode::INTERNAL_SERVER_ERROR);
    case OpenStatus::NOT_FOUND:
    case OpenStatus::OK:
        break;
    }
    return HttpResponse::make_error(StatusCode::NOT_FOUND);
}

// Le o arquivo inteiro (ate @p size bytes) com pread, que tem posicao explicita e repete em leitura curta.
// Se o arquivo encolher entre o fstat e a leitura, devolve so o que existe: o Content-Length sai igual ao corpo.
std::string read_small(const FileDescriptor& file, std::size_t size) {
    std::string body(size, '\0');
    std::size_t got = 0;
    while (got < size) {
        const ssize_t n = ::pread(file.get(), body.data() + got, size - got, static_cast<off_t>(got));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        got += static_cast<std::size_t>(n);
    }
    body.resize(got);
    return body;
}

} // namespace

HttpResponse serve_static(const std::filesystem::path& root, const HttpRequest& request) {
    if (request.method != HttpMethod::GET) {
        HttpResponse response = HttpResponse::make_error(StatusCode::METHOD_NOT_ALLOWED);
        (void)response.set_header("Allow", "GET"); // a RFC 9110 exige o Allow em 405
        return response;
    }

    std::optional<std::string> decoded = decode_target_path(request.path);
    if (!decoded) {
        return HttpResponse::make_error(StatusCode::BAD_REQUEST);
    }

    OpenedFile opened = open_static_file(root, *decoded);
    if (opened.status != OpenStatus::OK) {
        return error_for(opened.status);
    }

    const std::string_view content_type = mime_type(opened.path.extension().string());
    HttpResponse response{StatusCode::OK};
    if (opened.size <= kInlineStaticFileBytes) {
        response.set_body(read_small(opened.file, static_cast<std::size_t>(opened.size)), content_type);
    } else {
        response.set_file_body(std::move(opened.file), opened.size, content_type);
    }
    return response;
}

} // namespace citius
