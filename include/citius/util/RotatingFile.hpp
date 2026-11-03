#pragma once
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string_view>

namespace citius {

/// Arquivo de texto (de log) que nao cresce para sempre: quando a proxima escrita estouraria @c max_bytes, o arquivo
/// corrente "app.log" vira "app.log.1", o "app.log.1" vira "app.log.2" e assim por diante; o que passar de @c keep
/// arquivos antigos e apagado. Uma linha nunca e partida ao meio: a rotacao acontece antes de escrever a que nao cabe.
///
/// Nao e thread-safe (quem o usa serializa, como o Logger faz). Erros de disco em tempo de execucao NAO lancam
/// excecao: write() devolve false e o chamador decide (o Logger cai para o stderr), porque um log nao pode derrubar o
/// servidor. Move-only por conter um fluxo de arquivo.
class RotatingFile {
public:
    /// Abre (ou cria) @p path para acrescentar. O que ja estava no arquivo conta para o limite.
    /// @param max_bytes tamanho a partir do qual a proxima escrita rotaciona.
    /// @param keep quantos arquivos antigos guardar (".1" ... ".keep"); 0 = nao guardar nenhum, so recomecar.
    /// @throws std::runtime_error se o arquivo nao puder ser aberto (pasta inexistente, sem permissao): falha de
    ///         inicializacao, no mesmo estilo de "porta ocupada".
    RotatingFile(std::filesystem::path path, std::size_t max_bytes, std::size_t keep);

    /// Acrescenta @p text (ja com o "\n" final) e descarrega o buffer, rotacionando antes se for preciso.
    /// @return false se nao deu para escrever (disco cheio, arquivo removido...); o texto nao foi gravado.
    [[nodiscard]] bool write(std::string_view text);

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return m_path;
    }
    [[nodiscard]] std::size_t max_bytes() const noexcept {
        return m_max_bytes;
    }
    [[nodiscard]] std::size_t keep() const noexcept {
        return m_keep;
    }

private:
    void rotate();
    [[nodiscard]] std::filesystem::path numbered(std::size_t n) const;

    std::filesystem::path m_path;
    std::size_t m_max_bytes;
    std::size_t m_keep;
    std::size_t m_size = 0; ///< bytes do arquivo corrente
    std::ofstream m_stream;
};

} // namespace citius
