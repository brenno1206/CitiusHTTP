#pragma once

#include "citius/Router.hpp"
#include "citius/Socket.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>

namespace citius {

/// Quantos clientes aceitos podem esperar por uma thread livre em run() (padrao).
inline constexpr std::size_t kDefaultMaxQueuedClients = 128;

/// Servidor HTTP: escuta numa porta TCP; run() atende varios clientes ao mesmo tempo com um
/// ThreadPool. Cada conexao recebe uma unica requisicao e e fechada em seguida (sem keep-alive).
class Server {
public:
    /// Abre o socket de escuta em @p port (IPv4, todas as interfaces).
    /// Com @p port == 0 o sistema escolhe uma porta livre; use port() para saber qual.
    /// @param root pasta de arquivos publicos. Caminho relativo vale a partir do diretorio de
    ///        trabalho do processo (por isso o servidor deve ser iniciado na raiz do projeto).
    /// @param threads quantas threads atendem clientes em run(); 0 = uma por nucleo (minimo 2).
    /// @param max_queued quantos clientes podem esperar por uma thread livre; acima disso run() responde 503.
    /// @throws std::runtime_error se nao conseguir criar, associar ou escutar (ex.: porta ocupada).
    /// @throws std::invalid_argument se @p max_queued for zero.
    explicit Server(std::uint16_t port, std::filesystem::path root = "www", std::size_t threads = 0,
                    std::size_t max_queued = kDefaultMaxQueuedClients);

    /// @return a porta em que o servidor realmente escuta.
    [[nodiscard]] std::uint16_t port() const noexcept { return m_port; }

    /// Roteador das requisicoes. Caminhos sem rota caem nos arquivos estaticos de @c root.
    /// Registre as rotas antes de chamar run()/serve_one() (o Router nao e thread-safe para escrita).
    /// Um handler que lance excecao vira resposta 500; o servidor continua de pe.
    [[nodiscard]] Router& router() noexcept { return m_router; }

    /// Espera um cliente, le a requisicao, responde e fecha a conexao, tudo na thread que chamou.
    /// Falhas de um cliente (conexao cortada, requisicao invalida) nao lancam: sao registradas.
    /// Retorna sem atender ninguem se request_stop() for chamado enquanto espera.
    void serve_one();

    /// Aceita clientes em laco e os entrega ao ThreadPool (varios sao atendidos ao mesmo tempo).
    /// Se a fila do pool encher, o cliente recebe 503 com Retry-After.
    /// Retorna depois de request_stop(): para de aceitar, deixa terminar os clientes ja aceitos
    /// e so entao devolve o controle.
    void run();

    /// Pede para run()/serve_one() pararem. Pode ser chamado de outra thread ou de um handler de
    /// sinal (so grava um std::atomic lock-free). A parada leva ate ~100 ms.
    void request_stop() noexcept { m_stop.store(true); }

private:
    void start(std::uint16_t port);

    /// Espera (com poll, para poder checar m_stop) e aceita um cliente.
    /// @return o cliente, ou um Socket invalido se pediram para parar ou se o accept falhou.
    Socket accept_client();

    /// Le a requisicao de @p client, responde e deixa o chamador fechar o socket. Chamado de varias
    /// threads ao mesmo tempo: so le o estado do Server (roteador e raiz), sem alterar nada.
    void handle_client(Socket& client) const;

    Socket m_server_socket; ///< socket de escuta; fecha sozinho no destrutor
    std::uint16_t m_port = 0;
    std::filesystem::path m_root; ///< pasta de onde os arquivos estaticos sao servidos
    Router m_router;              ///< rotas registradas + fallback para m_root
    std::size_t m_threads;        ///< tamanho do pool de run()
    std::size_t m_max_queued;     ///< fila de espera do pool de run()
    std::atomic<bool> m_stop = false;
    static_assert(std::atomic<bool>::is_always_lock_free, "request_stop() precisa ser seguro em handler de sinal");
};

} // namespace citius
