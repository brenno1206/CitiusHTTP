#pragma once
#include "citius/util/FileDescriptor.hpp"
#include <cstdint>
#include <filesystem>
#include <string_view>

namespace citius {

/// Resultado de open_static_file(): o que fazer com a requisicao.
enum class OpenStatus {
    OK,          ///< OpenedFile::file esta aberto
    NOT_FOUND,   ///< nao existe, nao e arquivo comum, ou esta fora da raiz (o mesmo codigo, para nao revelar o que
                 ///< existe)
    FORBIDDEN,   ///< existe mas nao pode ser lido
    UNAVAILABLE, ///< sem descritores ou memoria agora (EMFILE, ENFILE, ENOMEM): tente de novo mais tarde
    ERROR        ///< outra falha do sistema
};

struct OpenedFile {
    OpenStatus status = OpenStatus::NOT_FOUND;
    FileDescriptor file;        ///< so valido com OK; aberto para leitura
    std::uint64_t size = 0;     ///< tamanho do arquivo no momento da abertura (fstat do proprio descritor)
    std::filesystem::path path; ///< caminho canonico aberto (so com OK); serve para achar a extensao
};

/// Abre com seguranca o arquivo que o alvo da requisicao (ja decodificado, ver decode_target_path) designa dentro de
/// @p root. Duas defesas em sequencia:
///
///  1. resolve_static_path(): confere o caminho (".." recusado, symlinks resolvidos, dentro da raiz);
///  2. open_verified(): abre sem seguir um symlink no ultimo componente (O_NOFOLLOW) e confere DE NOVO, no descritor ja
///     aberto, que o arquivo esta dentro da raiz.
///
/// O passo 2 fecha a janela TOCTOU (time-of-check to time-of-use): entre conferir um caminho e abri-lo alguem poderia
/// trocar um diretorio ou arquivo por um symlink para fora de www/. O descritor, depois de aberto, nao muda de objeto.
[[nodiscard]] OpenedFile open_static_file(const std::filesystem::path& root, std::string_view decoded_path);

/// A segunda metade de open_static_file(), separada para poder ser testada com o estado "link trocado depois da
/// checagem". @p candidate e um caminho que ja passou por resolve_static_path(); @p canonical_root vem de
/// canonical_static_root().
///
/// Abre com O_NONBLOCK: um FIFO (pipe nomeado) dentro de www/ travaria o open() para sempre, e como o servidor atende
/// todos os clientes numa thread so, travaria todos. Recusa o que nao for arquivo comum (diretorio, FIFO, dispositivo).
[[nodiscard]] OpenedFile open_verified(const std::filesystem::path& candidate,
                                       const std::filesystem::path& canonical_root);

/// @return true se o arquivo que @p file realmente aponta esta dentro de @p canonical_root. Pergunta ao SO o caminho do
///         descritor (F_GETPATH no macOS, /proc/self/fd no Linux), entao vale para o objeto aberto, nao para o nome que
///         se usou para abri-lo. Compara por componentes ("www2" nao esta dentro de "www"). Falha (false) se o SO nao
///         souber dizer o caminho.
[[nodiscard]] bool fd_is_inside(const FileDescriptor& file, const std::filesystem::path& canonical_root);

} // namespace citius
