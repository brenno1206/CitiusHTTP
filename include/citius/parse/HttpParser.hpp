#pragma once
#include "citius/http/HttpRequest.hpp"
#include "citius/http/StatusCode.hpp"
#include <cstddef>
#include <optional>
#include <string_view>
#include <utility>

namespace citius {

/// Por que uma requisicao foi rejeitada. Cada valor corresponde a um status HTTP.
/// (405 Method Not Allowed nao esta aqui: depende do recurso, e quem sabe isso e o Router.)
enum class ParseError {
    BAD_REQUEST,           ///< 400: sintaxe invalida, Host ausente/duplicado, cabecalho malformado
    NOT_IMPLEMENTED,       ///< 501: metodo que o servidor nao conhece
    URI_TOO_LONG,          ///< 414: alvo maior que HttpParser::kMaxTargetLength
    EXPECTATION_FAILED,    ///< 417: Expect com um valor que nao seja "100-continue"
    VERSION_NOT_SUPPORTED, ///< 505: versao bem formada, mas diferente de HTTP/1.0 e HTTP/1.1
    HEADERS_TOO_LARGE,     ///< 431: mais de HttpParser::kMaxHeaderCount cabecalhos
    PAYLOAD_TOO_LARGE      ///< 413: Content-Length maior que HttpParser::kMaxBodyLength
};

/// @return o status HTTP que corresponde a @p error (ver a tabela em ParseError).
[[nodiscard]] StatusCode to_status(ParseError error) noexcept;

/// Resultado de HttpParser::parse: ou ha uma requisicao, ou ha o motivo do erro.
struct ParseResult {
    std::optional<HttpRequest> request; ///< preenchido se, e somente se, o parse deu certo
    std::optional<ParseError> error;    ///< preenchido se, e somente se, o parse falhou

    [[nodiscard]] bool ok() const noexcept {
        return request.has_value();
    }

    [[nodiscard]] static ParseResult success(HttpRequest request) {
        return ParseResult{std::move(request), std::nullopt};
    }
    [[nodiscard]] static ParseResult failure(ParseError error) {
        return ParseResult{std::nullopt, error};
    }
};

/// Interpreta texto HTTP bruto. Nao conhece sockets: recebe texto e devolve estruturas.
class HttpParser {
public:
    static constexpr std::size_t kMaxTargetLength = 2048;      ///< bytes do alvo ("/caminho?query")
    static constexpr std::size_t kMaxHeaderCount = 64;         ///< linhas de cabecalho
    static constexpr std::size_t kMaxBodyLength = 1024 * 1024; ///< bytes de corpo aceitos (1 MB)

    /// Interpreta a cabeca de uma requisicao: linha de requisicao + cabecalhos + linha vazia
    /// final ("\r\n\r\n"). Bytes depois da linha vazia (corpo) sao ignorados.
    ///
    /// A entrada vem do cliente e e tratada como hostil: nunca lanca excecao. Segue a
    /// RFC 9112 (nomes de cabecalho sao tokens, sem espaco antes dos ":", sem linhas de
    /// continuacao). HTTP/1.1 exige exatamente um cabecalho Host.
    ///
    /// Content-Length vira HttpRequest::content_length. Aceita so uma sequencia de digitos, uma
    /// unica vez: qualquer outra forma (sinal, lista "5, 5", repetido) e 400, porque interpretacoes
    /// diferentes do tamanho entre servidores sao a base do request smuggling. Acima de
    /// kMaxBodyLength e 413. O corpo em si nao e lido aqui.
    /// Transfer-Encoding e recusado: 501 sozinho, 400 junto com Content-Length (request smuggling).
    ///
    /// Os dados sao copiados para o resultado, entao @p raw_head pode ser descartado depois.
    /// @return a requisicao ou o ParseError correspondente ao status a responder.
    [[nodiscard]] static ParseResult parse(std::string_view raw_head);
};

} // namespace citius
