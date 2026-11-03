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

## Erros de leitura com 3000 conexões: investigação e correção

Depois da Fase 7, o `wrk` com 3000 conexões mostrava de ~1.000 a ~2.000 "Socket errors: read" (≈0,05–0,1% das
requisições), sem nenhuma anomalia correspondente no log do servidor (só ~20 erros de leitura do lado dele). A causa
não era óbvia, então foi investigada com experimentos controlados (mesma máquina, Release, `wrk -t4 -c3000`):

| Experimento | Erros de leitura | Vazão |
|---|---|---|
| Servidor da Fase 7/8 (limite de 100 requisições/conexão, `accept` em lotes de 64) | 1.100–2.300 | ~240 mil req/s |
| *Lingering close* (FIN + descarte antes de fechar) | 1.100–2.300 (igual) | igual |
| Limite de requisições por conexão praticamente removido | **0** | ~266 mil req/s |
| Limite de 100, `accept` em lotes de 4096 | 560–980 | ~245 mil req/s |
| Limite de **1000**, `accept` em lotes de 1024 | **0** (3 rodadas) | ~263 mil req/s |

**Causa:** com 3000 clientes que começam juntos, todos chegam à requisição nº 100 quase ao mesmo tempo, o servidor fecha
as 3000 conexões e o `wrk` reconecta todas numa rajada. A fila de espera do `listen` no macOS tem 128 posições
(`kern.ipc.somaxconn`); a rajada a estoura, e o cliente que o kernel considerou conectado leva RST. Aceitar mais clientes
por volta do loop ajuda (a fila esvazia mais rápido), mas o que resolve é **reduzir a rajada**: o limite padrão de
requisições por conexão passou de 100 para **1000** (o nginx moderno também usa 1000), e ele agora é configurável
(`--max-requests`, `Server::set_max_requests_per_connection`).

Comparação final, três rodadas alternadas de cada servidor (`/ping`, 6 s por rodada):

| Conexões | Fase 8 (`main`) | Esta versão | Erros de leitura (`main` → esta) |
|---|---|---|---|
| 1000 | 288 / 276 / 280 mil req/s | 300 / 297 / 286 mil req/s | 0 → 0 |
| 3000 | 247 / 246 / 250 mil req/s | 260 / 262 / 259 mil req/s | 1.143 / 1.118 / 1.607 → **0** |

Observações: o *lingering close* continua valendo (evita o RST que apagaria um 400/408/431/503 de um cliente ainda
enviando; há teste), mas **não** era a causa destes erros. A diferença entre rodadas isoladas chega a ±10%: compare
sempre rodadas alternadas, nunca uma única medição.

## Arquivos estáticos em streaming (perda da Fase 7 recuperada)

A Fase 7 deixou o `serve_static` ~2× mais lento (24,5 mil req/s contra 48,7 mil do thread pool): ele lia o arquivo
inteiro para a memória, e agora isso roda na thread única do loop. A correção tem quatro partes:

1. **Streaming:** arquivos acima de 64 KB não são lidos para a memória; a `Connection` os envia em pedaços de 64 KB com
   `pread`, voltando ao loop quando o socket enche. Memória constante por conexão e **fim do limite de 10 MB**.
2. **Arquivos pequenos (≤ 64 KB)** vão numa única leitura para a memória e o descritor fecha na hora.
3. **Abertura segura:** o arquivo é aberto e o caminho é conferido **no descritor** (`F_GETPATH`/`/proc/self/fd`), o que fecha
   a janela TOCTOU. Como essa checagem é autoritativa, o caminho passou a ser montado só por texto (`..` recusado, nenhuma
   chamada de sistema) e a raiz é canonicalizada **uma vez** por servidor (`StaticRoot`), em vez de duas vezes por requisição.
4. `O_NONBLOCK` na abertura: um FIFO dentro de `www/` travaria o `open()` para sempre, e com uma thread só, todos os clientes.

Medição (`wrk -t4`, Release, rodadas alternadas; `index.html` de ~0,5 KB e um arquivo aleatório de 5 MB):

| Cenário | Fase 8 (`main`) | Esta versão |
|---|---|---|
| `index.html`, 64 conexões | 25.250 req/s (12,6 MB/s) | **57.900 req/s** (28,8 MB/s), +130% |
| `index.html`, 1000 conexões | 24.700 req/s, 26.363 erros de leitura | **56.200 req/s**, ~1.000 erros de leitura |
| 5 MB, 16 conexões | 61 req/s (**312 MB/s**) | **1.640 req/s** (**8 GB/s**), 26× |
| arquivo de 25 MB | 500 (limite de 10 MB) | serve, íntegro, em 151 ms (teste) |

O ganho do arquivo de 5 MB vem de o `main` ler o arquivo com um `istreambuf_iterator` (byte a byte) para dentro de uma
`std::string`. Já o ganho do arquivo pequeno vem de eliminar ~20 chamadas de sistema de canonicalização por requisição. O
resultado supera até o thread pool original (48,7 mil req/s).

**Limite conhecido:** com 1000 clientes abrindo a conexão **no mesmo instante** ainda aparecem algumas centenas de erros de
leitura (0,03–0,1%). Não vêm do servidor: a fila de escuta do macOS tem 128 posições (`kern.ipc.somaxconn`) e a rajada
inicial a excede. Testar menos eventos por volta do loop (32 em vez de 256) não mudou o resultado.
