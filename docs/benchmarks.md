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

## Depois do event loop

_(a preencher ao final da Fase 7)_
