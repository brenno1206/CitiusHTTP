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
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <functional>
#include <iterator>
#include <memory>
#include <queue>
#include <stdexcept> // Para lançar std::runtime_error
#include <unordered_map>
#include <utility> // std::move
#include <vector>

namespace citius {

namespace {

// De quanto em quanto tempo o loop acorda mesmo sem eventos: para notar request_stop() (que so grava
// um atomic e nao consegue acordar o Poller) e para varrer os prazos das conexoes
constexpr std::chrono::milliseconds kLoopTick{50};

// Quantos clientes aceitar por aviso do socket de escuta. A fila de espera do kernel e curta (128 no macOS): numa
// rajada de reconexoes, aceitar so 64 por volta do loop deixava a fila estourar e o cliente levava RST (medido com
// o wrk, ver docs/benchmarks.md). O limite existe para uma enxurrada de conexoes novas nao segurar o loop longe das
// que ja estao abertas (o aviso e por nivel: se sobrar cliente na fila, o proximo wait() avisa de novo).
constexpr int kAcceptBatch = 1024;

// Depois de um erro de accept (ex.: sem descritores, EMFILE) o servidor para de vigiar o socket de escuta por um tempo
// que dobra a cada falha seguida, de kMinAcceptBackoff ate kMaxAcceptBackoff, e volta sozinho. Dormir no laco seria
// pior: travaria todos os clientes, e vigiar o socket sem aceitar giraria em 100% de CPU (o aviso por nivel nao some).
constexpr std::chrono::milliseconds kMinAcceptBackoff{10};
constexpr std::chrono::milliseconds kMaxAcceptBackoff{1000};

// Uma conexao e o que o Poller foi mandado vigiar nela (para so chamar modify() quando mudar).
struct Entry {
    std::unique_ptr<Connection> conn;
    Interest registered;
    // O prazo mais cedo que ja esta na fila de prazos para esta conexao (max() = nenhum). Serve para so agendar de
    // novo quando o prazo ADIANTA: se ele so e adiado, a entrada antiga dispara, ve que ainda nao venceu e reagenda.
    Connection::Clock::time_point scheduled = Connection::Clock::time_point::max();
};

// Um prazo agendado: "olhe a conexao do fd tal quando o relogio chegar em 'at'". Entradas de conexoes que ja fecharam
// ficam na fila ate o instante chegar e sao ignoradas (custo: memoria por no maximo o maior prazo, ~30 s).
struct TimerEntry {
    Connection::Clock::time_point at;
    int fd;

    bool operator>(const TimerEntry& other) const {
        return at > other.at;
    }
};

using TimerQueue = std::priority_queue<TimerEntry, std::vector<TimerEntry>, std::greater<>>;

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

// Os prazos que o Server permite ajustar, lidos a cada cliente novo (por isso referencias a atomicos).
struct TimeoutSettings {
    const std::atomic<std::chrono::milliseconds::rep>& read;
    const std::atomic<std::chrono::milliseconds::rep>& idle;
    const std::atomic<std::chrono::milliseconds::rep>& linger;
    const std::atomic<std::size_t>& max_requests; // nao e prazo, mas e lido no mesmo momento: a cada cliente novo
};

// O event loop em si: um objeto por chamada a run()/serve_one(), com o estado que existe enquanto ele
// roda (Poller, conexoes, se ainda aceita, se esta encerrando). Cada metodo cuida de uma coisa: aceitar,
// tratar um evento, varrer prazos, encerrar.
class EventLoop {
public:
    using Clock = Connection::Clock;

    /// @param single_client aceita um cliente so e termina quando a conexao dele acabar (serve_one)
    /// @param timeouts lidos a cada cliente novo: o Server pode muda-los durante run()
    EventLoop(Listener& listener, bool single_client, std::size_t max_connections, Connection::Handler handler,
              TimeoutSettings timeouts, const std::atomic<bool>& stop)
        : m_listener(listener), m_single_client(single_client), m_max_connections(max_connections),
          m_handler(std::move(handler)), m_timeouts(timeouts), m_stop(stop) {
        if (!m_poller.add(m_listener.fd(), Interest::READ)) {
            throw std::runtime_error("Falha ao registrar o socket de escuta no Poller.");
        }
    }

    void run() {
        while (true) {
            if (m_stop.load() && !m_draining) begin_draining();
            // sem aceitar mais (parada, ou o unico cliente de serve_one) e sem conexoes: acabou
            if (!m_accepting && m_connections.empty()) return;

            resume_accepting_if_due(Clock::now());
            for (const PollEvent& event : m_poller.wait(wait_timeout(Clock::now()))) handle_event(event, Clock::now());
            run_due_timers(Clock::now());
        }
    }

private:
    // Aplica @p fn a cada conexao e descarta as que fecharam (ou nao da mais para vigiar).
    // Depois de mexer numa conexao: acompanha o interesse dela no Poller e, se o prazo dela adiantou, agenda o novo.
    // @return false se ela fechou (ou nao da mais para vigiar) e deve ser descartada.
    bool settle(Entry& entry) {
        if (!sync_interest(m_poller, entry)) return false;
        if (const auto deadline = entry.conn->deadline(); deadline && *deadline < entry.scheduled) {
            m_timers.push({*deadline, entry.conn->fd()});
            entry.scheduled = *deadline;
        }
        return true;
    }

