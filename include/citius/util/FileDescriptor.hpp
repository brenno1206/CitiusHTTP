#pragma once

namespace citius {

/// Dono unico de um file descriptor do SO (RAII): fecha o fd no destrutor. Serve para arquivo, socket, kqueue/epoll...
///
/// Move-only: a copia e deletada para que dois objetos nunca fechem o mesmo fd. Nao conhece nada de rede (por isso fica
/// em util/): o Socket o compoe e acrescenta as operacoes de socket; os arquivos estaticos o usam direto.
class FileDescriptor {
public:
    /// Cria um FileDescriptor sem fd (valid() == false).
    FileDescriptor() noexcept = default;

    /// Assume a posse de @p fd (que sera fechado no destrutor). Use -1 para "sem fd".
    explicit FileDescriptor(int fd) noexcept : m_fd(fd) {}

    ~FileDescriptor();

    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;

    /// Transfere a posse do fd de @p other, que fica invalido.
    FileDescriptor(FileDescriptor&& other) noexcept;

    /// Fecha o fd atual e transfere a posse do fd de @p other, que fica invalido.
    FileDescriptor& operator=(FileDescriptor&& other) noexcept;

    /// @return o fd cru (para chamadas POSIX). Nao feche nem guarde alem da vida do FileDescriptor.
    [[nodiscard]] int get() const noexcept {
        return m_fd;
    }

    /// @return true se ha um fd aberto sob posse deste objeto.
    [[nodiscard]] bool valid() const noexcept {
        return m_fd >= 0;
    }

    /// Abre mao da posse sem fechar o fd.
    /// @return o fd; quem o receber passa a ser responsavel por fecha-lo.
    [[nodiscard]] int release() noexcept;

    /// Fecha o fd atual (se houver) e passa a guardar @p new_fd. Sem argumento, apenas fecha.
    void reset(int new_fd = -1) noexcept;

private:
    int m_fd = -1;
};

} // namespace citius
