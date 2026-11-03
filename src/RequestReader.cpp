#include "citius/RequestReader.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <span>
#include <string_view>

namespace citius {

namespace {
constexpr std::string_view kHeadEnd = "\r\n\r\n";

// Chamar logo apos um recv que falhou (le errno). EAGAIN e EWOULDBLOCK: o SO_RCVTIMEO estourou
// (podem ser valores iguais ou diferentes).
ReadStatus failure_status() {
    return errno == EAGAIN || errno == EWOULDBLOCK ? ReadStatus::TIMEOUT : ReadStatus::ERROR;
}
} // namespace

ReadResult read_request_head(Socket& socket, std::size_t max_bytes, std::string_view initial) {
    ReadResult result;
    result.data.assign(initial);
    std::array<char, 4096> buffer;
    std::size_t search_from = 0;

    while (true) {
        // procura antes de ler: o delimitador pode ja estar em 'initial'
        if (std::size_t end = result.data.find(kHeadEnd, search_from); end != std::string::npos) {
            result.head_size = end + kHeadEnd.size();
            result.status = result.head_size > max_bytes ? ReadStatus::TOO_LARGE : ReadStatus::COMPLETE;
            return result;
        }
        if (result.data.size() > max_bytes) {
            result.status = ReadStatus::TOO_LARGE;
            return result;
        }

        ssize_t received = socket.recv_some(buffer);
        if (received < 0) {
            result.status = failure_status();
            return result;
        }
        if (received == 0) {
            result.status = ReadStatus::CLOSED;
            return result;
        }

        // recomeca a busca alguns bytes antes: o delimitador pode ter sido partido entre leituras
        search_from = result.data.size() >= kHeadEnd.size() - 1 ? result.data.size() - (kHeadEnd.size() - 1) : 0;
        result.data.append(buffer.data(), static_cast<std::size_t>(received));
    }
}

ReadResult read_request_body(Socket& socket, std::string_view already_read, std::size_t length) {
    ReadResult result;
    result.data.assign(already_read.substr(0, length));
    if (already_read.size() > length) {
        result.surplus.assign(already_read.substr(length));
    }
    std::array<char, 4096> buffer;

    while (result.data.size() < length) {
        // pede no maximo o que falta: assim nao consumimos bytes da proxima requisicao
        std::size_t missing = length - result.data.size();
        ssize_t received = socket.recv_some(std::span<char>{buffer.data(), std::min(buffer.size(), missing)});
        if (received < 0) {
            result.status = failure_status();
            return result;
        }
        if (received == 0) {
            result.status = ReadStatus::CLOSED;
            return result;
        }
        result.data.append(buffer.data(), static_cast<std::size_t>(received));
    }
    result.status = ReadStatus::COMPLETE;
    return result;
}

} // namespace citius
