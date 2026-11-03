#include "citius/Router.hpp"
#include <stdexcept>
#include <string>
#include <utility>

namespace citius {

namespace {

// A query nao faz parte do caminho da rota; o alvo "/ping?x=1" casa com "/ping".
std::string_view without_query(std::string_view target) {
    return target.substr(0, target.find('?'));
}

} // namespace

void Router::add(HttpMethod method, std::string_view path, Handler handler) {
    if (path.empty() || path.front() != '/' || path.find('?') != std::string_view::npos) {
        throw std::invalid_argument("Rota invalida: deve comecar com '/' e nao ter query string.");
    }
    if (!handler) {
        throw std::invalid_argument("Handler vazio.");
    }
    m_routes[std::string{path}][method] = std::move(handler);
}

void Router::set_fallback(Handler handler) {
    if (!handler) {
        throw std::invalid_argument("Handler vazio.");
    }
    m_fallback = std::move(handler);
}

HttpResponse Router::route(const HttpRequest& request) const {
    auto route = m_routes.find(without_query(request.path));
    if (route == m_routes.end()) {
        return m_fallback ? m_fallback(request) : HttpResponse::make_error(StatusCode::NOT_FOUND);
    }

    const MethodHandlers& handlers = route->second;
    if (auto handler = handlers.find(request.method); handler != handlers.end()) {
        return handler->second(request);
    }

    // a RFC 9110 exige o Allow em 405; o std::map ordena pelo enum (GET, POST, PUT, DELETE)
    std::string allow;
    for (const auto& entry : handlers) {
        if (!allow.empty()) {
            allow += ", ";
        }
        allow += method_name(entry.first);
    }
    HttpResponse response = HttpResponse::make_error(StatusCode::METHOD_NOT_ALLOWED);
    (void)response.set_header("Allow", allow);
    return response;
}

} // namespace citius
