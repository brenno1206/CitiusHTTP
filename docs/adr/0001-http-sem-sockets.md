# 0001. A camada `http/` não conhece sockets

- Status: Aceita
- Data: 2026-11-03

## Contexto

Parsear HTTP é lógica pura (texto entra, estrutura sai), mas costuma ficar misturada com a leitura
do socket. Aí cada teste do parser precisa de rede, e cada caso de borda (cabeçalho partido ao
meio, corpo grande, lixo) vira um teste lento e instável.

## Decisão

Os tipos de protocolo (`HttpParser`, `HttpRequest`, `HttpResponse`, `Router`, `RequestAssembler`) só
recebem `std::string_view` e devolvem estruturas; nenhum inclui header de socket. Quem lê e escreve
bytes é a `Connection`, que usa o `Socket` (RAII) e entrega os bytes ao `RequestAssembler`.

## Alternativas consideradas

- **Parser lendo direto do socket.** Menos código, mas só se testa com rede e não dá para reaproveitar
  num modelo não bloqueante (ver 0003).

## Consequências

- Testes unitários de protocolo rodam em milissegundos, sem porta nem thread; os casos maliciosos
  (cabeçalhos gigantes, `Content-Length` repetido, `Transfer-Encoding`) são baratos de cobrir.
- Foi o que tornou possível trocar o servidor bloqueante pelo event loop sem reescrever o parser.
- Custo: uma camada a mais de indireção entre os bytes e a requisição.
- A separação em pastas (`http/`, `net/`, `core/`) ainda não foi feita no disco: os arquivos estão
  planos em `include/citius/` e `src/`.
