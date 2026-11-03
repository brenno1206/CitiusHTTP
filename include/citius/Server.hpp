#pragma once

#include "citius/Socket.hpp"
#include <cstdint>
#include <filesystem>

namespace citius {

/// Servidor HTTP: escuta numa porta TCP e atende um cliente por vez (por enquanto).
/// Cada conexao recebe uma unica requisicao e e fechada em seguida (sem keep-alive).
class Server {
public:
    /// Abre o socket de escuta em @p port (IPv4, todas as interfaces).
    /// Com @p port == 0 o sistema escolhe uma porta livre; use port() para saber qual.
    /// @param root pasta de arquivos publicos. Caminho relativo vale a partir do diretorio de
    ///        trabalho do processo (por isso o servidor deve ser iniciado na raiz do projeto).
    /// @throws std::runtime_error se nao conseguir criar, associar ou escutar (ex.: porta ocupada).
    explicit Server(std::uint16_t port, std::filesystem::path root = "www");

    /// @return a porta em que o servidor realmente escuta.
    [[nodiscard]] std::uint16_t port() const noexcept { return m_port; }

    /// Espera um cliente, le a requisicao, responde e fecha a conexao.
    /// Falhas de um cliente (conexao cortada, requisicao invalida) nao lancam: sao registradas.
    void serve_one();

    /// Chama serve_one() em laco; so retorna se o processo for encerrado.
    void run();

private:
    void start(std::uint16_t port);

    Socket m_server_socket; ///< socket de escuta; fecha sozinho no destrutor
    std::uint16_t m_port = 0;
    std::filesystem::path m_root; ///< pasta de onde os arquivos estaticos sao servidos
};

} // namespace citius
