# 0007. Fechamento "lingering", backoff do `accept` e fila de prazos

- Status: Aceita
- Data: 2026-11-03

## Contexto

Três fragilidades do `Server` ficaram registradas como dívida depois do event loop (0003, 0005):

1. Responder 400/408/431/503 e fechar logo podia gerar um **RST** (o cliente ainda enviava dados), que pode apagar a
   resposta antes de o cliente lê-la.
2. Um erro de `accept` como `EMFILE` (sem descritores) dormia 10 ms **com o loop bloqueado** e tentava de novo, atrasando
   todos os clientes e enchendo o log.
3. Os prazos eram conferidos varrendo **todas** as conexões a cada 50 ms: custo O(conexões) e precisão de ±50 ms.

## Decisão

- **Lingering close** (RFC 9112 §9.6). Depois da última resposta (`Connection: close`, erro, limite de requisições) a
  `Connection` envia o FIN (`shutdown(SHUT_WR)`) e continua lendo e descartando até o cliente fechar, até 2 s, ou até 1 MiB
  descartado. Um fechamento por ociosidade (nada a proteger) continua imediato. Na parada do servidor, conexões em lingering
  fecham na hora.
- **Backoff do accept.** Num erro de recursos o servidor **tira o socket de escuta do `Poller`** por um tempo que dobra
  (10 ms → 1 s) e volta sozinho; as conexões abertas seguem normais. O `Listener` classifica o resultado do `accept`
  (`ACCEPTED`, `EMPTY`, `RETRY`, `EXHAUSTED`, `FAILED`) para o loop decidir sem olhar `errno`.
- **Fila de prazos.** Um *min-heap* com no máximo uma entrada viva por conexão. Quando o prazo apenas é **adiado** (o caso
  comum), a entrada antiga dispara, vê que ainda não venceu e reagenda: ~1 operação de heap por conexão a cada período de
  timeout, não por requisição. O `poll` espera até o próximo prazo (no máximo 50 ms, para notar `request_stop()`).

## Alternativas consideradas

- **Fechar sempre de imediato.** É o que gerava o RST. Para 408 em cliente parado o risco é pequeno, mas a regra única é
  mais simples de raciocinar e testar.
- **Dormir mais no erro do `accept`.** Continua travando o loop; vigiar o socket sem aceitar gira em 100% de CPU (o aviso
  por nível não some).
- **Timer wheel.** Mais preciso por conexão, mais código. O heap com reagendamento preguiçoso dá a precisão necessária
  (~1 ms) com pouco código.

## Consequências

- Uma conexão em lingering ainda ocupa uma vaga de `max_connections` e um descritor até o cliente fechar ou o prazo acabar
  (`Server::set_linger_timeout`).
- No macOS o kernel já descarta a conexão pendente quando o `accept` falha com `EMFILE` (no Linux ela espera na fila), então
  o teste só exige que o servidor volte a aceitar *novos* clientes.
- Entradas do heap de conexões já fechadas ficam até vencer (≤ ~30 s): memória limitada, ignoradas ao disparar.
- A varredura periódica saiu; o loop acorda no prazo certo em vez de a cada 50 ms.
