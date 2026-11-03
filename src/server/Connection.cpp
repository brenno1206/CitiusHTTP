#include "citius/server/Connection.hpp"
#include "citius/http/HttpRequest.hpp"
#include "citius/http/HttpResponse.hpp"
#include "citius/http/StatusCode.hpp"
#include "citius/net/Socket.hpp"
#include "citius/parse/RequestAssembler.hpp"
#include "citius/util/Logger.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <optional>
#include <sys/types.h> // IWYU pragma: keep (ssize_t, off_t)
#include <unistd.h>    // pread()
#include <utility>

namespace citius {

namespace {

// Acima disso o buffer de saida e devolvido ao SO depois de cada resposta: uma conexao keep-alive
// parada nao deve segurar o buffer de um download grande (o pedaco de arquivo, abaixo, e maior que isto)
constexpr std::size_t kKeepOutputCapacity = 16 * 1024;

// Quanto de um arquivo e lido por vez para o buffer de saida: memoria constante por conexao, qualquer que seja o
// arquivo
constexpr std::size_t kFileChunk = 64 * 1024;

constexpr std::size_t kReadChunk = 16 * 1024;

bool would_block() {
    return errno == EAGAIN || errno == EWOULDBLOCK;
}

HttpResponse close_with(StatusCode status) {
    HttpResponse response = HttpResponse::make_error(status);
    (void)response.set_header("Connection", "close");
    return response;
}

} // namespace

Connection::Connection(Socket socket, Handler handler, Clock::time_point now, ConnectionOptions options)
    : m_socket(std::move(socket)), m_handler(std::move(handler)), m_options(options),
      m_assembler(options.max_head_bytes), m_deadline(now + options.read_timeout) {
    if (!m_socket.valid()) {
        m_state = State::CLOSED;
        return;
    }
    // SO_NOSIGPIPE e por socket (macOS): precisa ser definido em cada cliente aceito
    if (!m_socket.set_nonblocking() || !m_socket.set_no_sigpipe()) {
        log_warn("Falha ao configurar o socket do cliente.");
        close();
    }
}

std::optional<Connection::Clock::time_point> Connection::deadline() const noexcept {
    if (closed()) return std::nullopt;
    return m_deadline;
}

void Connection::close() noexcept {
    m_socket.reset();
    m_state = State::CLOSED;
    m_out = std::string{};
    m_file.reset(); // fecha o arquivo de uma resposta interrompida
}

void Connection::on_readable(Clock::time_point now) {
    if (m_state == State::LINGERING) {
        discard_input();
        return;
    }
    if (m_state != State::READING) return;

    std::array<char, kReadChunk> buffer;
    const ssize_t received = m_socket.recv_some(buffer);
    if (received < 0) {
        if (would_block()) return; // nada ainda (evento atrasado): espera o proximo aviso
        log_debug("(erro de leitura)");
        close();
        return;
    }
    if (received == 0) {
        // fim normal de uma conexao keep-alive: o cliente terminou e fechou
        if (m_assembler.has_buffered_data()) log_debug("(conexao encerrada antes de completar a requisicao)");
        close();
        return;
    }

    // o relogio da requisicao comeca no primeiro byte dela; a primeira conta desde a conexao
    if (m_served > 0 && !m_assembler.has_buffered_data()) {
        m_deadline = now + m_options.read_timeout;
    }
    drive(now, std::string_view{buffer.data(), static_cast<std::size_t>(received)});
}

void Connection::drive(Clock::time_point now, std::string_view bytes) {
    AssembleStatus status = m_assembler.feed(bytes);

    while (m_state == State::READING) {
        if (status == AssembleStatus::NEED_MORE) {
            // o cliente pediu "Expect: 100-continue" e espera o aval antes de enviar o corpo
            if (m_assembler.take_continue()) send_interim_continue();
            return;
        }

        if (status == AssembleStatus::ERROR) {
            // depois de um erro nao se sabe onde a proxima requisicao comeca: responde e fecha
            const StatusCode error = m_assembler.error_status();
            if (error == StatusCode::HEADERS_TOO_LARGE) {
                log_info("(cabecalhos grandes demais) -> 431");
            } else {
                log_info(std::format("(requisicao invalida) -> {}", static_cast<int>(error)));
            }
            begin_response(close_with(error), false, now);
            return;
        }

        const HttpRequest request = m_assembler.take_request();
        const bool last = m_served + 1 >= m_options.max_requests || m_draining;
        ++m_served;
        const bool keep_alive = !last && request.keep_alive();

        HttpResponse response = respond_safely(request);
        // a RFC pede que o servidor avise quando vai fechar; "keep-alive" explicito tambem serve ao HTTP/1.0
        (void)response.set_header("Connection", keep_alive ? "keep-alive" : "close");
        log_debug(
            std::format("{} {} -> {}", method_name(request.method), request.path, static_cast<int>(response.status())));

        // se a resposta nao couber de uma vez, o estado vira WRITING e o laco para: a proxima
        // requisicao (ja guardada, se encadeada) espera a resposta sair
        begin_response(response, keep_alive, now);
        status = m_assembler.feed({});
    }
}

HttpResponse Connection::respond_safely(const HttpRequest& request) {
    // Um handler com bug nao pode derrubar o servidor nem deixar o cliente sem resposta.
    try {
        return m_handler(request);
    } catch (const std::exception& e) {
        log_error(std::format("Handler lancou excecao: {}", e.what()));
    } catch (...) {
        log_error("Handler lancou excecao desconhecida.");
    }
    return HttpResponse::make_error(StatusCode::INTERNAL_SERVER_ERROR);
}

void Connection::begin_response(const HttpResponse& response, bool keep_alive, Clock::time_point now) {
    m_out = response.serialize();
    m_out_pos = 0;
    m_file.reset();
    m_file_offset = 0;
    m_file_remaining = 0;
    if (const auto& body = response.file_body()) {
        m_file = body->file;
        m_file_remaining = body->size;
    }
    m_close_after_write = !keep_alive;
    m_state = State::WRITING;
    // prazo base + o tempo que o cliente levaria lendo na velocidade minima (ver kMinWriteBytesPerSecond)
    const std::uint64_t total_bytes = m_out.size() + m_file_remaining;
    m_deadline =
        now + m_options.write_timeout + std::chrono::milliseconds(total_bytes * 1000 / kMinWriteBytesPerSecond);
    if (flush()) finish_write(now);
}

bool Connection::refill_from_file() {
    const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(kFileChunk, m_file_remaining));
    m_out.resize(want);
    m_out_pos = 0;
    ssize_t n = 0;
    do {
        n = ::pread(m_file->get(), m_out.data(), want, static_cast<off_t>(m_file_offset));
    } while (n < 0 && errno == EINTR);
    if (n <= 0) {
        // O Content-Length ja foi enviado: o arquivo encolheu (ou o disco falhou) e nao da para cumprir a promessa.
        // Fechar e o unico jeito de o cliente saber que a resposta esta incompleta.
        log_warn("(arquivo encolheu ou falhou durante o envio: conexao encerrada)");
        close();
        return false;
    }
    m_out.resize(static_cast<std::size_t>(n));
    m_file_offset += static_cast<std::uint64_t>(n);
    m_file_remaining -= static_cast<std::uint64_t>(n);
    return true;
}

