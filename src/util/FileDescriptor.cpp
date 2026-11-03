#include "citius/util/FileDescriptor.hpp"
#include <unistd.h> // close()
#include <utility>

namespace citius {

FileDescriptor::~FileDescriptor() {
    reset();
}

FileDescriptor::FileDescriptor(FileDescriptor&& other) noexcept : m_fd(other.release()) {}

FileDescriptor& FileDescriptor::operator=(FileDescriptor&& other) noexcept {
    // sem este teste, "a = std::move(a)" fecharia o proprio fd
    if (this != &other) {
        reset(other.release());
    }
    return *this;
}

int FileDescriptor::release() noexcept {
    return std::exchange(m_fd, -1);
}

void FileDescriptor::reset(int new_fd) noexcept {
    if (m_fd >= 0) {
        ::close(m_fd);
    }
    m_fd = new_fd;
}

} // namespace citius
