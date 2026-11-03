#pragma once
#include "citius/http/HttpRequest.hpp"
#include "citius/http/StatusCode.hpp"
#include "citius/parse/ChunkedDecoder.hpp"
#include "citius/parse/HttpParser.hpp"
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace citius {

/// Em que pé está a montagem da requisição.
enum class AssembleStatus {
    NEED_MORE, ///< faltam bytes (ou nao ha nenhum): leia mais da conexao e chame feed() de novo
    COMPLETE,  ///< ha uma requisicao inteira: retire-a com take_request()
    ERROR      ///< requisicao invalida ou grande demais: responda error_status() e feche a conexao
};

/// Monta requisições HTTP a partir de bytes que chegam aos poucos, sem ler de socket nem bloquear.
///
/// Num event loop `recv` devolve o que houver naquele instante (meia linha, só os cabeçalhos, duas
/// requisições coladas...), e não dá para esperar o resto. O assembler guarda o estado entre uma
/// chamada e outra: o que um servidor bloqueante faria lendo do socket em laço até a
/// requisição inteira chegar, ele faz em pedaços. Mesmas regras: cabeça de até `max_head_bytes`, `Content-Length`
/// validado pelo HttpParser (413 acima de HttpParser::kMaxBodyLength, sem esperar o corpo), delimitador partido entre
/// leituras, e requisições encadeadas (pipelining).
///
/// Uso: `feed(bytes)` a cada leitura; em COMPLETE, `take_request()`; depois `feed("")` para ver se a
/// próxima requisição já estava nos bytes guardados. Não conhece sockets (camada http/); não é thread-safe.
class RequestAssembler {
public:
    /// Teto padrão para linha de requisição + cabeçalhos; acima disso, 431 (8 KB, como o nginx).
    static constexpr std::size_t kDefaultMaxHeadBytes = 8192;

    /// @param max_head_bytes teto para a cabeça (inclusive o "\r\n\r\n").
    explicit RequestAssembler(std::size_t max_head_bytes = kDefaultMaxHeadBytes) noexcept
        : m_max_head_bytes(max_head_bytes) {}

    /// Acrescenta @p bytes (pode ser vazio) e avança a montagem o quanto der. Os dados são copiados.
    /// Com uma requisição pronta e ainda não retirada, os bytes só são guardados (para a próxima), e o
    /// resultado continua COMPLETE: pare de ler da conexão enquanto processa uma requisição, para
    /// que a sobra guardada não cresça sem limite. Depois de um ERROR nada mais é interpretado.
    [[nodiscard]] AssembleStatus feed(std::string_view bytes);

    /// Retira a requisição completa (com o corpo preenchido). Bytes além dela ficam guardados para a
    /// próxima: chame feed("") para continuar.
    /// @throws std::logic_error se o último feed() não devolveu COMPLETE (bug do chamador).
    [[nodiscard]] HttpRequest take_request();

    /// @return o status a responder (400, 413, 431, 501, 505...) depois de um ERROR.
    /// @throws std::logic_error se não houve erro.
    [[nodiscard]] StatusCode error_status() const;

    /// @return true UMA vez se o cliente mandou "Expect: 100-continue", a cabeça foi aceita e o corpo ainda não chegou:
    ///         o chamador deve enviar "HTTP/1.1 100 Continue" para o cliente começar a enviar o corpo (RFC
    ///         9110 10.1.1). Nunca é true se o corpo já chegou junto com a cabeça, se não há corpo, ou se a cabeça foi
    ///         recusada (413, 417...): não se convida o cliente a enviar o que vai ser recusado.
    [[nodiscard]] bool take_continue() noexcept;

    /// @return true se há bytes guardados (requisição pela metade ou sobra da próxima). Serve para
    ///         distinguir uma conexão ociosa (nada guardado: fechar em silêncio) de uma requisição
    ///         parada no meio (responder 408).
    [[nodiscard]] bool has_buffered_data() const noexcept {
        return !m_buffer.empty() || m_state == State::BODY || m_state == State::CHUNKED_BODY;
    }

private:
    enum class State { HEAD, BODY, CHUNKED_BODY, DONE, FAILED };

    AssembleStatus advance();
    /// Procura e interpreta a cabeca. @return o resultado se ela ainda nao esta completa ou e invalida; std::nullopt se
    /// foi aceita (o estado passa a BODY ou CHUNKED_BODY).
    std::optional<AssembleStatus> advance_head();
    AssembleStatus advance_chunked_body();
    AssembleStatus advance_fixed_body();
    AssembleStatus complete();
    AssembleStatus fail(StatusCode status);

    std::size_t m_max_head_bytes;
    State m_state = State::HEAD;
    std::string m_buffer;          ///< bytes recebidos e ainda não consumidos (na fase BODY, só o corpo)
    std::size_t m_search_from = 0; ///< onde retomar a busca por "\r\n\r\n" (ele pode vir partido)
    HttpRequest m_request;         ///< cabeça já interpretada, enquanto o corpo chega
    ChunkedDecoder m_decoder{HttpParser::kMaxBodyLength}; ///< só usado quando o corpo é "chunked"
    bool m_continue_pending = false; ///< a cabeça pediu "Expect: 100-continue" e o chamador ainda não foi avisado
    std::optional<StatusCode> m_error;
};

} // namespace citius
