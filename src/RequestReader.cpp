#include "citius/RequestReader.hpp"
#include <algorithm>
#include <array>
#include <span>
#include <cerrno>
#include <string_view>

namespace citius {

namespace {
constexpr std::string_view kHeadEnd = "\r\n\r\n";

// Chamar logo apos um recv que falhou (le errno). EAGAIN e EWOULDBLOCK: o SO_RCVTIMEO estourou
// (podem ser valores iguais ou diferentes).
ReadStatus failure_status() {
    return errno == EAGAIN || errno == EWOULDBLOCK ? ReadStatus::TIMEOUT : ReadStatus::ERROR;
}
}

ReadResult read_request_head(Socket& socket, std::size_t max_bytes) {
    ReadResult result;
    std::array<char, 4096> buffer;

    while (true) {
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
        std::size_t search_from = result.data.size() >= kHeadEnd.size() - 1
                                      ? result.data.size() - (kHeadEnd.size() - 1)
                                      : 0;
        result.data.append(buffer.data(), static_cast<std::size_t>(received));

        if (std::size_t end = result.data.find(kHeadEnd, search_from); end != std::string::npos) {
            result.head_size = end + kHeadEnd.size();
            result.status = ReadStatus::COMPLETE;
            return result;
        }
        if (result.data.size() > max_bytes) {
            result.status = ReadStatus::TOO_LARGE;
            return result;
        }
    }
}

ReadResult read_request_body(Socket& socket, std::string_view already_read, std::size_t length) {
    ReadResult result;
    result.data.assign(already_read.substr(0, length));
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