bool Connection::flush() {
    for (;;) {
        while (m_out_pos < m_out.size()) {
            const ssize_t sent = m_socket.send_some(std::string_view{m_out}.substr(m_out_pos));
            if (sent < 0) {
                if (errno == EINTR) continue;
                if (!would_block()) {
                    log_debug("Falha ao enviar resposta.");
                    close();
                }
                return false;
            }
            if (sent == 0) return false; // nao deveria ocorrer com dados a enviar; o proximo aviso tenta de novo
            m_out_pos += static_cast<std::size_t>(sent);
        }
        // o buffer esvaziou: se a resposta tem um arquivo por enviar, o proximo pedaco vem dele
        if (m_file_remaining == 0) break;
        if (!refill_from_file()) return false;
    }
    m_file.reset();
    if (m_out.capacity() > kKeepOutputCapacity) {
        m_out = std::string{};
    } else {
        m_out.clear();
    }
    m_out_pos = 0;
    return true;
}

void Connection::finish_write(Clock::time_point now) {
    const bool buffered = m_assembler.has_buffered_data();
    if (m_close_after_write) {
        begin_lingering(now);
        return;
    }
    if (m_draining && !buffered) {
        close(); // parada do servidor e nada mais a responder: nao ha o que proteger
        return;
    }
    m_state = State::READING;
    m_deadline = now + (buffered ? m_options.read_timeout : m_options.idle_timeout);
}

