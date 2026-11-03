#include "citius/parse/ChunkedDecoder.hpp"
#include <gtest/gtest.h>
#include <string>
#include <string_view>

using citius::ChunkedDecoder;
using citius::ChunkedProgress;
using citius::ChunkedStatus;
using citius::ParseError;

namespace {

constexpr std::size_t kLimite = 1024 * 1024;

struct Resultado {
    ChunkedStatus status = ChunkedStatus::NEED_MORE;
    std::string corpo;
    std::size_t consumido = 0;
    ParseError erro = ParseError::BAD_REQUEST;
};

// Entrega tudo de uma vez.
Resultado decodificar(std::string_view entrada, std::size_t limite = kLimite) {
    ChunkedDecoder decoder{limite};
    Resultado r;
    ChunkedProgress p = decoder.feed(entrada, r.corpo);
    r.status = p.status;
    r.consumido = p.consumed;
    if (p.status == ChunkedStatus::ERROR) r.erro = decoder.error();
    return r;
}

// Entrega @p passo bytes por vez, como um recv() que devolve pedacos pequenos.
Resultado decodificar_em_pedacos(std::string_view entrada, std::size_t passo) {
    ChunkedDecoder decoder{kLimite};
    Resultado r;
    std::size_t pos = 0;
    while (pos < entrada.size()) {
        const std::string_view pedaco = entrada.substr(pos, passo);
        ChunkedProgress p = decoder.feed(pedaco, r.corpo);
        r.status = p.status;
        r.consumido += p.consumed;
        pos += p.consumed;
        if (p.status == ChunkedStatus::ERROR) {
            r.erro = decoder.error();
            return r;
        }
        if (p.status == ChunkedStatus::DONE) return r;
        if (p.consumed == 0) break; // nao avancou: nao deveria acontecer com NEED_MORE
    }
    return r;
}

} // namespace

TEST(ChunkedDecoderTest, UmChunkSo) {
    Resultado r = decodificar("5\r\nhello\r\n0\r\n\r\n");

    EXPECT_EQ(r.status, ChunkedStatus::DONE);
    EXPECT_EQ(r.corpo, "hello");
    EXPECT_EQ(r.consumido, std::string_view("5\r\nhello\r\n0\r\n\r\n").size());
}

TEST(ChunkedDecoderTest, VariosChunksSaoJuntados) {
    Resultado r = decodificar("3\r\nabc\r\n4\r\ndefg\r\n1\r\nh\r\n0\r\n\r\n");

    EXPECT_EQ(r.status, ChunkedStatus::DONE);
    EXPECT_EQ(r.corpo, "abcdefgh");
}

TEST(ChunkedDecoderTest, CorpoVazio) {
    Resultado r = decodificar("0\r\n\r\n");

    EXPECT_EQ(r.status, ChunkedStatus::DONE);
    EXPECT_TRUE(r.corpo.empty());
}

TEST(ChunkedDecoderTest, TamanhoEmHexadecimalMaiusculoEMinusculo) {
    const std::string quinze(15, 'x');
    Resultado a = decodificar("f\r\n" + quinze + "\r\n0\r\n\r\n");
    Resultado b = decodificar("F\r\n" + quinze + "\r\n0\r\n\r\n");
    Resultado c = decodificar("0f\r\n" + quinze + "\r\n0\r\n\r\n"); // zeros a esquerda sao validos

    EXPECT_EQ(a.corpo, quinze);
    EXPECT_EQ(b.corpo, quinze);
    EXPECT_EQ(c.corpo, quinze);
}

TEST(ChunkedDecoderTest, DadosPodemConterCrLfENul) {
    const std::string dados("a\r\nb\0c", 6);
    Resultado r = decodificar("6\r\n" + dados + "\r\n0\r\n\r\n");

    EXPECT_EQ(r.status, ChunkedStatus::DONE);
    EXPECT_EQ(r.corpo, dados); // o tamanho manda, nao o conteudo
}

TEST(ChunkedDecoderTest, ExtensaoDoChunkEIgnorada) {
    Resultado r = decodificar("5;nome=valor;outra\r\nhello\r\n0;fim\r\n\r\n");

    EXPECT_EQ(r.status, ChunkedStatus::DONE);
    EXPECT_EQ(r.corpo, "hello");
}

TEST(ChunkedDecoderTest, EspacoAntesDaExtensaoEPermitido) {
    EXPECT_EQ(decodificar("5 ;x=y\r\nhello\r\n0\r\n\r\n").status, ChunkedStatus::DONE);
}

TEST(ChunkedDecoderTest, TrailersSaoDescartados) {
    Resultado r = decodificar("5\r\nhello\r\n0\r\nX-Checksum: abc\r\nOutro: 1\r\n\r\n");

    EXPECT_EQ(r.status, ChunkedStatus::DONE);
    EXPECT_EQ(r.corpo, "hello");
}

