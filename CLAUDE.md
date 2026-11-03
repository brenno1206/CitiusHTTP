# CLAUDE.md — CitiusHTTP

Servidor HTTP/1.1 em C++20, escrito do zero (sem frameworks), para fins de aprendizado e portfólio, com arquitetura e práticas profissionais. Contexto, glossário e roadmap completos: @README.md

## Como você deve se comportar

O dono do projeto é estudante de Ciência da Computação e **está aprendendo programação de redes e C++ moderno**. Portanto:

- Responda e comente em **português do Brasil**. Identificadores de código em inglês; comentários no código em português, no estilo já usado.
- **Explique o porquê** antes do como: conceito, depois decisão, depois código. Use analogias curtas quando ajudar.
- Trabalhe em **passos pequenos**: um item por vez, compilando entre eles. Não implemente uma fase inteira de uma vez.
- Para qualquer tarefa não trivial, **proponha um plano e espere aprovação** antes de editar arquivos.
- **Não reescreva arquivos inteiros** nem renomeie/mova coisas sem avisar. Mostre o que muda e por quê.
- Quando houver mais de um caminho razoável, apresente 2 alternativas com prós e contras e recomende uma.
- Ao encontrar um bug ou ponto fraco no código existente, **aponte e explique**; corrija só quando for pedido ou fizer parte da tarefa.
- Não invente APIs. Se não tiver certeza de uma função POSIX ou da biblioteca padrão, diga e consulte a documentação.
- Ao concluir uma tarefa, diga em 2–3 linhas o que mudou e qual o próximo passo natural. Sem recapitular cada passo.

## Stack e plataforma

- **Linguagem:** C++20 (`-std=c++20`, sem extensões GNU). Não usar recursos C++23 (ex.: `std::expected`).
- **Build:** CMake ≥ 3.20.
- **Plataforma de desenvolvimento:** macOS com Apple Clang. O código deve continuar portável para Linux (gcc/clang); isole o que for específico de SO em `net/`.
- **Rede:** sockets POSIX (`<sys/socket.h>`, `<netinet/in.h>`, `<unistd.h>`). IPv4 por enquanto.
- **Dependências externas:** nenhuma no código de produção. Única exceção permitida: **GoogleTest** (via `FetchContent`) para testes. Pedir aprovação antes de adicionar qualquer outra.

## Comandos

```bash
cmake -S . -B build                          # configurar
cmake --build build                          # compilar
./build/citius_server                        # executar (porta 8080); --help lista as opções
./build/citius_server --port 9090 --log-level debug   # exemplo com opções
curl -v http://localhost:8080/               # teste manual
ctest --test-dir build --output-on-failure   # testes
cmake -S . -B build-asan -DCITIUS_SANITIZE=ON && cmake --build build-asan && ctest --test-dir build-asan --output-on-failure   # testes com ASan+UBSan
cmake -S . -B build-tsan -DCITIUS_TSAN=ON && cmake --build build-tsan && ctest --test-dir build-tsan --output-on-failure      # testes com ThreadSanitizer (não combina com ASan)
wrk -t4 -c1000 -d10s http://localhost:8099/ping   # benchmark (compile com -DCMAKE_BUILD_TYPE=Release; ver docs/benchmarks.md)
clang-format -i $(git ls-files 'src/*.cpp' 'include/*.hpp' 'tests/*.cpp')   # formatar (versão do CI: pip install clang-format==23.1.3)
cmake -S . -B build-tidy && git ls-files -co --exclude-standard 'src/*.cpp' | grep -v PollerEpoll | xargs clang-tidy -p build-tidy   # análise estática (clang-tidy 23.1.4; no macOS: export SDKROOT=$(xcrun --show-sdk-path); PollerEpoll só compila no Linux; `-co --exclude-standard` inclui arquivos novos ainda não rastreados)
cmake -S . -B build-ci -DCITIUS_WERROR=ON && cmake --build build-ci   # warnings como erro, como no CI
scripts/check-layers.sh                      # regra de camadas (quem pode incluir quem; sem socket em http/parse/routing/files/util)
```

`build/` é ignorado pelo git. Nunca commitar artefatos de build.

