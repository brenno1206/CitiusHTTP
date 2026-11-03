#pragma once

#include "citius/net/Socket.hpp"
#include "citius/routing/Router.hpp"
#include "citius/server/Connection.hpp"
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>

namespace citius {

// kDefaultReadTimeout, kDefaultIdleTimeout e kMaxRequestsPerConnection moraram aqui; agora estao em Connection.hpp.

/// Quantas conexoes abertas ao mesmo tempo o servidor aceita (padrao). Acima disso, 503.
/// Cada conexao usa um descritor de arquivo: o limite do processo (ulimit -n) precisa ser maior.
inline constexpr std::size_t kDefaultMaxConnections = 4096;

/// Servidor HTTP: escuta numa porta TCP e atende todos os clientes numa unica thread, com um event
/// loop (kqueue no macOS, epoll no Linux; ver Poller). Em vez de uma thread bloqueada por conexao,
/// o loop pergunta ao SO quais conexoes tem algo a fazer e so mexe nessas: milhares de clientes
/// parados custam quase nada. Cada cliente e uma Connection (maquina de estados nao bloqueante) que
/// faz keep-alive (RFC 9112 secao 9.3), com prazos de leitura, ociosidade e escrita.
///
/// Como tudo roda na thread de run(), um handler lento (ou a leitura de um arquivo grande) atrasa
/// todos os clientes: handlers devem ser curtos.
class Server {
public:
    /// Abre o socket de escuta em @p port (IPv4, todas as interfaces).
    /// Com @p port == 0 o sistema escolhe uma porta livre; use port() para saber qual.
    /// @param root pasta de arquivos publicos. Caminho relativo vale a partir do diretorio de
    ///        trabalho do processo (por isso o servidor deve ser iniciado na raiz do projeto).
    /// @param max_connections quantas conexoes podem estar abertas ao mesmo tempo; acima disso o
    ///        cliente novo recebe 503 e e desconectado.
    /// @throws std::runtime_error se nao conseguir criar, associar ou escutar (ex.: porta ocupada).
    /// @throws std::invalid_argument se @p max_connections for zero.
    explicit Server(std::uint16_t port, std::filesystem::path root = "www",
                    std::size_t max_connections = kDefaultMaxConnections);

    /// @return a porta em que o servidor realmente escuta.
    [[nodiscard]] std::uint16_t port() const noexcept {
        return m_port;
    }

    /// Roteador das requisicoes. Caminhos sem rota caem nos arquivos estaticos de @c root.
    /// Registre as rotas antes de chamar run()/serve_one() (o Router nao e thread-safe para escrita).
    /// Um handler que lance excecao vira resposta 500; o servidor continua de pe.
    [[nodiscard]] Router& router() noexcept {
        return m_router;
    }

    /// Define em quanto tempo uma requisicao deve chegar inteira (cabecalhos e corpo); estourou, o
    /// cliente recebe 408 e a conexao fecha. O prazo e da requisicao toda, nao de cada leitura. Vale
    /// para os clientes aceitos depois da chamada e pode ser chamado de qualquer thread. Prazo <= 0 e ignorado.
    void set_read_timeout(std::chrono::milliseconds timeout) noexcept;

    /// Define quanto uma conexao keep-alive pode esperar a proxima requisicao; estourou, o servidor
    /// a fecha sem responder nada (nao ha requisicao a responder). Vale para os clientes aceitos depois
    /// da chamada e pode ser chamado de qualquer thread. Prazo <= 0 e ignorado.
    void set_idle_timeout(std::chrono::milliseconds timeout) noexcept;

    /// Atende um unico cliente: espera a conexao, serve as requisicoes dela ate ela fechar (keep-alive
    /// incluido) e retorna. Falhas do cliente (conexao cortada, requisicao invalida) nao lancam: sao
    /// registradas. Retorna sem atender ninguem se request_stop() for chamado antes de um cliente chegar.
    void serve_one();

    /// Atende clientes em laco ate request_stop(). Cada cliente aceito vira uma Connection; acima de
    /// max_connections o cliente recebe 503 com Retry-After.
    /// Retorna depois de request_stop(): para de aceitar, fecha as conexoes ociosas, deixa terminar
    /// as requisicoes em andamento (cada uma com seu prazo) e so entao devolve o controle.
    /// @throws std::runtime_error se o SO nao criar o kqueue/epoll.
    void run();

    /// Pede para run()/serve_one() pararem. Pode ser chamado de outra thread ou de um handler de
    /// sinal (so grava um std::atomic lock-free). A parada leva ate ~50 ms para ser notada.
    void request_stop() noexcept {
        m_stop.store(true);
    }

private:
    void start(std::uint16_t port);

    /// O event loop de run() e serve_one(). Com @p single_client, aceita um cliente so e retorna
    /// quando a conexao dele terminar.
    void event_loop(bool single_client);

    Socket m_server_socket; ///< socket de escuta (nao bloqueante); fecha sozinho no destrutor
    std::uint16_t m_port = 0;
    std::filesystem::path m_root; ///< pasta de onde os arquivos estaticos sao servidos
    Router m_router;              ///< rotas registradas + fallback para m_root
    std::size_t m_max_connections;
    std::atomic<std::chrono::milliseconds::rep> m_read_timeout_ms{kDefaultReadTimeout.count()};
    std::atomic<std::chrono::milliseconds::rep> m_idle_timeout_ms{kDefaultIdleTimeout.count()};
    std::atomic<bool> m_stop = false;
    static_assert(std::atomic<bool>::is_always_lock_free, "request_stop() precisa ser seguro em handler de sinal");
};

} // namespace citius
