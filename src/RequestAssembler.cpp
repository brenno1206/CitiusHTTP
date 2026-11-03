#include "citius/RequestAssembler.hpp"
#include <stdexcept>
#include <utility>

namespace citius {

namespace {
constexpr std::string_view kHeadEnd = "\r\n\r\n";
}

AssembleStatus RequestAssembler::feed(std::string_view bytes) {
    if (m_state == State::FAILED) {
        return AssembleStatus::ERROR;
    }
    m_buffer.append(bytes);
    return advance();
}

AssembleStatus RequestAssembler::advance() {
    if (m_state == State::DONE) {
        return AssembleStatus::COMPLETE;
    }

    if (m_state == State::HEAD) {
        const std::size_t end = m_buffer.find(kHeadEnd, m_search_from);
        if (end == std::string::npos) {
            if (m_buffer.size() > m_max_head_bytes) {
                return fail(StatusCode::HEADERS_TOO_LARGE);
            }
            // recomeca a busca alguns bytes antes: o delimitador pode vir partido entre leituras
            m_search_from = m_buffer.size() >= kHeadEnd.size() - 1 ? m_buffer.size() - (kHeadEnd.size() - 1) : 0;
            return AssembleStatus::NEED_MORE;
        }

        // o limite vale tambem quando a cabeca chega inteira de uma vez
        const std::size_t head_size = end + kHeadEnd.size();
        if (head_size > m_max_head_bytes) {
            return fail(StatusCode::HEADERS_TOO_LARGE);
        }

        ParseResult parsed = HttpParser::parse(std::string_view{m_buffer}.substr(0, head_size));
        if (!parsed.request) {
            // pela invariante de ParseResult sem requisicao ha sempre um erro; o BAD_REQUEST so cobre um uso indevido
            return fail(parsed.error ? to_status(*parsed.error) : StatusCode::BAD_REQUEST);
        }
        m_request = std::move(*parsed.request);
        m_buffer.erase(0, head_size);
        m_search_from = 0;
        m_state = State::BODY;
    }

    // BODY: o parser ja limitou content_length (413), entao esperar por ele e seguro
    if (m_buffer.size() < m_request.content_length) {
        return AssembleStatus::NEED_MORE;
    }
    m_request.body.assign(m_buffer, 0, m_request.content_length);
    m_buffer.erase(0, m_request.content_length);
    m_state = State::DONE;
    return AssembleStatus::COMPLETE;
}

AssembleStatus RequestAssembler::fail(StatusCode status) {
    m_state = State::FAILED;
    m_error = status;
    m_buffer.clear(); // nao ha o que aproveitar: depois de um erro a conexao fecha
    return AssembleStatus::ERROR;
}

HttpRequest RequestAssembler::take_request() {
    if (m_state != State::DONE) {
        throw std::logic_error("RequestAssembler::take_request sem requisicao completa.");
    }
    HttpRequest request = std::exchange(m_request, HttpRequest{});
    m_state = State::HEAD;
    // a sobra em m_buffer e o comeco da proxima requisicao; so falta o chamador pedir feed("")
    return request;
}

StatusCode RequestAssembler::error_status() const {
    if (!m_error) {
        throw std::logic_error("RequestAssembler::error_status sem erro.");
    }
    return *m_error;
}

} // namespace citius