**Armadilha de ambiente (macOS):** se as Command Line Tools forem atualizadas além do Xcode, a linkagem falha com `tapi error: malformed file ... unknown architecture` (o `ld` do Xcode não lê o SDK mais novo das CLT). Solução: `export SDKROOT=/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk` (idealmente no `~/.zshrc`) e reconfigurar com um diretório de build novo. Remover quando o Xcode for atualizado.

## Arquitetura

Grupos em `include/citius/<grupo>/`, `src/<grupo>/` e `tests/unit/<grupo>/`. **Cada grupo só depende dos de cima da lista** (verificado por `scripts/check-layers.sh`, que roda no CI):

```
util/     Logger, RotatingFile, FileDescriptor, AsciiText        transversal (depende de nada)
net/      Socket (RAII), Listener, Poller (kqueue/epoll)         POSIX (depende de util)
http/     HttpRequest, HttpResponse, HttpSyntax, StatusCode      tipos do protocolo (util)
parse/    HttpParser, RequestAssembler, ChunkedDecoder           texto vira requisição (http, util)
routing/  Router                                                 (http, util)
files/    StaticFiles (StaticRoot), StaticOpen, StaticPath, Mime arquivos estáticos (http, util)
server/   Server, Connection, Config, ThreadPool                 orquestração (todos)
main.cpp
```

Regra de ouro: **`util/`, `http/`, `parse/`, `routing/` e `files/` nunca incluem headers de socket** (`<sys/socket.h>`, `<netinet/*>`, `<poll.h>`, `<sys/event.h>`...). O parser recebe `std::string_view` e devolve estruturas; isso o torna testável sem rede. O que é específico de SO fica em `net/` (exceção justificada: `files/StaticOpen.cpp` usa `F_GETPATH` / `/proc/self/fd` para conferir o caminho real de um descritor).

O CMake tem a biblioteca `citius_core` (nasce vazia; cada `src/<grupo>/CMakeLists.txt` lista os seus fontes e `src/net/` escolhe o `Poller` do SO), o executável `citius_server` (só `main`) e o executável `citius_tests` (cada `tests/unit/<grupo>/CMakeLists.txt` lista os seus testes). Usar apenas comandos por target (`target_*`), nunca flags globais. Arquivo novo = uma linha no `CMakeLists.txt` da pasta dele.

## Convenções de código

Siga o estilo já existente no repositório:

- Classes e structs: `PascalCase`. Arquivos: mesmo nome da classe (`HttpParser.hpp/.cpp`).
- Funções, métodos, variáveis: `snake_case`. Membros privados: prefixo `m_` (`m_server_fd`).
- Valores de `enum class`: `MAIÚSCULAS` (`HttpMethod::GET`). Constantes: `kPascalCase`.
- Headers com `#pragma once`. Cada header inclui o que usa (não depender de includes transitivos).
- Tudo dentro do namespace `citius`.
- Warnings ligados: `-Wall -Wextra -Wpedantic`. **Código novo não pode gerar warnings.**

### Comentários e documentação

- **Todo item público de header** (classe, função, enum, campo não óbvio) leva comentário de documentação `///` em português, no header: o que faz, `@param` / `@return` quando não for óbvio, `@throws` se lançar, e regras de posse/vida (ex.: "copia os dados", "não guarde o `string_view`").
- **Nos `.cpp` só comentários de porquê** (decisão, armadilha de SO, invariante). Não repetir o que o código já diz nem o que a doc do header já diz.
- Comentários desatualizados são bug: ao mudar o código, atualize o comentário no mesmo commit.

### Regras de C++ moderno (obrigatórias)

1. **RAII para todo recurso.** Socket, arquivo, thread: um objeto dono que libera no destrutor. Proibido `close()` espalhado pelo código.
2. **Sem `new`/`delete`** nem `malloc`/`free`. Usar contêineres e `std::unique_ptr`.
3. Tipos que possuem recurso são **move-only** (copy deletado).
4. `const` por padrão; `[[nodiscard]]` em funções cujo retorno não pode ser ignorado; `explicit` em construtores de um argumento.
5. Receber texto somente leitura como `std::string_view`; nunca guardar um `string_view` além da vida do dono dos dados.
6. **Erros:** exceções só para falhas de inicialização/ambiente (porta ocupada). Entrada inválida de cliente é rotina, **não** exceção: retornar `std::optional`/tipo de resultado com o motivo do erro.
7. Não usar `using namespace std;` em headers.
8. Preferir `<algorithm>`/`<ranges>` a laços manuais quando ficar mais claro. Clareza vence esperteza.