void Connection::send_interim_continue() noexcept {
    // Resposta interina (1xx): nao tem corpo nem Content-Length, entao nao passa pelo HttpResponse. Uma tentativa so:
    // sao 25 bytes num buffer de envio vazio (a conexao esta em READING, sem nada pendente). Se falhar, o cliente
    // espera ~1 s pelo 100 e envia o corpo mesmo assim (RFC 9110 10.1.1).
    constexpr std::string_view kContinue = "HTTP/1.1 100 Continue\r\n\r\n";
    (void)m_socket.send_some(kContinue);
}

void Connection::begin_lingering(Clock::time_point now) {
    // O FIN vai depois da resposta (o SO envia o que esta na fila antes). Se o shutdown falhar (o cliente ja foi
    // embora, ENOTCONN), nao ha o que proteger: fecha.
    if (!m_socket.shutdown_write()) {
        close();
        return;
    }
    m_state = State::LINGERING;
    m_deadline = now + m_options.linger_timeout;
    m_discarded = 0;
}

void Connection::discard_input() {
    std::array<char, kReadChunk> buffer;
    const ssize_t received = m_socket.recv_some(buffer);
    if (received < 0) {
        if (!would_block()) close();
        return;
    }
    // 0 = o cliente fechou o lado dele: agora sim podemos fechar sem risco de RST
    if (received == 0) {
        close();
        return;
    }
    m_discarded += static_cast<std::size_t>(received);
    if (m_discarded >= kMaxLingerBytes) {
        log_debug("(cliente nao para de enviar durante o fechamento)");
        close();
    }
}

void Connection::on_writable(Clock::time_point now) {
    if (m_state != State::WRITING) return;
    if (flush()) {
        finish_write(now);
        if (m_state == State::READING) drive(now, {}); // o cliente pode ter encadeado requisicoes
    }
}

void Connection::check_deadline(Clock::time_point now) {
    if (closed() || now < m_deadline) return;

    if (m_state == State::WRITING) {
        log_debug("(cliente nao leu a resposta a tempo)");
        close();
        return;
    }
    if (m_state == State::LINGERING) {
        close(); // o cliente nao fechou a tempo: ja demos a ele a chance de ler a resposta
        return;
    }

    // Primeira requisicao que nao veio, ou requisicao pela metade: ha a quem responder. Entre
    // requisicoes, ficar calado e normal: nao ha requisicao a responder, so fechamos.
    if (m_served == 0 || m_assembler.has_buffered_data()) {
        log_info("(cliente parado) -> 408");
        const std::string response = close_with(StatusCode::REQUEST_TIMEOUT).serialize();
        (void)m_socket.send_some(response); // uma tentativa so: quem esta parado talvez nem receba
        begin_lingering(now);
        return;
    }
    close();
}

void Connection::close_when_idle() {
    if (closed()) return;
    m_draining = true;
    // Ociosa (nada a ler nem a responder) ou em lingering (so espera o cliente fechar: na parada nao vale esperar)
    const bool idle = m_state == State::READING && !m_assembler.has_buffered_data();
    if (idle || m_state == State::LINGERING) close();
}

} // namespace citius