TEST(ChunkedDecoderTest, NaoConsomeAlemDoFimDaMensagem) {
    // pipelining: o que vem depois e o comeco da proxima requisicao
    const std::string msg = "5\r\nhello\r\n0\r\n\r\n";
    Resultado r = decodificar(msg + "GET /b HTTP/1.1\r\n");

    EXPECT_EQ(r.status, ChunkedStatus::DONE);
    EXPECT_EQ(r.consumido, msg.size());
}

TEST(ChunkedDecoderTest, FuncionaComQualquerTamanhoDePedaco) {
    const std::string msg = "5;a=b\r\nhello\r\n6\r\n world\r\n0\r\nT: v\r\n\r\n";
    for (std::size_t passo = 1; passo <= msg.size(); ++passo) {
        Resultado r = decodificar_em_pedacos(msg, passo);
        EXPECT_EQ(r.status, ChunkedStatus::DONE) << "passo " << passo;
        EXPECT_EQ(r.corpo, "hello world") << "passo " << passo;
        EXPECT_EQ(r.consumido, msg.size()) << "passo " << passo;
    }
}

TEST(ChunkedDecoderTest, MensagemIncompletaPedeMaisBytes) {
    EXPECT_EQ(decodificar("5\r\nhel").status, ChunkedStatus::NEED_MORE);
    EXPECT_EQ(decodificar("5\r\nhello").status, ChunkedStatus::NEED_MORE);
    EXPECT_EQ(decodificar("5\r\nhello\r\n").status, ChunkedStatus::NEED_MORE);
    EXPECT_EQ(decodificar("5\r\nhello\r\n0\r\n").status, ChunkedStatus::NEED_MORE);
    EXPECT_EQ(decodificar("5\r\nhello\r\n0\r\nT: v\r\n").status, ChunkedStatus::NEED_MORE);
    EXPECT_EQ(decodificar("").status, ChunkedStatus::NEED_MORE);
}

TEST(ChunkedDecoderTest, DadosParciaisJaChegamAoCorpo) {
    ChunkedDecoder decoder{kLimite};
    std::string corpo;

    ASSERT_EQ(decoder.feed("5\r\nhel", corpo).status, ChunkedStatus::NEED_MORE);
    EXPECT_EQ(corpo, "hel");
    ASSERT_EQ(decoder.feed("lo\r\n0\r\n\r\n", corpo).status, ChunkedStatus::DONE);
    EXPECT_EQ(corpo, "hello");
}

// ---- entradas invalidas (todas viram 400; um parser frouxo aqui e a porta do request smuggling) -------------------

TEST(ChunkedDecoderTest, TamanhoQueNaoEHexadecimalDa400) {
    for (const char* msg : {"xyz\r\nabc\r\n0\r\n\r\n", "g\r\na\r\n0\r\n\r\n", "5g\r\nhello\r\n0\r\n\r\n"}) {
        Resultado r = decodificar(msg);
        EXPECT_EQ(r.status, ChunkedStatus::ERROR) << msg;
        EXPECT_EQ(r.erro, ParseError::BAD_REQUEST) << msg;
    }
}

TEST(ChunkedDecoderTest, TamanhoVazioOuComSinalOuPrefixoDa400) {
    for (const char* msg : {"\r\nabc\r\n0\r\n\r\n", "+5\r\nhello\r\n0\r\n\r\n", "-5\r\nhello\r\n0\r\n\r\n",
                            "0x5\r\nhello\r\n0\r\n\r\n", " 5\r\nhello\r\n0\r\n\r\n"}) {
        Resultado r = decodificar(msg);
        EXPECT_EQ(r.status, ChunkedStatus::ERROR) << msg;
        EXPECT_EQ(r.erro, ParseError::BAD_REQUEST) << msg;
    }
}

TEST(ChunkedDecoderTest, TamanhoComDigitosDemaisNaoEstouraOInteiro) {
    // 17 digitos: passaria de 64 bits se fosse lido sem checar
    Resultado r = decodificar("fffffffffffffffff\r\nx\r\n0\r\n\r\n");

    EXPECT_EQ(r.status, ChunkedStatus::ERROR);
}

TEST(ChunkedDecoderTest, LfSozinhoNaLinhaDoTamanhoDa400) {
    // CRLF e obrigatorio; aceitar so LF e como servidores discordam de onde a linha termina
    Resultado r = decodificar("5\nhello\r\n0\r\n\r\n");

    EXPECT_EQ(r.status, ChunkedStatus::ERROR);
    EXPECT_EQ(r.erro, ParseError::BAD_REQUEST);
}

TEST(ChunkedDecoderTest, FaltaOCrLfDepoisDosDadosDa400) {
    for (const char* msg : {"5\r\nhelloXX0\r\n\r\n", "5\r\nhello\n0\r\n\r\n", "5\r\nhello\r0\r\n\r\n"}) {
        Resultado r = decodificar(msg);
        EXPECT_EQ(r.status, ChunkedStatus::ERROR) << msg;
        EXPECT_EQ(r.erro, ParseError::BAD_REQUEST) << msg;
    }
}

