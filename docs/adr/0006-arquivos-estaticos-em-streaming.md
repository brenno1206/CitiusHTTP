# 0006. Arquivos estáticos em streaming, validados no descritor

- Status: Aceita
- Data: 2026-11-03

## Contexto

O `serve_static` tinha três problemas, os três agravados pelo event loop de uma thread só (ver 0003):

1. **Lia o arquivo inteiro para a memória** (limite de 10 MB, 500 acima disso), dentro do loop: um download grande
   segurava a memória e atrasava todos os clientes. A leitura ainda era byte a byte (`istreambuf_iterator`): 312 MB/s.
2. **Janela TOCTOU**: o caminho era conferido (`weakly_canonical`) e *depois* o arquivo era aberto. Quem controla um
   diretório dentro de `www/` poderia trocar um symlink nesse intervalo e ler um arquivo de fora.
3. **Lento para arquivos pequenos**: ~20 chamadas de sistema de canonicalização por requisição (25 mil req/s, metade do
   thread pool anterior).

## Decisão

- **Streaming.** `HttpResponse` ganha um *corpo de arquivo* (só o descritor e o tamanho). A `Connection` envia o arquivo em
  pedaços de 64 KB lidos com `pread` e volta ao loop quando o socket enche. Arquivos de até 64 KB continuam indo numa leitura
  só para a memória (o descritor fecha na hora).
- **A defesa é o descritor, não o nome.** `open_static_file` abre o arquivo e confere o **caminho real do descritor já
  aberto** (`F_GETPATH` no macOS, `/proc/self/fd/N` no Linux) contra a raiz canônica, comparando por componentes. Depois de
  aberto, o descritor não muda de objeto, então não há janela. O caminho em si é montado só por texto
  (`lexical_static_path`: recusa `..` e NUL, nenhuma chamada de sistema).
- **A raiz é canonicalizada uma vez** por servidor (`StaticRoot`). Se `www/` for trocada depois, os caminhos reais deixam de
  bater e tudo responde 404 (falha de forma segura).
- **`O_NONBLOCK` na abertura**: um FIFO dentro de `www/` travaria o `open()` para sempre, e com uma thread só, todos os
  clientes. O que não for arquivo comum (diretório, FIFO, dispositivo) dá 404.
- O prazo de escrita de uma resposta é `write_timeout` **mais** o tempo que o cliente levaria lendo a 16 KB/s: um download
  grande não é cortado por ser grande, mas quem lê mais devagar que isso (*slow read*) é desconectado.

## Alternativas consideradas

- **Entregar arquivos a um `ThreadPool` (opção B do 0003).** Tira o disco do loop, mas acrescenta threads e TSan em tudo, e
  não resolvia o TOCTOU nem a leitura de 312 MB/s. Continua possível se o disco frio virar problema.
- **`sendfile(2)`.** Zero-copy, mas a assinatura difere entre macOS e Linux e o envio parcial em socket não bloqueante é
  traiçoeiro; `pread` + `send` é portável e já satura (8 GB/s no teste de 5 MB). Fica como otimização.
- **Manter `O_NOFOLLOW`.** Foi testado e removido: recusaria também symlinks que apontam para *dentro* de `www/` (que
  funcionavam), e é redundante, já que o caminho real do descritor barra qualquer fuga.

## Consequências

- Arquivo pequeno: 25 mil → **58 mil req/s** (acima do thread pool, 48,7 mil). Arquivo de 5 MB: 312 MB/s → **8 GB/s**. Fim do
  limite de 10 MB (teste com 25 MB). Números em [`docs/benchmarks.md`](../benchmarks.md).
- Memória por download constante (um pedaço de 64 KB por conexão), qualquer que seja o arquivo.
- Se o arquivo **encolher** durante o envio, o `Content-Length` já enviado não pode ser cumprido: a conexão é encerrada (o
  cliente vê o corpo curto e sabe que está incompleto).
- Ainda não há `Range`, `ETag` nem `If-Modified-Since`, e o disco frio ainda é lido na thread do loop.
