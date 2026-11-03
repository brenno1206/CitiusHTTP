#include "citius/RequestReader.hpp"
#include <algorithm>
#include <array>
#include <string_view>

namespace citius {

namespace {
constexpr std::string_view kHeadEnd = "\r\n\r\n";
}

ReadResult read_request_head(Socket& socket, std::size_t max_bytes) {
    ReadResult result;
    std::array<char, 4096> buffer;

    while (true) {
        ssize_t received = socket.recv_some(buffer);
        if (received < 0) {
            result.status = ReadStatus::ERROR;
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

        if (result.data.find(kHeadEnd, search_from) != std::string::npos) {
            result.status = ReadStatus::COMPLETE;
            return result;
        }
        if (result.data.size() > max_bytes) {
            result.status = ReadStatus::TOO_LARGE;
            return result;
        }
    }
}

} // namespace citius
