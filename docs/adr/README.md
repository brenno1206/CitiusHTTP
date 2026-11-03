# Decisões de arquitetura (ADRs)

Um ADR (*Architecture Decision Record*) é um texto curto que registra **uma decisão**: o problema,
o que foi escolhido, o que ficou de fora e o que isso custa. Daqui a seis meses o código mostra
*o quê*; o ADR lembra *por quê*.

| # | Decisão | Status |
|---|---|---|
| [0001](0001-http-sem-sockets.md) | A camada `http/` não conhece sockets | Aceita |
| [0002](0002-erros-em-dois-niveis.md) | Erros em dois níveis: exceção para o ambiente, valor para o cliente | Aceita |
| [0003](0003-event-loop-em-vez-de-thread-pool.md) | Event loop de uma thread em vez de thread pool | Aceita |
| [0004](0004-poller-kqueue-epoll.md) | `Poller` com `kqueue` (macOS) e `epoll` (Linux) | Aceita |
| [0005](0005-prazos-com-tempo-injetado.md) | Prazos por conexão, totais por requisição, com tempo injetado | Aceita |
| [0006](0006-arquivos-estaticos-em-streaming.md) | Arquivos estáticos em streaming, validados no descritor | Aceita |
| [0007](0007-lingering-close-backoff-e-fila-de-prazos.md) | Fechamento *lingering*, backoff do `accept` e fila de prazos | Aceita |
| [0008](0008-chunked-e-expect-sem-abrir-brecha-de-smuggling.md) | `chunked` e `Expect: 100-continue` sem abrir brecha de smuggling | Aceita |
| [0009](0009-configuracao-por-arquivo-e-log-em-arquivo.md) | Configuração por arquivo e log em arquivo | Aceita |
| [0010](0010-limite-de-requisicoes-por-conexao.md) | Limite de requisições por conexão: 1000, configurável | Aceita |

## Modelo

```markdown
# NNNN. Título curto da decisão

- Status: Proposta | Aceita | Substituída por NNNN
- Data: AAAA-MM-DD

## Contexto
O problema e as forças em jogo.

## Decisão
O que foi escolhido, em uma ou duas frases, e como.

## Alternativas consideradas
O que ficou de fora e por quê.

## Consequências
O que melhora, o que piora, o que fica para depois.
```

Um ADR aceito não se reescreve: se a decisão mudar, escreva um novo e marque o antigo como
*Substituída por*.
