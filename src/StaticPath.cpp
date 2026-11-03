#include "citius/StaticPath.hpp"
#include <algorithm>
#include <system_error>

namespace citius {

namespace {

// Valor de um digito hexadecimal, ou -1 se nao for.
int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

} // namespace

std::optional<std::string> decode_target_path(std::string_view target) {
    if (target.empty() || target.front() != '/') {
        return std::nullopt;
    }
    target = target.substr(0, target.find('?'));

    std::string decoded;
    decoded.reserve(target.size());
    for (std::size_t i = 0; i < target.size(); ++i) {
        char c = target[i];
        if (c == '%') {
            // precisa haver dois caracteres depois do '%'
            bool has_two = i + 2 < target.size();
            int high = has_two ? hex_value(target[i + 1]) : -1;
            int low = has_two ? hex_value(target[i + 2]) : -1;
            if (high < 0 || low < 0) {
                return std::nullopt;
            }
            c = static_cast<char>(high * 16 + low);
            i += 2;
        }
        // NUL truncaria o caminho em chamadas de SO escritas em C ("a\0b" abriria "a")
        if (c == '\0') {
            return std::nullopt;
        }
        decoded.push_back(c);
    }
    return decoded;
}

std::optional<std::filesystem::path> resolve_static_path(const std::filesystem::path& root,
                                                         std::string_view decoded_path) {
    namespace fs = std::filesystem;

    if (decoded_path.empty() || decoded_path.front() != '/' || decoded_path.find('\0') != std::string_view::npos) {
        return std::nullopt;
    }

    // Monta o caminho segmento a segmento. Nunca fazemos root / "/etc": operator/ com um
    // argumento absoluto SUBSTITUI o lado esquerdo, que seria a fuga perfeita.
    const bool wants_index = decoded_path.back() == '/';
    fs::path relative;
    while (!decoded_path.empty()) {
        std::size_t slash = decoded_path.find('/');
        std::string_view segment = decoded_path.substr(0, slash);
        decoded_path = slash == std::string_view::npos ? std::string_view{} : decoded_path.substr(slash + 1);

        if (segment.empty() || segment == ".") {
            continue;
        }
        if (segment == "..") {
            return std::nullopt;
        }
        relative /= fs::path(std::string(segment));
    }
    if (wants_index) {
        relative /= "index.html";
    }

    // error_code: weakly_canonical lancaria em caso de erro do SO (nome longo demais, ELOOP...);
    // para entrada de cliente isso e rotina, entao viramos "recusado"
    std::error_code ec;
    fs::path canonical_root = fs::weakly_canonical(root, ec);
    if (ec) {
        return std::nullopt;
    }
    // "www/" canonicaliza com um componente final vazio, que quebraria a comparacao abaixo
    if (!canonical_root.has_filename()) {
        canonical_root = canonical_root.parent_path();
    }
    // resolve symlinks dos trechos que existem; e aqui que um link para fora da raiz e desmascarado
    fs::path candidate = fs::weakly_canonical(canonical_root / relative, ec);
    if (ec) {
        return std::nullopt;
    }

    // dentro da raiz = todos os componentes da raiz sao prefixo do candidato.
    // Comparar componentes (e nao texto) evita que "www2" passe por "www".
    if (std::ranges::mismatch(canonical_root, candidate).in1 != canonical_root.end()) {
        return std::nullopt;
    }
    return candidate;
}

} // namespace citius
