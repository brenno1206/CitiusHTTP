#pragma once
#include "HttpRequest.hpp"
#include "StatusCode.hpp"
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
    VERSION_NOT_SUPPORTED, ///< 505: versao bem formada, mas diferente de HTTP/1.0 e HTTP/1.1
    HEADERS_TOO_LARGE      ///< 431: mais de HttpParser::kMaxHeaderCount cabecalhos
};

/// @return o status HTTP que corresponde a @p error (ver a tabela em ParseError).
[[nodiscard]] StatusCode to_status(ParseError error) noexcept;

/// Resultado de HttpParser::parse: ou ha uma requisicao, ou ha o motivo do erro.
struct ParseResult {
    std::optional<HttpRequest> request; ///< preenchido se, e somente se, o parse deu certo
    std::optional<ParseError> error;    ///< preenchido se, e somente se, o parse falhou

    [[nodiscard]] bool ok() const noexcept { return request.has_value(); }

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
    static constexpr std::size_t kMaxTargetLength = 2048; ///< bytes do alvo ("/caminho?query")
    static constexpr std::size_t kMaxHeaderCount = 64;    ///< linhas de cabecalho

    /// Interpreta a cabeca de uma requisicao: linha de requisicao + cabecalhos + linha vazia
    /// final ("\r\n\r\n"). Bytes depois da linha vazia (corpo) sao ignorados.
    ///
    /// A entrada vem do cliente e e tratada como hostil: nunca lanca excecao. Segue a
    /// RFC 9112 (nomes de cabecalho sao tokens, sem espaco antes dos ":", sem linhas de
    /// continuacao). HTTP/1.1 exige exatamente um cabecalho Host.
    ///
    /// Os dados sao copiados para o resultado, entao @p raw_head pode ser descartado depois.
    /// @return a requisicao ou o ParseError correspondente ao status a responder.
    [[nodiscard]] static ParseResult parse(std::string_view raw_head);
};

} // namespace citius