## Rede: regras que não podem ser quebradas

- TCP é fluxo de bytes: **`recv` pode devolver parte da requisição e `send` pode enviar só parte.** Sempre em laço; tratar `EINTR`.
- Nunca imprimir/tratar o buffer de `recv` como string C terminada em `\0`; usar o `ssize_t` retornado.
- Definir `SO_REUSEADDR` no socket de escuta.
- **macOS não tem `MSG_NOSIGNAL`**: evitar `SIGPIPE` com `SO_NOSIGPIPE` no socket (ou ignorar o sinal). Em Linux usar `MSG_NOSIGNAL`. Encapsular isso em `net/`.
- Todo input do cliente é hostil: limites de tamanho para linha de requisição, cabeçalhos e corpo; timeouts de leitura.
- Arquivos estáticos: normalizar o caminho (`std::filesystem::weakly_canonical`) e **garantir que fica dentro de `www/`**. Rejeitar `..`, caminhos absolutos e bytes nulos.
- Respostas sempre com `Content-Length` correto (ou `Transfer-Encoding`) e códigos de status adequados (400, 404, 405, 413, 431, 505...). Seguir RFC 9110/9112.

## Estado atual

**Protocolo (`http/`, `parse/`).** `HttpParser::parse` interpreta só a cabeça e devolve `ParseResult` (requisição ou `ParseError` → 400/414/417/431/501/505; nomes de cabeçalho em minúsculas, repetidos juntados com `", "`; valida versão, alvo e `Host`). `Content-Length` aceita **só dígitos, uma vez**; acima de `HttpParser::kMaxBodyLength` (1 MB) é 413 sem ler o corpo. `Transfer-Encoding: chunked` é decodificado (`ChunkedDecoder`, estrito: hex puro, CRLF obrigatório, trailers descartados, 1 KB por linha, 8 KB de trailers, 1 MB de corpo); **qualquer ambiguidade é 400** (junto com `Content-Length`, em HTTP/1.0, `chunked` repetido ou fora do fim), outras codificações são 501. `Expect: 100-continue` recebe o interino `HTTP/1.1 100 Continue` só se a cabeça foi aceita e o corpo ainda não chegou; outro `Expect` é 417 (ignorado em HTTP/1.0). `RequestAssembler` monta a requisição aos poucos (cabeça até 8 KB, corpo com tamanho fixo ou chunked, pipelining: `feed("")` entrega a próxima sem ler nada). `HttpResponse`: `serialize()` calcula o `Content-Length`; `set_header` recusa CR/LF/NUL e `Content-Length`; `set_file_body` usa um descritor como corpo (`serialize()` devolve só a cabeça). O parser nunca devolve 405 (depende do recurso: é do `Router`); método desconhecido é 501. `Router`: casamento exato (query ignorada, sem parâmetros tipo `/users/:id`); caminho existente com outro método dá 405 com `Allow` (o fallback não é consultado); sem rota vai para `set_fallback` ou 404; rota inválida ou handler vazio lança `std::invalid_argument`.