    // Quanto o poll pode esperar: ate o proximo prazo (ou a retomada do accept), no maximo kLoopTick para notar
    // request_stop() (que so grava um atomic e nao acorda o Poller). Arredonda para cima: esperar 0 ms por um prazo
    // a 0,3 ms de distancia viraria um laco de polls vazios.
    std::chrono::milliseconds wait_timeout(Clock::time_point now) const {
        Clock::time_point wake = now + kLoopTick;
        if (!m_timers.empty()) wake = std::min(wake, m_timers.top().at);
        if (m_accepting && !m_listener_watched) wake = std::min(wake, m_accept_resume_at);
        if (wake <= now) return std::chrono::milliseconds::zero();
        return std::chrono::ceil<std::chrono::milliseconds>(wake - now);
    }

    template <typename Fn> void update_all(Fn&& fn) {
        for (auto it = m_connections.begin(); it != m_connections.end();) {
            fn(*it->second.conn);
            it = settle(it->second) ? std::next(it) : m_connections.erase(it);
        }
    }

    void stop_accepting() {
        if (m_listener_watched) {
            (void)m_poller.remove(m_listener.fd());
            m_listener_watched = false;
        }
        m_accepting = false;
    }

    // O accept falhou por falta de recursos: para de vigiar o socket de escuta por um tempo (ver kMinAcceptBackoff).
    // As conexoes abertas seguem sendo atendidas normalmente.
    void pause_accepting(Clock::time_point now) {
        if (m_listener_watched) {
            (void)m_poller.remove(m_listener.fd());
            m_listener_watched = false;
        }
        m_accept_backoff =
            m_accept_backoff.count() == 0 ? kMinAcceptBackoff : std::min(m_accept_backoff * 2, kMaxAcceptBackoff);
        m_accept_resume_at = now + m_accept_backoff;
        log_error(std::format("Falha ao aceitar conexao; nova tentativa em {} ms.", m_accept_backoff.count()));
    }

    void resume_accepting_if_due(Clock::time_point now) {
        if (!m_accepting || m_listener_watched || now < m_accept_resume_at) return;
        if (m_poller.add(m_listener.fd(), Interest::READ)) {
            m_listener_watched = true;
        } else {
            m_accept_resume_at = now + m_accept_backoff; // nao deu para voltar a vigiar: tenta de novo adiante
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
            m_accept_backoff =
                std::chrono::milliseconds::zero(); // voltou a funcionar: a proxima falha recomeca do minimo
            break;
        case AcceptStatus::RETRY:
            return true; // sinal, ou o cliente desistiu: tenta o proximo
        case AcceptStatus::EMPTY:
            return false; // fila de espera vazia
        case AcceptStatus::EXHAUSTED:
        case AcceptStatus::FAILED:
            pause_accepting(now);
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
        options.read_timeout = std::chrono::milliseconds(m_timeouts.read.load());
        options.idle_timeout = std::chrono::milliseconds(m_timeouts.idle.load());
        options.linger_timeout = std::chrono::milliseconds(m_timeouts.linger.load());
        options.max_requests = m_timeouts.max_requests.load();
        auto conn = std::make_unique<Connection>(std::move(client), m_handler, now, options);
        if (conn->closed()) return;

        const int conn_fd = conn->fd();
        if (!m_poller.add(conn_fd, Interest::READ)) {
            log_warn("Falha ao registrar o cliente no Poller.");
            return; // o destrutor de 'conn' fecha o fd
        }
        const auto emplaced = m_connections.emplace(conn_fd, Entry{std::move(conn), Interest::READ});
        (void)settle(emplaced.first->second);  // agenda o primeiro prazo (a primeira requisicao tem read_timeout)
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
        if (!settle(it->second)) m_connections.erase(it);
    }

    // Dispara os prazos vencidos. Cada conexao tem no maximo uma entrada "viva" (Entry::scheduled); ao vencer, a
    // conexao confere o prazo REAL dela (check_deadline nao faz nada se ele ja foi adiado) e settle() reagenda. Assim o
    // custo e de ~1 operacao de heap por conexao a cada periodo de timeout, e nao uma por requisicao nem uma varredura
    // de todas.
    void run_due_timers(Clock::time_point now) {
        while (!m_timers.empty() && m_timers.top().at <= now) {
            const TimerEntry due = m_timers.top();
            m_timers.pop();
            const auto it = m_connections.find(due.fd);
            if (it == m_connections.end() || it->second.scheduled != due.at)
                continue; // fechou, ou ha entrada mais nova
            it->second.scheduled = Clock::time_point::max();
            it->second.conn->check_deadline(now);
            if (!settle(it->second)) m_connections.erase(it);
        }
    }

    Poller m_poller;
    Listener& m_listener;
    bool m_single_client;
    std::size_t m_max_connections;
    Connection::Handler m_handler;
    TimeoutSettings m_timeouts;
    const std::atomic<bool>& m_stop;
    ConnectionMap m_connections;
    bool m_accepting = true;
    bool m_listener_watched = true; ///< o socket de escuta esta no Poller (fica false durante o backoff)
    std::chrono::milliseconds m_accept_backoff{0};
    Clock::time_point m_accept_resume_at;
    bool m_draining = false;
    TimerQueue m_timers;
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
    m_router.set_fallback(
        [root = StaticRoot{m_root}](const HttpRequest& request) { return serve_static(root, request); });
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

void Server::set_max_requests_per_connection(std::size_t max_requests) noexcept {
    if (max_requests > 0) {
        m_max_requests.store(max_requests);
    }
}

void Server::set_linger_timeout(std::chrono::milliseconds timeout) noexcept {
    if (timeout > std::chrono::milliseconds::zero()) {
        m_linger_timeout_ms.store(timeout.count());
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
                   TimeoutSettings{m_read_timeout_ms, m_idle_timeout_ms, m_linger_timeout_ms, m_max_requests},
                   m_stop};
    loop.run();
}

} // namespace citius
