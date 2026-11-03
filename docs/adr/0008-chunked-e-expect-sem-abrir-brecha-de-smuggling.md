# 0008. `Transfer-Encoding: chunked` e `Expect: 100-continue` sem abrir brecha de smuggling

- Status: Aceita
- Data: 2026-11-03

## Contexto

O servidor respondia 501 a qualquer `Transfer-Encoding` e ignorava `Expect`. Clientes reais (curl com corpo grande, bibliotecas
HTTP) usam `chunked` e `Expect: 100-continue`, então suportá-los é necessário. Mas `chunked` é a fonte clássica de **request
smuggling**: dois servidores em fila que discordam de onde uma mensagem termina deixam um atacante "esconder" uma segunda
requisição dentro da primeira.

## Decisão

- O decodificador (`ChunkedDecoder`) é **estrito e incremental**, sem socket: tamanho só em hexadecimal (1 a 16 dígitos; sem
  sinal, `0x` ou espaço antes), **CRLF obrigatório** (um LF sozinho é erro), extensões sem caractere de controle, trailers
  validados como cabeçalhos mas **descartados**, linhas até 1 KB, trailers até 8 KB, corpo total até 1 MB. Um chunk que
  estoura o limite é recusado (413) **assim que o tamanho é lido**, sem esperar os dados.
- O parser trata qualquer ambiguidade como **400**: `Transfer-Encoding` junto com `Content-Length`; `Transfer-Encoding` em
  HTTP/1.0; lista com item vazio; `chunked` repetido; `chunked` que não é a última codificação. Outras codificações que não
  conhecemos (`gzip`, `identity`...) são **501**.
- `Expect: 100-continue` é respondido com o interino `HTTP/1.1 100 Continue` **só se** a cabeça foi aceita e o corpo ainda não
  chegou. Se o corpo ia ser recusado (413, 417...), não se convida o cliente a enviá-lo. Outro valor de `Expect` é 417; em
  HTTP/1.0 ele é ignorado (RFC 9110 §10.1.1).

## Alternativas consideradas

- **Aceitar LF sozinho e outras tolerâncias** (como alguns servidores fazem). Cada tolerância é uma discordância em potencial
  com o servidor ao lado. Preferimos recusar.
- **Guardar os trailers.** Nenhum handler precisa deles hoje, e eles são mais uma superfície para validar.
- **Decodificar `chunked` dentro do `HttpParser`.** O parser vê só a cabeça; o corpo chega aos poucos e precisa de estado
  entre leituras, que é o papel do `RequestAssembler` + `ChunkedDecoder`.

## Consequências

- 30 testes só do decodificador (cada tamanho de pedaço de 1 byte até a mensagem inteira, entradas malformadas, limites) e
  testes do parser, do assembler, da `Connection` e de integração.
- O `100 Continue` é uma única tentativa de `send` (25 bytes num buffer vazio). Se falhar, o cliente espera ~1 s e envia o
  corpo mesmo assim, como a RFC prevê.
- Respostas chunked (do servidor) continuam fora de escopo: toda resposta tem `Content-Length`.
