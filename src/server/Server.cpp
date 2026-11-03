#include "citius/server/Server.hpp"
#include "citius/files/StaticFiles.hpp"
#include "citius/http/HttpRequest.hpp"
#include "citius/http/HttpResponse.hpp"
#include "citius/http/StatusCode.hpp"
#include "citius/net/Listener.hpp"
#include "citius/net/Poller.hpp"
#include "citius/net/Socket.hpp"
#include "citius/server/Connection.hpp"
#include "citius/util/Logger.hpp"
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <iterator>
#include <memory>
#include <stdexcept> // Para lançar std::runtime_error
#include <thread>
#include <unordered_map>
#include <utility> // std::move

namespace citius {

namespace {

// De quanto em quanto tempo o loop acorda mesmo sem eventos: para notar request_stop() (que so grava
// um atomic e nao consegue acordar o Poller) e para varrer os prazos das conexoes
constexpr std::chrono::milliseconds kLoopTick{50};

// Quantos clientes aceitar por aviso do socket de escuta. Em rajadas isso poupa chamadas ao Poller;
// o limite evita que uma enxurrada de conexoes novas segure o loop longe das que ja estao abertas
// (o aviso e por nivel: se sobrar cliente na fila, o proximo wait() avisa de novo)
constexpr int kAcceptBatch = 64;

// Pausa depois de um erro persistente de accept (ex.: sem fds, EMFILE), para nao girar em laco apertado
constexpr auto kAcceptErrorPause = std::chrono::milliseconds(10);

// Uma conexao e o que o Poller foi mandado vigiar nela (para so chamar modify() quando mudar).
struct Entry {
    std::unique_ptr<Connection> conn;
    Interest registered;
};

using ConnectionMap = std::unordered_map<int, Entry>;

// Depois de mexer numa conexao: acompanha o interesse dela no Poller.
// @return false se ela fechou (ou nao da mais para vigiar) e deve ser descartada; o destrutor fecha o fd.
bool sync_interest(Poller& poller, Entry& entry) {
    if (entry.conn->closed()) return false;
    const Interest wanted = entry.conn->interest();
    if (wanted != entry.registered) {
        if (!poller.modify(entry.conn->fd(), wanted)) {
            log_debug("Falha ao atualizar o interesse da conexao no Poller.");
            return false;
        }
        entry.registered = wanted;
    }
    return true;
}

// O cliente passou do limite de conexoes: uma tentativa de avisar e adeus. Nao ha laco de envio
// (a resposta e minuscula e cabe no buffer de um socket novo) nem espera: nada aqui pode bloquear o loop.
void reject_busy(Socket client) {
    HttpResponse busy = HttpResponse::make_error(StatusCode::SERVICE_UNAVAILABLE);
    (void)busy.set_header("Retry-After", "1");
    (void)busy.set_header("Connection", "close");
    if (!client.set_no_sigpipe()) return;
    (void)client.send_some(busy.serialize());
}

// O event loop em si: um objeto por chamada a run()/serve_one(), com o estado que existe enquanto ele
// roda (Poller, conexoes, se ainda aceita, se esta encerrando). Cada metodo cuida de uma coisa: aceitar,
// tratar um evento, varrer prazos, encerrar.
class EventLoop {
public:
    using Clock = Connection::Clock;
    using AtomicMillis = std::atomic<std::chrono::milliseconds::rep>;

    /// @param single_client aceita um cliente so e termina quando a conexao dele acabar (serve_one)
    /// @param read_timeout_ms,idle_timeout_ms lidos a cada cliente novo: o Server pode muda-los durante run()
    EventLoop(Listener& listener, bool single_client, std::size_t max_connections, Connection::Handler handler,
              const AtomicMillis& read_timeout_ms, const AtomicMillis& idle_timeout_ms, const std::atomic<bool>& stop)
        : m_listener(listener), m_single_client(single_client), m_max_connections(max_connections),
          m_handler(std::move(handler)), m_read_timeout_ms(read_timeout_ms), m_idle_timeout_ms(idle_timeout_ms),
          m_stop(stop), m_next_sweep(Clock::now() + kLoopTick) {
        if (!m_poller.add(m_listener.fd(), Interest::READ)) {
            throw std::runtime_error("Falha ao registrar o socket de escuta no Poller.");
        }
    }

    void run() {
        while (true) {
            if (m_stop.load() && !m_draining) begin_draining();
            // sem aceitar mais (parada, ou o unico cliente de serve_one) e sem conexoes: acabou
            if (!m_accepting && m_connections.empty()) return;

            for (const PollEvent& event : m_poller.wait(kLoopTick)) handle_event(event, Clock::now());
            sweep_deadlines(Clock::now());
        }
    }

private:
    // Aplica @p fn a cada conexao e descarta as que fecharam (ou nao da mais para vigiar).
    template <typename Fn> void update_all(Fn&& fn) {
        for (auto it = m_connections.begin(); it != m_connections.end();) {
            fn(*it->second.conn);
            it = sync_interest(m_poller, it->second) ? std::next(it) : m_connections.erase(it);
        }
    }

    void stop_accepting() {
        if (m_accepting) {
            (void)m_poller.remove(m_listener.fd());
            m_accepting = false;
        }
    }

