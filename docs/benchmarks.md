# Benchmarks

Medidas com `wrk` para comparar o servidor antes e depois do event loop (Fase 7).

## Como medir

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release   # sem otimização o número não vale
cmake --build build-release --target citius_server
./build-release/citius_server --port 8099 --log-level error &
wrk -t4 -c64   -d10s http://localhost:8099/ping   # rota do Router (resposta curta)
wrk -t4 -c64   -d10s http://localhost:8099/       # arquivo estático (www/index.html)
wrk -t4 -c1000 -d10s http://localhost:8099/ping   # muitas conexões simultâneas
wrk -t4 -c3000 -d10s http://localhost:8099/ping   # ainda mais (precisa de ulimit -n alto no shell do wrk)
```

Cliente e servidor rodam na mesma máquina e disputam CPU: o valor absoluto importa menos que a
comparação entre as versões, medidas nas mesmas condições.

Ambiente: Apple M3 (8 núcleos), macOS, Apple Clang, `Release`. Servidor com os padrões
(`--threads 0` = 8 threads, fila de 128).

## Baseline: thread pool (uma thread por conexão), antes da Fase 7

| Cenário | Req/s | Latência média | Observações |
|---|---|---|---|
| `/ping`, 64 conexões | 188.632 | 7,07 ms | sem erros |
| `/` (arquivo), 64 conexões | 50.025 | 26,26 ms | sem erros |
| `/ping`, 1000 conexões | 69.948 | 10,31 ms (máx. 1,37 s) | 204.149 respostas não-2xx (503: fila cheia), erros de socket: 862 connect, 1147 read, 42 write, 144 timeout |

Leitura: com 64 conexões o pool dá conta, porque cada conexão é recusada ou fechada rápido (limite
de 100 requisições por conexão). Com 1000 conexões abertas ao mesmo tempo, 8 threads + fila de 128
não bastam: cada conexão keep-alive prende uma thread (dívida 5 do `CLAUDE.md`), e o resto recebe
503 ou erro. É o cenário que o event loop deve resolver.

## Depois do event loop (Fase 7)

Servidor novo: um event loop numa thread só (`kqueue`), uma `Connection` por cliente, padrões
(`--max-connections 4096`). Para a comparação ser justa, o servidor antigo (commit `6cdaf91`, thread
pool) foi compilado em Release e os dois foram medidos em seguida, na mesma sessão, com o mesmo
script (`wrk -t4 -d10s`, uma conexão de cada vez por cenário). O limite de descritores do shell
era 1.048.576: os erros do antigo não vêm do `wrk`.

| Cenário | Antigo (pool) req/s | Novo (loop) req/s | Latência média antigo → novo | Respostas não-2xx antigo → novo |
|---|---|---|---|---|
| `/ping`, 64 conexões | 183.817 | **253.989** (+38%) | 7,26 ms → 0,24 ms | 0 → 0 |
| `/` (arquivo), 64 conexões | **48.740** | 24.476 (−50%) | 27,5 ms → 2,6 ms | 0 → 0 |
| `/ping`, 1000 conexões | 84.303 | **263.246** (3,1×) | 28,0 ms → 3,8 ms | 214.372 → 0 |
| `/ping`, 3000 conexões | 15.280 | **234.142** (15×) | 113 ms → 12,7 ms | 153.111 → 0 |

Erros de socket (`wrk`): com 1000 conexões, o antigo teve 863 de connect, 914 de read e 104 de
timeout; o novo, nenhum. Com 3000, o antigo teve 27.286 de read e 54 de timeout; o novo, 1.191 de
read (0,05% das requisições; causa não investigada).

### Como ler esses números

- **O req/s do antigo com muitas conexões engana.** Ele conta as respostas `503` (rápidas de
  gerar) como requisições: com 3000 conexões, 153.111 de ~152.800 requisições medidas foram não-2xx,
  ou seja, **praticamente nada foi atendido de verdade**. O novo serve todas.
- **Latência:** com 64 conexões a média caiu de 7 ms para 0,24 ms. Num thread pool, a conexão que
  espera uma thread livre acumula fila; no loop quem está pronto é atendido na hora.
- **Servir arquivo ficou mais lento (a perda da Fase 7).** `serve_static` faz trabalho de sistema de
  arquivos (`weakly_canonical`, abrir, ler o arquivo inteiro) dentro do handler. Antes isso se
  espalhava por 8 threads; agora roda na única thread do loop, que fica ocupada e atrasa os demais.
  É o custo previsto da opção A (loop único). Se for relevante, o caminho é a opção B: o loop só
  faz I/O de rede e entrega handlers lentos ao `ThreadPool` (que continua na biblioteca).
- Cliente e servidor disputam os mesmos núcleos (M3, 8 núcleos); `wrk` usa 4 threads. O valor
  absoluto importa menos que a comparação.
