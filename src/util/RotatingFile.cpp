#include "citius/util/RotatingFile.hpp"
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace citius {

RotatingFile::RotatingFile(std::filesystem::path path, std::size_t max_bytes, std::size_t keep)
    : m_path(std::move(path)), m_max_bytes(max_bytes), m_keep(keep) {
    std::error_code ec;
    const auto existing = std::filesystem::file_size(m_path, ec);
    if (!ec) m_size = static_cast<std::size_t>(existing);

    m_stream.open(m_path, std::ios::app | std::ios::binary);
    if (!m_stream) {
        throw std::runtime_error("Nao foi possivel abrir o arquivo de log: " + m_path.string());
    }
}

std::filesystem::path RotatingFile::numbered(std::size_t n) const {
    return {m_path.string() + "." + std::to_string(n)};
}

void RotatingFile::rotate() {
    namespace fs = std::filesystem;
    m_stream.close();
    std::error_code
        ec; // falhas aqui (arquivo que nao existe, permissao) nao devem derrubar o servidor: seguimos adiante

    if (m_keep == 0) {
        fs::remove(m_path, ec);
    } else {
        fs::remove(numbered(m_keep), ec); // o mais velho sai
        for (std::size_t i = m_keep - 1; i >= 1; --i) {
            fs::rename(numbered(i), numbered(i + 1), ec); // .1 -> .2, .2 -> .3 ... (do mais velho para o mais novo)
        }
        fs::rename(m_path, numbered(1), ec);
    }

    m_stream.clear();
    m_stream.open(m_path, std::ios::app | std::ios::binary);
    m_size = 0;
}

bool RotatingFile::write(std::string_view text) {
    // m_size > 0: uma linha maior que o limite e escrita sozinha num arquivo vazio, em vez de rotacionar sem parar
    if (m_size > 0 && m_size + text.size() > m_max_bytes) rotate();

    if (!m_stream.is_open()) { // a rotacao nao conseguiu reabrir (ou o disco falhou antes): tenta de novo
        m_stream.clear();
        m_stream.open(m_path, std::ios::app | std::ios::binary);
    }
    m_stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    m_stream.flush();
    if (!m_stream) {
        m_stream.clear(); // o proximo write tenta de novo
        return false;
    }
    m_size += text.size();
    return true;
}

} // namespace citius