    // Encerramento gracioso: nao aceita mais; conexao ociosa fecha na hora, as demais terminam a requisicao em curso.
    void begin_draining() {
        m_draining = true;
        stop_accepting();
        if (!m_single_client) log_info("Encerrando: esperando os clientes em andamento...");
        update_all([](Connection& conn) { conn.close_when_idle(); });
    }

    void accept_pending(Clock::time_point now) {
        for (int i = 0; i < kAcceptBatch && m_accepting; ++i) {
            if (!accept_one(now)) return;
        }
    }

    // @return false quando nao ha mais cliente a aceitar por agora (ou o accept falhou)
    bool accept_one(Clock::time_point now) {
        AcceptResult accepted = m_listener.accept();
        switch (accepted.status) {
        case AcceptStatus::ACCEPTED:
            break;
        case AcceptStatus::RETRY:
            return true; // sinal, ou o cliente desistiu: tenta o proximo
        case AcceptStatus::EMPTY:
            return false; // fila de espera vazia
        case AcceptStatus::EXHAUSTED:
        case AcceptStatus::FAILED:
            log_error("Falha ao aceitar conexao.");
            std::this_thread::sleep_for(kAcceptErrorPause);
            return false;
        }

        Socket client = std::move(accepted.socket);
        if (m_connections.size() >= m_max_connections) {
            log_warn("(limite de conexoes) -> 503");
            reject_busy(std::move(client));
        } else {
            admit(std::move(client), now);
        }
        return true;
    }

    void admit(Socket client, Clock::time_point now) {
        ConnectionOptions options;
        options.read_timeout = std::chrono::milliseconds(m_read_timeout_ms.load());
        options.idle_timeout = std::chrono::milliseconds(m_idle_timeout_ms.load());
        auto conn = std::make_unique<Connection>(std::move(client), m_handler, now, options);
        if (conn->closed()) return;

        const int conn_fd = conn->fd();
        if (!m_poller.add(conn_fd, Interest::READ)) {
            log_warn("Falha ao registrar o cliente no Poller.");
            return; // o destrutor de 'conn' fecha o fd
        }
        m_connections.emplace(conn_fd, Entry{std::move(conn), Interest::READ});
        if (m_single_client) stop_accepting(); // serve_one atende um cliente so
    }

    void handle_event(const PollEvent& event, Clock::time_point now) {
        if (event.fd == m_listener.fd()) {
            if (m_accepting) accept_pending(now);
            return;
        }
        auto const it = m_connections.find(event.fd);
        if (it == m_connections.end()) return;

        Connection& conn = *it->second.conn;
        if (event.readable) conn.on_readable(now);
        // "closed" sem "writable" acontece quando o par some durante o envio: on_writable descobre pelo erro do send
        if (!conn.closed() && (event.writable || event.closed)) conn.on_writable(now);
        if (!sync_interest(m_poller, it->second)) m_connections.erase(it);
    }

    // Prazos: varredura periodica, nao a cada evento (com milhares de conexoes seria custo por evento).
    void sweep_deadlines(Clock::time_point now) {
        if (now < m_next_sweep) return;
        m_next_sweep = now + kLoopTick;
        update_all([now](Connection& conn) { conn.check_deadline(now); });
    }

    Poller m_poller;
    Listener& m_listener;
    bool m_single_client;
    std::size_t m_max_connections;
    Connection::Handler m_handler;
    const AtomicMillis& m_read_timeout_ms;
    const AtomicMillis& m_idle_timeout_ms;
    const std::atomic<bool>& m_stop;
    ConnectionMap m_connections;
    bool m_accepting = true;
    bool m_draining = false;
    Clock::time_point m_next_sweep;
};

// Validado antes de o Listener abrir a porta: um argumento invalido nao deve deixar uma porta aberta por um instante.
std::size_t validated_max_connections(std::size_t max_connections) {
    if (max_connections == 0) {
        throw std::invalid_argument("Server: max_connections deve ser maior que zero.");
    }
    return max_connections;
}

} // namespace

Server::Server(std::uint16_t port, std::filesystem::path root, std::size_t max_connections)
    : m_root(std::move(root)), m_max_connections(validated_max_connections(max_connections)), m_listener(port) {
    // captura a raiz por valor: o fallback nao pode depender do endereco do Server (que e movivel)
    m_router.set_fallback([root = m_root](const HttpRequest& request) { return serve_static(root, request); });
    log_info(std::format("Servidor escutando na porta {}...", m_listener.port()));
}

void Server::set_read_timeout(std::chrono::milliseconds timeout) noexcept {
    if (timeout > std::chrono::milliseconds::zero()) {
        m_read_timeout_ms.store(timeout.count());
    }
}

void Server::set_idle_timeout(std::chrono::milliseconds timeout) noexcept {
    if (timeout > std::chrono::milliseconds::zero()) {
        m_idle_timeout_ms.store(timeout.count());
    }
}

void Server::serve_one() {
    event_loop(true);
}

void Server::run() {
    event_loop(false);
}

void Server::event_loop(bool single_client) {
    EventLoop loop{m_listener,
                   single_client,
                   m_max_connections,
                   [this](const HttpRequest& request) { return m_router.route(request); },
                   m_read_timeout_ms,
                   m_idle_timeout_ms,
                   m_stop};
    loop.run();
}

} // namespace citius
