#include "citius/files/StaticOpen.hpp"
#include "citius/files/StaticPath.hpp"
#include "citius/util/FileDescriptor.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <climits> // IWYU pragma: keep (PATH_MAX)
#include <cstdint>
#include <fcntl.h>
#include <filesystem>
#include <optional>
#include <string> // IWYU pragma: keep (std::to_string, so no ramo do Linux)
#include <string_view>
#include <sys/stat.h>
#include <sys/types.h> // IWYU pragma: keep (ssize_t)
#include <unistd.h>    // IWYU pragma: keep (readlink, so no ramo do Linux)
#include <utility>

namespace citius {

namespace {

namespace fs = std::filesystem;

// O caminho que o SO diz que o descritor realmente tem (resolvido, sem symlinks), ou nullopt.
std::optional<fs::path> path_of_fd(int fd) {
#ifdef __APPLE__
    std::array<char, PATH_MAX> buffer{}; // NOLINT(misc-include-cleaner): PATH_MAX vem de <climits> (incluido)
    if (::fcntl(fd, F_GETPATH, buffer.data()) == -1) return std::nullopt;
    return fs::path(buffer.data());
#else
    std::array<char, PATH_MAX> buffer{}; // NOLINT(misc-include-cleaner): PATH_MAX vem de <climits> (incluido)
    const std::string link = "/proc/self/fd/" + std::to_string(fd);
    const ssize_t n = ::readlink(link.c_str(), buffer.data(), buffer.size() - 1);
    if (n < 0) return std::nullopt;
    return fs::path(std::string(buffer.data(), static_cast<std::size_t>(n)));
#endif
}

OpenedFile failure(OpenStatus status) {
    OpenedFile result;
    result.status = status;
    return result;
}

OpenStatus status_from_errno(int error) {
    switch (error) {
    case ENOENT:
    case ENOTDIR:
    case ELOOP: // O_NOFOLLOW num symlink: alguem trocou o arquivo por um link
    case ENXIO: // dispositivo/FIFO sem o outro lado
        return OpenStatus::NOT_FOUND;
    case EACCES:
    case EPERM:
        return OpenStatus::FORBIDDEN;
    case EMFILE:
    case ENFILE:
    case ENOMEM:
        return OpenStatus::UNAVAILABLE;
    default:
        return OpenStatus::ERROR;
    }
}

} // namespace

bool fd_is_inside(const FileDescriptor& file, const fs::path& canonical_root) {
    if (!file.valid()) return false;
    const std::optional<fs::path> real = path_of_fd(file.get());
    if (!real) return false;
    // dentro = todos os componentes da raiz sao prefixo do caminho real (e nao texto: "www2" nao esta em "www")
    return std::ranges::mismatch(canonical_root, *real).in1 == canonical_root.end();
}

OpenedFile open_verified(const fs::path& candidate, const fs::path& canonical_root) {
    // O_NONBLOCK: nao travar num FIFO; O_NOFOLLOW: nao seguir um symlink no ultimo componente; O_CLOEXEC: nao vazar o
    // fd para processos filhos. Nenhum dos tres muda a leitura de um arquivo comum.
    FileDescriptor file{::open(candidate.c_str(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC)};
    if (!file.valid()) return failure(status_from_errno(errno));

    // tudo daqui em diante e sobre o objeto JA ABERTO, nao sobre um nome que pode mudar
    struct stat info{};
    if (::fstat(file.get(), &info) != 0) return failure(OpenStatus::ERROR);
    if (!S_ISREG(info.st_mode)) return failure(OpenStatus::NOT_FOUND); // diretorio, FIFO, dispositivo...

    if (!fd_is_inside(file, canonical_root)) return failure(OpenStatus::NOT_FOUND);

    OpenedFile result;
    result.status = OpenStatus::OK;
    result.size = static_cast<std::uint64_t>(info.st_size);
    result.path = candidate;
    result.file = std::move(file);
    return result;
}

OpenedFile open_static_file(const fs::path& root, std::string_view decoded_path) {
    const std::optional<fs::path> canonical_root = canonical_static_root(root);
    if (!canonical_root) return failure(OpenStatus::NOT_FOUND);
    const std::optional<fs::path> candidate = resolve_static_path(root, decoded_path);
    if (!candidate) return failure(OpenStatus::NOT_FOUND);
    return open_verified(*candidate, *canonical_root);
}

} // namespace citius
