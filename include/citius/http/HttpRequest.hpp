#pragma once
#include "citius/http/HttpSyntax.hpp"
#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <string_view>

namespace citius {

/// Metodos HTTP reconhecidos. UNKNOWN indica metodo nao suportado.
enum class HttpMethod { GET, POST, PUT, DELETE, UNKNOWN };

/// @return o nome do metodo como aparece na requisicao ("GET"), ou "?" para UNKNOWN.
[[nodiscard]] constexpr std::string_view method_name(HttpMethod method) noexcept {
    switch (method) {
    case HttpMethod::GET:
        return "GET";
    case HttpMethod::POST:
        return "POST";
    case HttpMethod::PUT:
        return "PUT";
    case HttpMethod::DELETE:
        return "DELETE";
    case HttpMethod::UNKNOWN:
        break;
    }
    return "?";
}

/// Requisicao HTTP ja interpretada. Os campos sao copias (nao dependem do buffer original).
struct HttpRequest {
    /// Cabecalhos: nome sempre em minusculas ("content-type"); valor sem espacos nas pontas.
    /// Cabecalhos repetidos sao juntados com ", ". O comparador transparente permite
    /// buscar com std::string_view: headers.find("host").
    using Headers = std::map<std::string, std::string, std::less<>>;

    HttpMethod method = HttpMethod::UNKNOWN;
    std::string path;    ///< alvo da requisicao como veio, incluindo query string, ex.: "/busca?q=1"
    std::string version; ///< "HTTP/1.0" ou "HTTP/1.1"
    Headers headers;
    std::size_t content_length = 0; ///< tamanho do corpo declarado em Content-Length (0 se ausente ou se chunked)
    bool chunked = false; ///< o corpo vem em "Transfer-Encoding: chunked": o tamanho so se sabe depois de decodifica-lo
    bool expect_continue =
        false;        ///< o cliente enviou "Expect: 100-continue" (so em HTTP/1.1) e espera o 100 antes do corpo
    std::string body; ///< corpo; o HttpParser nao o preenche, quem le o socket o faz

    /// @return true se a conexao pode ser reaproveitada depois desta requisicao (RFC 9112, 9.3):
    ///         HTTP/1.1 e persistente, a menos que o cabecalho Connection tenha o token "close";
    ///         HTTP/1.0 so e persistente com o token "keep-alive". "close" vence "keep-alive".
    [[nodiscard]] bool keep_alive() const {
        const bool persistent_by_default = version == "HTTP/1.1";
        auto const it = headers.find("connection");
        if (it == headers.end()) return persistent_by_default;

        bool has_close = false;
        bool has_keep_alive = false;
        std::string_view list = it->second;
        while (!list.empty()) {
            std::size_t const comma = list.find(',');
            std::string_view token = list.substr(0, comma);
            list = comma == std::string_view::npos ? std::string_view{} : list.substr(comma + 1);
            while (!token.empty() && (token.front() == ' ' || token.front() == '\t')) token.remove_prefix(1);
            while (!token.empty() && (token.back() == ' ' || token.back() == '\t')) token.remove_suffix(1);
            has_close = has_close || iequals(token, "close");
            has_keep_alive = has_keep_alive || iequals(token, "keep-alive");
        }
        if (has_close) return false;
        return has_keep_alive || persistent_by_default;
    }
};

} // namespace citius
