#pragma once
#include <chrono>
#include <span>
#include <string_view>
#include <sys/types.h> // ssize_t

namespace citius {

/// Dono unico de um file descriptor de socket (RAII): fecha o fd no destrutor.
///
/// Move-only: a copia e deletada para que dois objetos nunca fechem o mesmo fd.
/// Os metodos de I/O sao bloqueantes e nao lancam excecoes; falhas sao devolvidas no retorno.
class Socket {
public:
    /// Cria um Socket sem fd (valid() == false).
    Socket() noexcept = default;

    /// Assume a posse de @p fd (que sera fechado no destrutor). Use -1 para "sem fd".
    explicit Socket(int fd) noexcept : m_fd(fd) {}

    ~Socket();

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    /// Transfere a posse do fd de @p other, que fica invalido.
    Socket(Socket&& other) noexcept;

    /// Fecha o fd atual e transfere a posse do fd de @p other, que fica invalido.
    Socket& operator=(Socket&& other) noexcept;

    /// @return o fd cru (para chamadas POSIX). Nao feche nem guarde alem da vida do Socket.
    [[nodiscard]] int get() const noexcept { return m_fd; }

    /// @return true se ha um fd aberto sob posse deste objeto.
    [[nodiscard]] bool valid() const noexcept { return m_fd >= 0; }

    /// Abre mao da posse sem fechar o fd.
    /// @return o fd; quem o receber passa a ser responsavel por fecha-lo.
    [[nodiscard]] int release() noexcept;

    /// Fecha o fd atual (se houver) e passa a guardar @p new_fd.
    /// Sem argumento, apenas fecha e deixa o Socket invalido.
    void reset(int new_fd = -1) noexcept;

    /// Liga SO_REUSEADDR: permite reusar a porta logo apos reiniciar o servidor
    /// (evita "Address already in use"). Deve ser chamado antes do bind().
    /// @return false se a opcao nao pode ser definida.
    [[nodiscard]] bool set_reuse_addr() noexcept;

    /// Impede que send() em conexao fechada mate o processo com SIGPIPE.
    /// So tem efeito no macOS (SO_NOSIGPIPE, por socket); no Linux a protecao e a flag
    /// MSG_NOSIGNAL, ja usada em send_all(). Chamar em cada socket de cliente aceito.
    /// @return false se a opcao nao pode ser definida.
    [[nodiscard]] bool set_no_sigpipe() noexcept;

    /// Define quanto tempo um recv() pode esperar dados (SO_RCVTIMEO). Passado o prazo, recv_some()
    /// devolve -1 com errno == EAGAIN/EWOULDBLOCK. O prazo vale para cada recv, nao para a
    /// requisicao inteira. Prazo zero ou negativo e recusado (zero significaria "sem limite").
    /// @return false se a opcao nao pode ser definida.
    [[nodiscard]] bool set_recv_timeout(std::chrono::milliseconds timeout) noexcept;

    /// Envia todos os bytes de @p data, repetindo send() em envios parciais e em EINTR.
    /// @return false se a conexao falhou (ex.: o cliente fechou) antes de enviar tudo.
    [[nodiscard]] bool send_all(std::string_view data) noexcept;

    /// Le ate buffer.size() bytes com um unico recv() (repete apenas em EINTR).
    /// Pode devolver menos do que o pedido: o TCP nao preserva fronteiras de mensagem.
    /// @return bytes lidos; 0 se o par fechou a conexao; -1 em erro (errno == EAGAIN/EWOULDBLOCK se
    ///         o prazo de set_recv_timeout() estourou).
    [[nodiscard]] ssize_t recv_some(std::span<char> buffer) noexcept;

private:
    int m_fd = -1;
};

} // namespace citius
