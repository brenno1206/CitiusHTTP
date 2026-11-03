#pragma once
#include <string_view>

namespace citius {

/// Escreve uma linha em stdout (o '\n' final e acrescentado aqui; nao o inclua em @p line).
/// Seguro para varias threads: uma linha nunca se mistura com outra, e as threads nao disputam
/// o estado de formatacao de std::cout (isso e uma corrida de dados, achada pelo TSan).
/// Solucao minima ate o Logger com niveis (Fase 6).
void log_info(std::string_view line);

/// Igual a log_info(), mas para stderr.
void log_error(std::string_view line);

} // namespace citius
