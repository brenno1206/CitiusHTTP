#include "citius/parse/ChunkedDecoder.hpp"
#include "citius/http/HttpSyntax.hpp"
#include "citius/util/AsciiText.hpp"
#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>

namespace citius {

namespace {

// Valor de um digito hexadecimal, ou -1 se nao for.
int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// 16 digitos hexadecimais cabem exatamente em 64 bits: com mais que isso o valor poderia estourar
constexpr std::size_t kMaxSizeDigits = 16;

} // namespace

void ChunkedDecoder::fail(ParseError error) noexcept {
    m_state = State::FAILED;
    m_error = error;
    m_line.clear();
}

bool ChunkedDecoder::read_line(std::string_view input, std::size_t& used) {
    while (used < input.size()) {
        const char c = input[used++];
        if (c == '\n') {
            // LF sozinho (sem o CR) e ambiguo: outro servidor poderia ler a linha de outro jeito
            if (m_line.empty() || m_line.back() != '\r') {
                fail(ParseError::BAD_REQUEST);
                return false;
            }
            m_line.pop_back();
            return true;
        }
        m_line.push_back(c);
        if (m_line.size() > kMaxLineBytes) {
            fail(ParseError::BAD_REQUEST);
            return false;
        }
    }
    return false;
}

void ChunkedDecoder::parse_size_line() {
    std::size_t digits = 0;
    std::size_t value = 0;
    while (digits < m_line.size() && hex_value(m_line[digits]) >= 0) {
        value = (value * 16) + static_cast<std::size_t>(hex_value(m_line[digits]));
        if (++digits > kMaxSizeDigits) return fail(ParseError::BAD_REQUEST);
    }
    if (digits == 0) return fail(ParseError::BAD_REQUEST);

    // depois do tamanho: nada, ou [BWS] ";" extensao (ignorada, mas sem caractere de controle)
    std::string_view rest = std::string_view{m_line}.substr(digits);
    while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\t')) rest.remove_prefix(1);
    if (!rest.empty() && (rest.front() != ';' || has_control_chars(rest))) return fail(ParseError::BAD_REQUEST);

    // recusa pelo tamanho declarado, sem esperar os dados chegarem
    if (value > m_max_body - m_total) return fail(ParseError::PAYLOAD_TOO_LARGE);

    m_line.clear();
    if (value == 0) {
        m_state = State::TRAILER_LINE;
    } else {
        m_remaining = value;
        m_state = State::DATA;
    }
}

void ChunkedDecoder::parse_trailer_line() {
    m_trailer_bytes += m_line.size() + 2; // + CRLF
    if (m_trailer_bytes > kMaxTrailerBytes) return fail(ParseError::BAD_REQUEST);

    if (m_line.empty()) { // linha vazia: fim da mensagem
        m_state = State::DONE;
        return;
    }
    // um trailer e um campo de cabecalho: validamos o formato, mas o valor e descartado
    const std::size_t colon = m_line.find(':');
    if (has_control_chars(m_line) || colon == std::string::npos ||
        !is_token(std::string_view{m_line}.substr(0, colon))) {
        return fail(ParseError::BAD_REQUEST);
    }
    m_line.clear();
}

ChunkedProgress ChunkedDecoder::feed(std::string_view input, std::string& body) {
    if (m_state == State::FAILED) return {ChunkedStatus::ERROR, 0};
    if (m_state == State::DONE) return {ChunkedStatus::DONE, 0};

    std::size_t used = 0;
    while (used < input.size() && m_state != State::DONE && m_state != State::FAILED) {
        switch (m_state) {
        case State::SIZE_LINE:
            if (read_line(input, used)) parse_size_line();
            break;
        case State::DATA: {
            const std::size_t n = std::min(m_remaining, input.size() - used);
            body.append(input.substr(used, n));
            used += n;
            m_remaining -= n;
            m_total += n;
            if (m_remaining == 0) {
                m_state = State::CHUNK_END;
                m_cr_seen = false;
            }
            break;
        }
        case State::CHUNK_END: {
            // exatamente CRLF depois dos dados do chunk
            const char c = input[used++];
            if (!m_cr_seen && c == '\r') {
                m_cr_seen = true;
            } else if (m_cr_seen && c == '\n') {
                m_state = State::SIZE_LINE;
            } else {
                fail(ParseError::BAD_REQUEST);
            }
            break;
        }
        case State::TRAILER_LINE:
            if (read_line(input, used)) parse_trailer_line();
            break;
        case State::DONE:
        case State::FAILED:
            break;
        }
    }

    if (m_state == State::FAILED) return {ChunkedStatus::ERROR, used};
    if (m_state == State::DONE) return {ChunkedStatus::DONE, used};
    return {ChunkedStatus::NEED_MORE, used};
}

} // namespace citius