**Conexões e event loop (`server/`, `net/`).** `Server` atende todos os clientes numa thread. `Listener` (socket de escuta não bloqueante; `accept()` classificado em `ACCEPTED/EMPTY/RETRY/EXHAUSTED/FAILED`) → `Poller` (por nível; `kqueue` no macOS, `epoll` no Linux) → uma `Connection` por cliente, máquina de estados não bloqueante `READING → WRITING → LINGERING → CLOSED` (o tempo entra como parâmetro `now`, então os testes simulam prazos sem dormir). `Server::event_loop` é a classe local `EventLoop` (`run()` e `serve_one()` são variações): aceita até 1024 clientes por aviso (a fila do kernel é curta: 128 no macOS), `503` com `Retry-After` acima de `max_connections` (padrão 4096), **backoff do `accept`** em `EMFILE` (10 ms → 1 s, tirando o socket de escuta do `Poller`), **fila de prazos** (min-heap, uma entrada viva por conexão, reagendamento preguiçoso; o `poll` espera até o próximo prazo, no máximo 50 ms para notar `request_stop()`, que só grava um `atomic<bool>` e é seguro em handler de sinal). Prazos por conexão: leitura = **prazo total da requisição** (padrão 10 s; um slowloris não renova; estourou → `408`), ociosidade entre requisições (5 s, fecha em silêncio), escrita (30 s **mais** o tempo a 16 KB/s para o tamanho da resposta). Depois de uma resposta que fecha a conexão há **lingering close**: FIN (`shutdown(SHUT_WR)`) e descarte até o cliente fechar (2 s, 1 MiB; `set_linger_timeout`). Keep-alive: HTTP/1.1 persistente salvo `Connection: close`; qualquer erro fecha; `kMaxRequestsPerConnection` = **1000** (configurável: `--max-requests`; 100 gerava rajadas de reconexão que estouravam o backlog, ADR 0010). Contrapressão: enquanto uma resposta não sai, a próxima requisição não é lida. Handler que lança vira 500 dentro da `Connection` (sem vazar a mensagem). `main.cpp` liga `request_stop()` a SIGINT/SIGTERM (`SA_RESETHAND`: o segundo Ctrl+C mata na hora) e **sobe o limite de descritores** (`raise_fd_limit`; o padrão do macOS é 256). Encerramento gracioso: para de aceitar, fecha conexões ociosas e em lingering, espera as requisições em andamento (cada uma com seu prazo). `Server(port, root, max_connections)`; `serve_one()` atende um cliente até a conexão fechar (é o que a maioria dos testes de integração usa). O `ThreadPool` continua na biblioteca, mas o `Server` não o usa.

**Arquivos estáticos (`files/`).** `decode_target_path` (query, `%XX` uma vez só) → `lexical_static_path` (só texto: recusa `..`/NUL, ignora `//` e `.`) → `open_static_file`: abre com `O_NONBLOCK` (um FIFO não trava o loop) e confere o **caminho real do descritor** contra a raiz canônica (`StaticRoot`, calculada uma vez por servidor), por componentes; o que não for arquivo comum, estiver fora da raiz ou não existir dá **404** (não revela o que existe). Arquivos de até 64 KB vão em memória; maiores em **streaming** (a `Connection` lê 64 KB por vez com `pread`; sem limite de tamanho; se o arquivo encolher no meio, a conexão é encerrada). Respostas: 200/400/403/404/405 (`Allow: GET`)/503 (`Retry-After`)/500. `mime_type` em `Mime`. A raiz (`Server(port, root = "www")`) é relativa ao diretório de trabalho: iniciar o servidor na raiz do projeto.

**Log e configuração.** `Logger` (`log_debug/info/warn/error`): linha `2026-11-03 14:20:01 [INFO] msg`; sem arquivo, DEBUG/INFO em stdout e WARN/ERROR em stderr; com `log_to_file` (`RotatingFile`, rotação por tamanho `app.log` → `.1` → `.2`) **todas** vão para o arquivo e, se a escrita falhar, a linha cai no stderr (nunca se perde em silêncio). Thread-safe (mutex na escrita; `std::cout` concorrente é corrida de dados, achada pelo TSan). Caracteres de controle viram `?` (evita log injection): por isso mensagens de exceção **não terminam em `\n`**. Níveis: ERROR = falha de sistema/handler que lançou; WARN = tolerado mas estranho (limite de conexões → 503); INFO = ciclo de vida e requisições rejeitadas; DEBUG = rotina por requisição. Teste: `Logger(out, err, nivel)` com `ostringstream`. `parse_config(args)` devolve `ConfigResult{OK/HELP/INVALID, config, error}` sem exceção; aceita `--opcao valor` e `--opcao=valor`; opções: `--port --root --max-connections --max-requests --read-timeout-ms --idle-timeout-ms --log-level --log-file --log-max-bytes --log-keep --config`. `--config ARQUIVO` lê linhas `chave = valor` (`#` no início da linha é comentário, até 64 KB, erros citam `arquivo:linha`); precedência **padrões < arquivo < linha de comando**, qualquer que seja a ordem. `main.cpp`: `--help` (stdout) sai com 0, argumento inválido com 2 (via `log_error`), falha de execução (porta ocupada, log não abre) com 1; `std::cout`/`std::cerr` só no `--help` e na rede de segurança do `main`.

