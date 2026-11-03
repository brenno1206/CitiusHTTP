# 0003. Event loop de uma thread em vez de thread pool

- Status: Aceita
- Data: 2026-11-03

## Contexto

Na Fase 4 o servidor passou a usar um thread pool: uma thread por conexão aceita. Com keep-alive
(Fase 5), cada conexão aberta prende uma thread até fechar, mesmo parada. Com 8 threads e fila de
128, mil clientes simultâneos já derrubavam o servidor.

Medido com `wrk` (Release, M3 de 8 núcleos; detalhes em [`docs/benchmarks.md`](../benchmarks.md)):

| `/ping` | Thread pool | Event loop |
|---|---|---|
| 1000 conexões | 84.303 req/s, 214 mil respostas 503 | 263.246 req/s, nenhuma falha |
| 3000 conexões | 15.280 req/s, quase tudo 503 | 234.142 req/s, nenhuma falha |

## Decisão

Um **event loop numa thread só**: o `Poller` (ver 0004) avisa quais conexões têm algo a fazer e cada
`Connection` é uma máquina de estados não bloqueante. O limite passa a ser o número de conexões
abertas (`--max-connections`, acima dele 503), não o de threads.

## Alternativas consideradas

- **Manter o thread pool e aumentar threads/fila.** Cada thread custa uma pilha e troca de contexto;
  milhares de clientes parados continuam caros.
- **Loop + thread pool para os handlers (opção B).** Mais complexo (devolver a resposta ao loop,
  acordá-lo) e exige TSan em tudo. Fica como evolução se os números pedirem.
- **Uma thread por conexão, sem pool.** Mesmo problema, sem sequer o teto.

## Consequências

- Muito melhor com muitas conexões, e latência média com 64 conexões caiu de 7 ms para 0,24 ms.
- **Custo medido:** servir arquivo estático ficou ~2× mais lento (48.740 → 24.476 req/s com 64
  conexões). O `serve_static` faz trabalho de disco dentro do loop, e antes isso se espalhava por 8 threads.
- Um handler lento atrasa **todos** os clientes. Handlers devem ser curtos; o caminho para os que não
  puderem é a opção B (o `ThreadPool` continua na biblioteca, sem uso pelo `Server`).
- Acabou a corrida de dados entre threads do servidor, mas o código passa a depender de nunca bloquear.
- Quebra de compatibilidade: `--threads` e `--max-queued` deram lugar a `--max-connections`.
