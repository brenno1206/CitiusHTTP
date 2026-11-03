#pragma once
#include "citius/HttpParser.hpp"
#include "citius/HttpRequest.hpp"
#include "citius/StatusCode.hpp"
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
/// chamada e outra: o que `read_request_head` + `read_request_body` fazem bloqueando, ele faz em
/// pedaços. Mesmas regras: cabeça de até `max_head_bytes`, `Content-Length` validado pelo
/// HttpParser (413 acima de HttpParser::kMaxBodyLength, sem esperar o corpo), delimitador partido
/// entre leituras, e requisições encadeadas (pipelining).
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

    /// @return true se há bytes guardados (requisição pela metade ou sobra da próxima). Serve para
    ///         distinguir uma conexão ociosa (nada guardado: fechar em silêncio) de uma requisição
    ///         parada no meio (responder 408).
    [[nodiscard]] bool has_buffered_data() const noexcept {
        return !m_buffer.empty() || m_state == State::BODY;
    }

private:
    enum class State { HEAD, BODY, DONE, FAILED };

    AssembleStatus advance();
    AssembleStatus fail(StatusCode status);

    std::size_t m_max_head_bytes;
    State m_state = State::HEAD;
    std::string m_buffer;          ///< bytes recebidos e ainda não consumidos (na fase BODY, só o corpo)
    std::size_t m_search_from = 0; ///< onde retomar a busca por "\r\n\r\n" (ele pode vir partido)
    HttpRequest m_request;         ///< cabeça já interpretada, enquanto o corpo chega
    std::optional<StatusCode> m_error;
};

} // namespace citius