**Build, CI e qualidade.** CI em `.github/workflows/ci.yml` (todo push): `build-test` (macOS/Apple Clang, Ubuntu com gcc e com clang, `-DCITIUS_WERROR=ON`), `sanitizers` (ASan+UBSan e TSan), `format` (`clang-format==23.1.3`), `clang-tidy` (`==23.1.4`, em Ubuntu **e** macOS porque cada um compila um `Poller`; no macOS precisa de `SDKROOT=$(xcrun --show-sdk-path)`) e `camadas`. Use as **mesmas versões** localmente. `CMAKE_BUILD_TYPE` padrão Debug; o configure verifica `std::format` e diz o que fazer se faltar. `.clang-format`: 120 colunas, `if`/laço curtos numa linha. `.clang-tidy`: bugs (bugprone, analyzer, performance), `modernize`/`cppcoreguidelines` selecionados, `misc-const-correctness` e `misc-include-cleaner`, com `WarningsAsErrors: '*'`: **zero achados é o padrão**; exceção só com `NOLINT` e o motivo na linha, e includes que só valem numa plataforma levam `// IWYU pragma: keep`. Os testes de integração usam um diretório temporário por processo (o `ctest --parallel` roda vários ao mesmo tempo). Decisão nova importante ganha um ADR novo (não se reescreve um aceito).

## Limites conhecidos (nenhuma dívida aberta)

1. O disco frio é lido na thread do loop: um download de arquivo que não está no cache do SO pode atrasar os outros. Caminhos: `ThreadPool` (já existe, sem uso no `Server`) ou `sendfile`.
2. Sem `Range`, `ETag` nem `If-Modified-Since` nos arquivos estáticos; respostas sempre com `Content-Length` (sem `chunked` de saída).
3. Com ~1000 clientes abrindo a conexão no mesmo instante ainda aparecem algumas centenas de erros no `wrk` (0,03–0,1%): a rajada inicial excede a fila de escuta do kernel (128 no macOS), que a aplicação não controla (`docs/benchmarks.md`).
4. Entradas da fila de prazos de conexões já fechadas ficam até vencer (≤ ~30 s): memória limitada, ignoradas ao disparar.
5. Log: rotação só por tamanho e sem reabrir por `SIGHUP`; a configuração é lida só na inicialização.
6. O `100 Continue` é uma única tentativa de `send` (25 bytes); se falhar, o cliente espera ~1 s e envia o corpo mesmo assim.

## Testes

- Framework: GoogleTest. Estrutura: `tests/unit/<grupo>/` (espelha `src/`; sem rede, com `socketpair` quando precisa de um socket) e `tests/integration/` (sobe o servidor numa porta efêmera e conecta de verdade).
- **Escrever o teste primeiro** para parser e router; cobrir casos válidos, inválidos, vazios, gigantes e maliciosos.
- Todo bug corrigido ganha um teste que o reproduz.
- Antes de considerar pronto, rodar também com `-fsanitize=address,undefined` (e `thread` para código concorrente).

## Git

- Branch `main` estável. Trabalho em `feat/<assunto>` ou `fix/<assunto>`, uma por fase/tarefa.
- Commits no padrão **Conventional Commits** em português ou inglês, imperativo e curtos: `feat: adiciona classe Socket RAII`, `fix: corrige mapeamento do método DELETE`.
- Commits pequenos e atômicos (um motivo por commit). **Não commitar nem dar push sem o usuário pedir.**

## Definição de pronto (vale para toda tarefa)

- [ ] Compila sem warnings.
- [ ] `clang-format`, `clang-tidy` e `scripts/check-layers.sh` sem achados (o CI confere, em macOS e Ubuntu).
- [ ] Testes novos e antigos passam (`ctest`).
- [ ] Sem vazamento de recursos; nenhum erro dos sanitizers.
- [ ] Segue as convenções e regras de rede acima.
- [ ] Usuário entendeu o que foi feito (explicação dada).
- [ ] `README.md`/`CLAUDE.md` atualizados se uma decisão, comando ou estrutura mudou.

## Fora de escopo (por enquanto)

HTTPS/TLS, HTTP/2 e HTTP/3, WebSocket, compressão, IPv6 e suporte a Windows. Não introduzir sem o usuário pedir.

## Manutenção deste arquivo

Mantenha-o curto e verificável. Quando uma decisão importante for tomada ou uma armadilha for descoberta, **proponha** adicioná-la aqui. Remova itens resolvidos da lista de dívidas.
