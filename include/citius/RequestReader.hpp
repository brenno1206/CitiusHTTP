#pragma once
#include "citius/Socket.hpp"
#include <cstddef>
#include <string>

namespace citius {

/// Como terminou a leitura da requisicao.
enum class ReadStatus {
    COMPLETE,  ///< chegou o fim dos cabecalhos (\r\n\r\n)
    CLOSED,    ///< o cliente fechou a conexao antes de completar
    TOO_LARGE, ///< passou do limite sem terminar os cabecalhos (responder 431)
    ERROR      ///< falha do sistema (recv)
};

struct ReadResult {
    ReadStatus status = ReadStatus::ERROR;
    std::string data; ///< bytes recebidos; pode conter o inicio do corpo apos os cabecalhos
};

/// Le do socket, em laco, ate encontrar o fim dos cabecalhos HTTP (\r\n\r\n).
///
/// O TCP nao preserva fronteiras de mensagem, entao a requisicao pode chegar em varios
/// pedacos (ate com o delimitador partido ao meio). Todo byte recebido e mantido em
/// ReadResult::data, inclusive os que vierem depois dos cabecalhos.
///
/// @param socket     conexao do cliente
/// @param max_bytes  teto para o total lido; protege contra clientes que nunca terminam
[[nodiscard]] ReadResult read_request_head(Socket& socket, std::size_t max_bytes);

} // namespace citius
