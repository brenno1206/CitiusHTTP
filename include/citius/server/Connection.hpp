#pragma once
#include "citius/http/HttpRequest.hpp"
#include "citius/http/HttpResponse.hpp"
#include "citius/net/Poller.hpp"
#include "citius/net/Socket.hpp"
#include "citius/parse/RequestAssembler.hpp"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace citius {

/// Quanto tempo uma requisicao tem para chegar inteira (padrao). Vale para a requisicao toda,
/// nao para cada leitura: contar so cada leitura deixaria um cliente que manda 1 byte de vez em
/// quando (slowloris) prender a conexao para sempre.
inline constexpr std::chrono::milliseconds kDefaultReadTimeout{10000};

/// Quanto tempo uma conexao keep-alive pode ficar parada esperando a proxima requisicao (padrao).
inline constexpr std::chrono::milliseconds kDefaultIdleTimeout{5000};

/// Quanto tempo o cliente tem para receber uma resposta inteira (padrao). Evita que quem nao le
/// prenda a conexao e a memoria da resposta.
inline constexpr std::chrono::milliseconds kDefaultWriteTimeout{30000};

/// Quanto tempo uma conexao que vai fechar espera o cliente fechar o lado dele (padrao). Ver Connection (lingering
/// close).
inline constexpr std::chrono::milliseconds kDefaultLingerTimeout{2000};

/// Quantos bytes uma conexao em lingering descarta antes de desistir e fechar (um cliente que nao para de enviar).
inline constexpr std::size_t kMaxLingerBytes = 1024 * 1024;

/// Quantas requisicoes uma conexao atende antes de o servidor fecha-la (evita conexao eterna; padrao). Nao e so
/// higiene: com milhares de clientes que comecam juntos, todos chegam ao limite quase ao mesmo tempo e reconectam numa
/// rajada que estoura a fila de escuta do kernel (128 no macOS), e o cliente leva RST. Com limite 100 e 3000
/// conexoes o wrk viu ~1.000-2.000 erros de leitura; com 1000, nenhum (docs/benchmarks.md). Configuravel por
/// Server::set_max_requests_per_connection e --max-requests.
inline constexpr std::size_t kMaxRequestsPerConnection = 1000;

/// Velocidade minima de leitura que se espera de um cliente (bytes por segundo). O prazo para uma resposta sair
/// e write_timeout MAIS o tempo que o cliente levaria nessa velocidade: uma resposta pequena segue o prazo base, e
/// um arquivo de centenas de MB nao e cortado por ser grande, mas quem le mais devagar que isso (slow read) e
/// considerado parado e desconectado.
inline constexpr std::size_t kMinWriteBytesPerSecond = 16 * 1024;

/// Ajustes de uma Connection.
struct ConnectionOptions {
    std::chrono::milliseconds read_timeout =
        kDefaultReadTimeout; ///< prazo para a requisicao chegar inteira; estourou, 408
    std::chrono::milliseconds idle_timeout =
        kDefaultIdleTimeout; ///< prazo entre requisicoes; estourou, fecha em silencio
    std::chrono::milliseconds write_timeout =
        kDefaultWriteTimeout; ///< prazo para a resposta sair inteira; estourou, fecha
    std::chrono::milliseconds linger_timeout = kDefaultLingerTimeout;    ///< prazo do lingering close; estourou, fecha
    std::size_t max_requests = kMaxRequestsPerConnection;                ///< requisicoes por conexao
    std::size_t max_head_bytes = RequestAssembler::kDefaultMaxHeadBytes; ///< teto da cabeca; acima, 431
};

/// Uma conexao de cliente num event loop: tudo o que o Server antes fazia bloqueando, numa thread
/// por conexao, agora como maquina de estados que avanca quando o loop avisa que algo aconteceu.
///
///   READING --(requisicao completa)--> WRITING --(resposta enviada)--> READING (keep-alive)
///                                                                  \-> LINGERING -> CLOSED (close, erro, limite)
///
/// LINGERING (RFC 9112 secao 9.6): depois da ultima resposta a conexao envia o FIN (shutdown de escrita) e continua
/// lendo e descartando ate o cliente fechar, ate kDefaultLingerTimeout ou ate kMaxLingerBytes. Fechar de uma vez
/// com dados do cliente ainda por ler faz o SO mandar um RST, que pode apagar a resposta (o 400/431/503, por exemplo)
/// antes de o cliente le-la.
///
///                                                                  \-> CLOSED (close, erro, limite)
///
/// O loop (Poller) chama: on_readable() quando ha dados, on_writable() quando cabe escrever
/// (interest() diz qual dos dois ele deve vigiar), e check_deadline() quando deadline() passar.
/// Nada aqui bloqueia: o socket e nao bloqueante, uma resposta que nao cabe de uma vez fica num
/// buffer e sai aos poucos, e enquanto ela nao sai a proxima requisicao nao e lida nem processada
/// (contrapressao: um cliente que manda muitas requisicoes e nao le as respostas nao enche a memoria).
///
/// O tempo e passado como parametro (@c now) em vez de lido do relogio, para os testes poderem
/// simular minutos sem esperar. Quando closed() vira true o fd ja foi fechado (e o SO o tira
/// do Poller sozinho): o loop so precisa descartar o objeto. Move-only por conter um Socket.
/// Nao e thread-safe: uma conexao pertence a uma thread so.
class Connection {
public:
    using Clock = std::chrono::steady_clock;

