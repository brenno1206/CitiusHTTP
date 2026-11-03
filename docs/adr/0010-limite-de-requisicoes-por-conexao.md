# 0010. Limite de requisições por conexão: 1000, configurável

- Status: Aceita
- Data: 2026-11-03

## Contexto

O servidor fecha a conexão keep-alive depois de N requisições, para nenhuma conexão viver para sempre. O padrão era 100.
Com 3000 clientes simultâneos o `wrk` via de 1.000 a 2.000 *erros de leitura* (≈0,1% das requisições), sem nenhuma anomalia
correspondente no log do servidor.

## Decisão

Investigada com experimentos controlados (mesma máquina, `wrk -t4 -c3000`, Release; tabela completa em
[`docs/benchmarks.md`](../benchmarks.md)):

| Experimento | Erros de leitura |
|---|---|
| Limite de 100 | 1.100–2.300 |
| *Lingering close* | igual (não era a causa) |
| Limite praticamente removido | **0** |
| Limite de 100, `accept` em lotes de 4096 | 560–980 |
| **Limite de 1000, `accept` em lotes de 1024** | **0** (3 rodadas) |

**Causa:** clientes que começam juntos chegam ao limite quase ao mesmo tempo, o servidor fecha 3000 conexões e o `wrk`
reconecta todas numa rajada que estoura a fila de escuta do kernel (128 no macOS, `kern.ipc.somaxconn`); o cliente que o
kernel considerou conectado leva RST. Reduzir a rajada resolve: o padrão passa a **1000** (o nginx moderno também usa 1000), o
lote de `accept` por aviso sobe de 64 para 1024, e o limite é configurável (`--max-requests`,
`Server::set_max_requests_per_connection`).

## Alternativas consideradas

- **Remover o limite.** Resolve, mas uma conexão que nunca fecha acumula estado (ex.: buffers retidos) sem teto.
- **Aumentar `somaxconn`.** Exige `sysctl` com root e não vale para quem roda o servidor.
- **Só aumentar o lote de `accept`.** Ajuda, mas não elimina: o limite do kernel continua sendo o gargalo.

## Consequências

- Zero erros de leitura em 3000 conexões e ~5% mais vazão (menos reconexões).
- Com 1000 clientes abrindo a conexão **no mesmo instante** ainda aparecem algumas centenas de erros: é a rajada inicial contra
  a fila de 128 do kernel, que a aplicação não controla.
- A lição de método: comparar sempre **rodadas alternadas**; uma única medição variou ±10% entre execuções.
