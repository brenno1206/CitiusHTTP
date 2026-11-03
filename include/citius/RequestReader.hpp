#pragma once
#include "citius/Socket.hpp"
#include <cstddef>
#include <string>
#include <string_view>

namespace citius {

/// Como terminou a leitura da requisicao.
enum class ReadStatus {
    COMPLETE,  ///< chegou o fim dos cabecalhos (\r\n\r\n)
    CLOSED,    ///< o cliente fechou a conexao antes de completar
    TOO_LARGE, ///< passou do limite sem terminar os cabecalhos (responder 431)
    TIMEOUT,   ///< o prazo de leitura do socket estourou (responder 408)
    ERROR      ///< falha do sistema (recv)
};

struct ReadResult {
    ReadStatus status = ReadStatus::ERROR;
    std::string data; ///< bytes recebidos; pode conter o inicio do corpo apos os cabecalhos
    std::size_t head_size = 0; ///< bytes de data ate o fim de "\r\n\r\n" (so com COMPLETE); o resto e corpo
    std::string surplus; ///< so em read_request_body: bytes recebidos alem do corpo (inicio da proxima requisicao)
};

/// Le do socket, em laco, ate encontrar o fim dos cabecalhos HTTP (\r\n\r\n).
///
/// O TCP nao preserva fronteiras de mensagem, entao a requisicao pode chegar em varios
/// pedacos (ate com o delimitador partido ao meio). Todo byte recebido e mantido em
/// ReadResult::data, inclusive os que vierem depois dos cabecalhos.
///
/// Se o socket tiver prazo (Socket::set_recv_timeout), um cliente parado resulta em TIMEOUT.
///
/// @param socket     conexao do cliente
/// @param max_bytes  teto para a cabeca (inclusive o "\r\n\r\n"); protege contra clientes que nunca terminam
/// @param initial    bytes ja lidos antes (sobra da requisicao anterior numa conexao keep-alive); sao
///                   examinados antes de qualquer recv, entao uma requisicao completa em @p initial
///                   nao toca no socket. Copiados para ReadResult::data.
[[nodiscard]] ReadResult read_request_head(Socket& socket, std::size_t max_bytes, std::string_view initial = {});

/// Completa o corpo da requisicao: parte dele pode ja ter chegado junto com a cabeca.
///
/// @param already_read bytes do corpo que ja estao em maos (data.substr(head_size) da cabeca)
/// @param length       tamanho declarado em Content-Length (o chamador ja o limitou)
/// @return COMPLETE com ReadResult::data tendo exatamente @p length bytes; o que sobrar em
///         @p already_read (requisicoes encadeadas) vai para ReadResult::surplus. Nunca le do socket
///         alem do corpo. Ou CLOSED/TIMEOUT/ERROR se a leitura falhou.
[[nodiscard]] ReadResult read_request_body(Socket& socket, std::string_view already_read, std::size_t length);

} // namespace citius
