# 0004. `Poller` com `kqueue` (macOS) e `epoll` (Linux)

- Status: Aceita
- Data: 2026-11-03

## Contexto

O event loop (0003) precisa perguntar ao sistema quais descritores estão prontos. O projeto se
desenvolve no macOS e deve continuar portável para Linux, sem dependências externas no código de
produção.

## Decisão

Uma interface única, `Poller` (`add`, `modify`, `remove`, `wait`), com duas implementações escolhidas
pelo CMake: `PollerKqueue.cpp` no macOS e `PollerEpoll.cpp` no Linux. O aviso é **por nível**
(*level-triggered*): enquanto houver dados não lidos, todo `wait()` avisa de novo. O resto do código
não vê a diferença; um fd aparece no máximo uma vez por `wait()` (o `kqueue` gera um evento por
filtro, e o `Poller` os junta).

## Alternativas consideradas

- **`poll(2)`/`select(2)`.** Portáveis, mas O(n) a cada chamada: o custo cresce com o número de conexões.
- **libevent / libuv / Boost.Asio.** Resolvem tudo, mas violam a regra de não ter dependências e
  escondem justamente o que o projeto quer ensinar.
- **Disparo por borda (*edge-triggered*).** Menos chamadas, mas exige ler até `EAGAIN` em toda
  parte; um erro vira conexão parada para sempre. Por nível é mais simples e seguro.

## Consequências

- Duas implementações para manter. O CI compila e testa as duas (Ubuntu usa `epoll`, macOS `kqueue`),
  e o `clang-tidy` roda nos dois.
- Os testes do `Poller` usam `socketpair`, sem rede.
- Fechar um fd o remove do `kqueue`/`epoll` sozinho; por isso a `Connection` só precisa fechá-lo.