    /// Gera a resposta de uma requisicao. Se lancar excecao, a conexao responde 500 (sem vazar a mensagem).
    using Handler = std::function<HttpResponse(const HttpRequest&)>;

    /// Assume @p socket, que passa a ser nao bloqueante. Se ele for invalido, ou se o SO recusar as
    /// opcoes, a conexao ja nasce fechada (closed() == true).
    /// @param now instante atual; a primeira requisicao tem read_timeout a partir dele.
    Connection(Socket socket, Handler handler, Clock::time_point now, ConnectionOptions options = {});

    /// @return o fd, para registrar no Poller (-1 depois de fechada).
    [[nodiscard]] int fd() const noexcept {
        return m_socket.get();
    }

    /// @return true se a conexao terminou e o fd ja foi fechado.
    [[nodiscard]] bool closed() const noexcept {
        return m_state == State::CLOSED;
    }

    /// @return true se ja enviou tudo, ja mandou o FIN e so espera o cliente fechar (descartando o que ele enviar).
    [[nodiscard]] bool lingering() const noexcept {
        return m_state == State::LINGERING;
    }

    /// @return o que o Poller deve vigiar: READ enquanto espera uma requisicao, WRITE enquanto
    ///         sobra resposta a enviar. Pode mudar depois de qualquer chamada abaixo.
    [[nodiscard]] Interest interest() const noexcept {
        return m_state == State::WRITING ? Interest::WRITE : Interest::READ;
    }

    /// @return quando check_deadline() deve ser chamado; std::nullopt se a conexao ja fechou.
    [[nodiscard]] std::optional<Clock::time_point> deadline() const noexcept;

    /// Le (uma vez) o que o cliente mandou e atende as requisicoes que ficarem completas.
    /// Chamar sem dados disponiveis ou fora do estado de leitura nao faz nada.
    void on_readable(Clock::time_point now);

    /// Continua enviando a resposta pendente; terminada, retoma a leitura (e atende requisicoes
    /// que ja estavam guardadas, se o cliente as encadeou).
    void on_writable(Clock::time_point now);

    /// Aplica o prazo se @p now ja passou de deadline(): requisicao incompleta (ou primeira que
    /// nunca veio) recebe 408 e fecha; conexao ociosa entre requisicoes fecha em silencio; cliente
    /// que nao le a resposta e desconectado. Antes do prazo, nao faz nada.
    void check_deadline(Clock::time_point now);

    /// Pede encerramento gracioso: fecha ja se estiver ociosa (nada a responder); se houver
    /// requisicao em andamento, deixa terminar e a resposta avisa "Connection: close".
    void close_when_idle();

private:
    enum class State { READING, WRITING, LINGERING, CLOSED };

    /// Alimenta o assembler com @p bytes e atende requisicoes ate faltar dado, ficar resposta pendente ou fechar.
    void drive(Clock::time_point now, std::string_view bytes);
    HttpResponse respond_safely(const HttpRequest& request);
    /// Guarda @p response no buffer e tenta envia-la; @p keep_alive false fecha a conexao depois.
    void begin_response(const HttpResponse& response, bool keep_alive, Clock::time_point now);
    /// @return true se o buffer de saida esvaziou; false se sobrou (EAGAIN) ou a conexao fechou por erro.
    bool flush();
    void finish_write(Clock::time_point now);
    /// Manda o FIN e passa a esperar o cliente fechar; se nao der, fecha.
    void begin_lingering(Clock::time_point now);
    /// Le o proximo pedaco do arquivo da resposta para m_out. @return false se nao deu (o arquivo encolheu ou a leitura
    /// falhou): a conexao foi fechada, porque nao se cumpre mais o Content-Length ja enviado.
    bool refill_from_file();
    /// Envia "HTTP/1.1 100 Continue" (resposta interina) quando o cliente pediu Expect: 100-continue.
    void send_interim_continue() noexcept;
    void discard_input();
    void close() noexcept;

    Socket m_socket;
    Handler m_handler;
    ConnectionOptions m_options;
    RequestAssembler m_assembler;
    State m_state = State::READING;
    std::string m_out;                            ///< resposta serializada, ainda nao enviada por inteiro
    std::shared_ptr<const FileDescriptor> m_file; ///< arquivo que e o corpo da resposta atual (streaming), ou nulo
    std::uint64_t m_file_offset = 0;              ///< proxima posicao a ler do arquivo
    std::uint64_t m_file_remaining = 0;           ///< bytes do arquivo ainda por enviar
    std::size_t m_out_pos = 0;                    ///< quantos bytes de m_out ja saíram
    bool m_close_after_write = false;
    bool m_draining = false;     ///< close_when_idle() foi chamado
    std::size_t m_discarded = 0; ///< bytes descartados em LINGERING
    std::size_t m_served = 0;    ///< requisicoes ja atendidas nesta conexao
    Clock::time_point m_deadline;
};

} // namespace citius
