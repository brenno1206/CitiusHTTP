# 0005. Prazos por conexão, totais por requisição, com tempo injetado

- Status: Aceita
- Data: 2026-11-03

## Contexto

No servidor bloqueante o timeout era do `recv` (`SO_RCVTIMEO`): valia para *cada leitura*. Um cliente
que manda 1 byte a cada poucos segundos (*slowloris*) nunca estourava o prazo e prendia a conexão
para sempre. Além disso, testar um prazo de 10 s exigia esperar 10 s.

## Decisão

Cada `Connection` mantém **um prazo de cada vez**, conforme o estado:

- **Leitura:** prazo *total* da requisição (cabeça e corpo), contado desde o primeiro byte dela (ou
  desde a conexão, para a primeira). Estourou: `408` e fecha. Bytes novos não o renovam.
- **Ociosidade** entre requisições: fecha em silêncio (não há requisição a responder).
- **Escrita:** prazo para a resposta sair inteira; quem não lê é desconectado.

O tempo entra como parâmetro (`now`) nos métodos da `Connection` em vez de ser lido do relógio, e o
`Server` varre os prazos a cada 50 ms.

## Alternativas consideradas

- **`SO_RCVTIMEO` por socket.** É o que causava o slowloris e não existe para sockets não bloqueantes.
- **Timer por conexão (heap ou *timer wheel*).** Mais preciso, mais código. A varredura periódica custa
  O(conexões) a cada 50 ms, suficiente para milhares; é um próximo passo possível.
- **Ler o relógio dentro da `Connection`.** Os testes teriam que dormir.

## Consequências

- Um teste de integração reproduz o slowloris (1 byte a cada 100 ms, prazo de 300 ms) e confirma o `408`.
- Os testes unitários da `Connection` simulam minutos sem dormir.
- A precisão dos prazos é de ~50 ms (a granularidade da varredura).