TEST(ChunkedDecoderTest, ChunkMaiorQueODeclaradoDa400) {
    // diz 3 bytes mas manda 5: depois do 3o byte ainda nao ha CRLF
    Resultado r = decodificar("3\r\nhello\r\n0\r\n\r\n");

    EXPECT_EQ(r.status, ChunkedStatus::ERROR);
}

TEST(ChunkedDecoderTest, ExtensaoComCaractereDeControleDa400) {
    Resultado r = decodificar(std::string("5;a=\x01\r\nhello\r\n0\r\n\r\n"));

    EXPECT_EQ(r.status, ChunkedStatus::ERROR);
}

TEST(ChunkedDecoderTest, ExtensaoSemPontoEVirgulaDa400) {
    EXPECT_EQ(decodificar("5 x\r\nhello\r\n0\r\n\r\n").status, ChunkedStatus::ERROR);
}

TEST(ChunkedDecoderTest, LinhaDeTamanhoGigantePassaDoLimiteDa400) {
    // um cliente que nunca manda o CRLF nao pode fazer o servidor guardar a linha para sempre
    Resultado r = decodificar("5;" + std::string(5000, 'a'));

    EXPECT_EQ(r.status, ChunkedStatus::ERROR);
    EXPECT_EQ(r.erro, ParseError::BAD_REQUEST);
}

TEST(ChunkedDecoderTest, TrailerSemDoisPontosDa400) {
    EXPECT_EQ(decodificar("0\r\nlixo\r\n\r\n").status, ChunkedStatus::ERROR);
}

TEST(ChunkedDecoderTest, TrailerComNomeInvalidoDa400) {
    EXPECT_EQ(decodificar("0\r\nNome Ruim: v\r\n\r\n").status, ChunkedStatus::ERROR);
    EXPECT_EQ(decodificar("0\r\n: v\r\n\r\n").status, ChunkedStatus::ERROR);
}

TEST(ChunkedDecoderTest, TrailerComLfSozinhoDa400) {
    EXPECT_EQ(decodificar("0\r\nT: v\n\r\n").status, ChunkedStatus::ERROR);
}

TEST(ChunkedDecoderTest, TrailersGrandesDemaisDa400) {
    std::string msg = "0\r\n";
    for (int i = 0; i < 400; ++i) msg += "T" + std::to_string(i) + ": " + std::string(40, 'v') + "\r\n";
    msg += "\r\n";

    Resultado r = decodificar(msg);

    EXPECT_EQ(r.status, ChunkedStatus::ERROR);
    EXPECT_EQ(r.erro, ParseError::BAD_REQUEST);
}

// ---- limite do corpo ---------------------------------------------------------------------------------------------

TEST(ChunkedDecoderTest, CorpoNoLimiteEAceito) {
    Resultado r = decodificar("a\r\n0123456789\r\n0\r\n\r\n", 10);

    EXPECT_EQ(r.status, ChunkedStatus::DONE);
    EXPECT_EQ(r.corpo.size(), 10u);
}

TEST(ChunkedDecoderTest, ChunkQueEstouraOLimiteDa413SemReceberOsDados) {
    // o tamanho declarado ja basta: nao esperamos 1 GB chegar para recusar
    Resultado r = decodificar("40000000\r\n", kLimite); // 1 GiB

    EXPECT_EQ(r.status, ChunkedStatus::ERROR);
    EXPECT_EQ(r.erro, ParseError::PAYLOAD_TOO_LARGE);
}

TEST(ChunkedDecoderTest, SomaDosChunksEstouraOLimiteDa413) {
    Resultado r = decodificar("6\r\n012345\r\n6\r\n678901\r\n0\r\n\r\n", 10);

    EXPECT_EQ(r.status, ChunkedStatus::ERROR);
    EXPECT_EQ(r.erro, ParseError::PAYLOAD_TOO_LARGE);
}

TEST(ChunkedDecoderTest, DepoisDoErroContinuaEmErro) {
    ChunkedDecoder decoder{kLimite};
    std::string corpo;
    ASSERT_EQ(decoder.feed("xx\r\n", corpo).status, ChunkedStatus::ERROR);

    // depois de um erro nao se sabe onde a mensagem termina: nada mais e interpretado
    EXPECT_EQ(decoder.feed("5\r\nhello\r\n0\r\n\r\n", corpo).status, ChunkedStatus::ERROR);
    EXPECT_TRUE(corpo.empty());
}

TEST(ChunkedDecoderTest, DepoisDeTerminarNaoConsomeMais) {
    ChunkedDecoder decoder{kLimite};
    std::string corpo;
    ASSERT_EQ(decoder.feed("0\r\n\r\n", corpo).status, ChunkedStatus::DONE);

    ChunkedProgress p = decoder.feed("5\r\nhello\r\n", corpo);

    EXPECT_EQ(p.status, ChunkedStatus::DONE);
    EXPECT_EQ(p.consumed, 0u);
    EXPECT_TRUE(corpo.empty());
}
