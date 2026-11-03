#pragma once
#include "citius/parse/HttpParser.hpp"
#include <cstddef>
#include <string>
#include <string_view>

namespace citius {

/// Em que pe esta a decodificacao do corpo "chunked".
enum class ChunkedStatus {
    NEED_MORE, ///< faltam bytes: chame feed() de novo quando chegarem
    DONE,      ///< corpo completo (inclusive o ultimo chunk e os trailers)
    ERROR      ///< corpo invalido ou grande demais: ver ChunkedDecoder::error()
};

struct ChunkedProgress {
    ChunkedStatus status = ChunkedStatus::NEED_MORE;
    std::size_t consumed = 0; ///< quantos bytes de @c input foram usados; com DONE, o que sobrar e a proxima requisicao
};

/// Decodifica um corpo "Transfer-Encoding: chunked" (RFC 9112 secao 7.1) aos poucos, sem socket e sem bloquear:
///
///     chunk-size[;extensao] CRLF  dados CRLF   (repetido)   0[;extensao] CRLF  [trailers] CRLF
///
/// Como o recv() devolve o que houver (meia linha, um chunk no meio...), o decodificador guarda o estado entre
/// uma chamada e outra; os dados decodificados vao direto para a string do chamador.
///
/// E uma fronteira de seguranca (request smuggling: servidores que discordam de onde a mensagem termina), entao e
/// estrito:
/// - tamanho so em hexadecimal (1 a 16 digitos; sem sinal, sem "0x", sem espaco antes); chunk maior que o limite
///   restante do corpo e recusado assim que o tamanho e lido, sem esperar os dados;
/// - CRLF obrigatorio onde a RFC o exige: um LF sozinho e erro;
/// - extensoes ignoradas, mas sem caracteres de controle; trailers lidos e DESCARTADOS, validados como cabecalhos
///   (nome token, com ":") e limitados a kMaxTrailerBytes;
/// - nenhuma linha passa de kMaxLineBytes (um cliente que nunca manda o CRLF nao enche a memoria).
/// Depois de um ERROR nada mais e interpretado. Nao e thread-safe.
class ChunkedDecoder {
public:
    static constexpr std::size_t kMaxLineBytes = 1024;    ///< linha do tamanho (com extensoes) ou de um trailer
    static constexpr std::size_t kMaxTrailerBytes = 8192; ///< soma de todos os trailers

    /// @param max_body_bytes teto para o corpo decodificado (a soma dos chunks); acima, ERROR com PAYLOAD_TOO_LARGE.
    explicit ChunkedDecoder(std::size_t max_body_bytes) noexcept : m_max_body(max_body_bytes) {}

    /// Consome o que puder de @p input e acrescenta os dados decodificados a @p body.
    /// @return o progresso; com DONE, @c consumed e onde a mensagem terminou (o resto de @p input nao e do corpo).
    [[nodiscard]] ChunkedProgress feed(std::string_view input, std::string& body);

    /// @return o motivo do erro: BAD_REQUEST (400) ou PAYLOAD_TOO_LARGE (413). So vale depois de um ERROR.
    [[nodiscard]] ParseError error() const noexcept {
        return m_error;
    }

private:
    enum class State { SIZE_LINE, DATA, CHUNK_END, TRAILER_LINE, DONE, FAILED };

    /// Acumula bytes em m_line ate o CRLF. @return true quando uma linha completa (sem o CRLF) esta em m_line.
    bool read_line(std::string_view input, std::size_t& used);
    void parse_size_line();
    void parse_trailer_line();
    void fail(ParseError error) noexcept;

    std::size_t m_max_body;
    std::size_t m_total = 0;     ///< bytes de corpo ja decodificados
    std::size_t m_remaining = 0; ///< bytes que faltam do chunk atual
    std::size_t m_trailer_bytes = 0;
    State m_state = State::SIZE_LINE;
    bool m_cr_seen = false; ///< em CHUNK_END: o CR ja chegou, falta o LF
    std::string m_line;
    ParseError m_error = ParseError::BAD_REQUEST;
};

} // namespace citius
