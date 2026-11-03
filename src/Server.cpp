#include "citius/Server.hpp"
#include "citius/HttpResponse.hpp"
#include "citius/Logger.hpp"
#include "citius/Poller.hpp"
#include "citius/StaticFiles.hpp"
#include <algorithm>
#include <arpa/inet.h> // ntohs()
#include <cerrno>
#include <chrono>
#include <format>
#include <iterator>
#include <memory>
#include <netinet/in.h> // Para struct sockaddr_in, htons(), htonl()
#include <stdexcept>    // Para lançar std::runtime_error
#include <sys/socket.h> // Para socket(), bind(), listen()
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

} // namespace

Server::Server(std::uint16_t port, std::filesystem::path root, std::size_t max_connections)
    : m_root(std::move(root)), m_max_connections(max_connections) {
    if (max_connections == 0) {
        throw std::invalid_argument("Server: max_connections deve ser maior que zero.");
    }
    // captura a raiz por valor: o fallback nao pode depender do endereco do Server (que e movivel)
    m_router.set_fallback([root = m_root](const HttpRequest& request) { return serve_static(root, request); });
    start(port);
}

void Server::start(std::uint16_t port) {
    m_server_socket.reset(socket(AF_INET, SOCK_STREAM, 0));

    if (!m_server_socket.valid()) {
        throw std::runtime_error("Erro na criacao do socket");
    }

    // precisa vir antes do bind, senao reiniciar o servidor falha com "Address already in use"
    if (!m_server_socket.set_reuse_addr()) {
        throw std::runtime_error("Falha ao definir SO_REUSEADDR.");
    }

    sockaddr_in address{
        .sin_family = AF_INET,              // IPv4
        .sin_port = htons(port),            // htons: converte para a ordem de bytes da rede (big-endian)
        .sin_addr = {.s_addr = INADDR_ANY}, // aceita conexoes em qualquer interface da maquina
        .sin_zero = {} // preenchimento: o Linux o declara, e -Wmissing-field-initializers reclama se faltar
    };
    if (bind(m_server_socket.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0) {
        throw std::runtime_error("Erro no bind.");
    }

    // fila de conexoes aguardando accept(). Com 10, uma rajada de ~30 clientes perdia conexoes no macOS
    // (o cliente via a conexao cair sem resposta); SOMAXCONN pede o maximo que o SO permite
    if (listen(m_server_socket.get(), SOMAXCONN) < 0) {
        throw std::runtime_error("Falha no listen.");
    }

    // nao bloqueante: o aviso de "ha cliente" do Poller pode ser falso (o cliente desiste entre o aviso e
    // o accept), e um accept bloqueante nesse caso travaria o loop inteiro ate o proximo cliente
    if (!m_server_socket.set_nonblocking()) {
        throw std::runtime_error("Falha ao tornar o socket de escuta nao bloqueante.");
    }

    // com porta 0 so agora o sistema sabe qual porta escolheu
    sockaddr_in bound{};
    socklen_t bound_size = sizeof(bound);
    if (getsockname(m_server_socket.get(), reinterpret_cast<sockaddr*>(&bound), &bound_size) < 0) {
        throw std::runtime_error("Falha no getsockname.");
    }
    m_port = ntohs(bound.sin_port);

    log_info(std::format("Servidor escutando na porta {}...", m_port));
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
    using Clock = Connection::Clock;

    Poller poller;
    if (!poller.add(m_server_socket.get(), Interest::READ)) {
        throw std::runtime_error("Falha ao registrar o socket de escuta no Poller.");
    }

    ConnectionMap connections;
    bool accepting = true;
    bool draining = false;
    Clock::time_point next_sweep = Clock::now() + kLoopTick;
    const Connection::Handler handler = [this](const HttpRequest& request) { return m_router.route(request); };

    auto stop_accepting = [&] {
        if (accepting) {
            (void)poller.remove(m_server_socket.get());
            accepting = false;
        }
    };

    auto const accept_pending = [&](Clock::time_point now) {
        for (int i = 0; i < kAcceptBatch && accepting; ++i) {
            // nullptr, nullptr: por enquanto nao queremos saber o endereco do cliente
            const int fd = accept(m_server_socket.get(), nullptr, nullptr);
            if (fd < 0) {
                if (errno == EINTR || errno == ECONNABORTED) continue; // sinal, ou o cliente desistiu: tenta o proximo
                if (errno == EAGAIN || errno == EWOULDBLOCK) return;   // fila de espera vazia
                log_error("Falha ao aceitar conexao.");
                std::this_thread::sleep_for(kAcceptErrorPause);
                return;
            }

            Socket client{fd};
            if (connections.size() >= m_max_connections) {
                log_warn("(limite de conexoes) -> 503");
                reject_busy(std::move(client));
                continue;
            }

            ConnectionOptions options;
            options.read_timeout = std::chrono::milliseconds(m_read_timeout_ms.load());
            options.idle_timeout = std::chrono::milliseconds(m_idle_timeout_ms.load());
            auto conn = std::make_unique<Connection>(std::move(client), handler, now, options);
            if (conn->closed()) continue;

            const int conn_fd = conn->fd();
            if (!poller.add(conn_fd, Interest::READ)) {
                log_warn("Falha ao registrar o cliente no Poller.");
                continue; // o destrutor de 'conn' fecha o fd
            }
            connections.emplace(conn_fd, Entry{std::move(conn), Interest::READ});
            if (single_client) stop_accepting(); // serve_one atende um cliente so
        }
    };

    while (true) {
        if (m_stop.load() && !draining) {
            // encerramento gracioso: nao aceita mais; conexao ociosa fecha na hora, as demais terminam a requisicao em
            // curso
            draining = true;
            stop_accepting();
            if (!single_client) log_info("Encerrando: esperando os clientes em andamento...");
            for (auto it = connections.begin(); it != connections.end();) {
                it->second.conn->close_when_idle();
                it = sync_interest(poller, it->second) ? std::next(it) : connections.erase(it);
            }
        }
        // sem aceitar mais (parada, ou o unico cliente de serve_one) e sem conexoes: acabou
        if (!accepting && connections.empty()) break;

        for (const PollEvent& event : poller.wait(kLoopTick)) {
            const Clock::time_point now = Clock::now();

            if (event.fd == m_server_socket.get()) {
                if (accepting) accept_pending(now);
                continue;
            }
            auto const it = connections.find(event.fd);
            if (it == connections.end()) continue;

            Connection& conn = *it->second.conn;
            if (event.readable) conn.on_readable(now);
            // "closed" sem "writable" acontece quando o par some durante o envio: on_writable descobre pelo erro do
            // send
            if (!conn.closed() && (event.writable || event.closed)) conn.on_writable(now);
            if (!sync_interest(poller, it->second)) connections.erase(it);
        }

        // prazos: varredura periodica, nao a cada evento (com milhares de conexoes seria custo por evento)
        const Clock::time_point now = Clock::now();
        if (now >= next_sweep) {
            next_sweep = now + kLoopTick;
            for (auto it = connections.begin(); it != connections.end();) {
                it->second.conn->check_deadline(now);
                it = sync_interest(poller, it->second) ? std::next(it) : connections.erase(it);
            }
        }
    }
}

} // namespace citius
