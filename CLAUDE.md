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
./build/citius_server                        # executar (porta 8080)
curl -v http://localhost:8080/               # teste manual
ctest --test-dir build --output-on-failure   # testes
cmake -S . -B build-asan -DCITIUS_SANITIZE=ON && cmake --build build-asan && ctest --test-dir build-asan --output-on-failure   # testes com ASan+UBSan
cmake -S . -B build-tsan -DCITIUS_TSAN=ON && cmake --build build-tsan && ctest --test-dir build-tsan --output-on-failure      # testes com ThreadSanitizer (não combina com ASan)
clang-format -i src/*.cpp include/citius/**/*.hpp   # formatar
```

`build/` é ignorado pelo git. Nunca commitar artefatos de build.

**Armadilha de ambiente (macOS):** se as Command Line Tools forem atualizadas além do Xcode, a linkagem falha com `tapi error: malformed file ... unknown architecture` (o `ld` do Xcode não lê o SDK mais novo das CLT). Solução: `export SDKROOT=/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk` (idealmente no `~/.zshrc`) e reconfigurar com um diretório de build novo. Remover quando o Xcode for atualizado.

## Arquitetura

Camadas; **cada uma só depende das de baixo**:

```
main.cpp
core/  Server, ThreadPool, Config      orquestração
http/  Request, Response, Parser, Router, StatusCode, Mime   protocolo (NÃO chama sockets)
net/   Socket (RAII), Listener, Connection                  POSIX
util/  Logger                                               transversal
```

Estado atual: ainda é plano (`include/citius/*.hpp`, `src/*.cpp`). Migrar para subpastas de forma gradual, uma camada por commit (tarefa à parte, ainda pendente).

Regra de ouro: **`http/` nunca inclui headers de socket.** O parser recebe `std::string_view` e devolve estruturas; isso o torna testável sem rede.

O CMake já tem: biblioteca `citius_core` + executável `citius_server` (só `main`) + executável `citius_tests` linkado à biblioteca. Usar apenas comandos por target (`target_*`), nunca flags globais.

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

## Estado atual e dívidas conhecidas

Fases 0, 1, 2, 3, 4 e 5 concluídas. `Socket` RAII move-only (`SO_REUSEADDR`, `SO_NOSIGPIPE`/`MSG_NOSIGNAL`, `send_all`, `recv_some`); `read_request_head` (lê até `\r\n\r\n`, limite de 8 KB); `HttpParser::parse` devolve `ParseResult` (requisição ou `ParseError` → 400/414/431/501/505), lê cabeçalhos (nomes em minúsculas, repetidos juntados com `", "`) e valida versão, alvo e `Host`; `StatusCode`, `to_status(ParseError)` e `HttpResponse` (`serialize()` calcula o `Content-Length`; `set_header` recusa CR/LF/NUL e `Content-Length`; `make_error`); regras de sintaxe compartilhadas em `HttpSyntax.hpp`. `Server` lê → parseia → responde (arquivo de `www/` via `serve_static`, handler do `Router`, ou o status do erro) e `run()` atende **vários clientes ao mesmo tempo**: `accept_client()` espera com `poll` (100 ms, para notar `request_stop()`), entrega o `Socket` (num `shared_ptr`, porque `std::function` exige tarefa copiável) ao `ThreadPool` (`std::jthread` + mutex + condition variable, fila limitada) e `handle_client()` roda nas threads (só lê o estado do `Server`). Fila cheia dá `503` com `Retry-After`. `request_stop()` é seguro em handler de sinal (só grava um `atomic<bool>`); `main.cpp` o liga a SIGINT/SIGTERM via `sigaction` (`SA_RESETHAND`: um segundo Ctrl+C mata na hora). O pool é local a `run()`, então sair do laço espera os clientes em andamento (encerramento gracioso). `Server(port, root, threads = 0 → um por núcleo, mín. 2, max_queued = 128)`. `serve_one()` continua síncrono (é o que a maioria dos testes usa). Logs de várias threads passam por `log_info`/`log_error` (`Log.hpp`, com mutex): `std::cout <<` concorrente é corrida de dados (achada pelo TSan). **Fase 5 (HTTP robusto):** `handle_client` atende **várias requisições por conexão** (keep-alive, RFC 9112 §9.3). `HttpRequest::keep_alive()`: HTTP/1.1 é persistente salvo token `close` em `Connection`; HTTP/1.0 só com `keep-alive`; `close` vence. A resposta sempre diz `Connection: keep-alive` ou `close`. A conexão fecha por `close`, HTTP/1.0 sem `keep-alive`, **qualquer erro** (depois dele não se sabe onde a próxima requisição começa), ociosidade (`set_idle_timeout`, padrão 5 s, fecha em silêncio sem 408), `kMaxRequestsPerConnection` (100) ou `request_stop()` (a espera pela próxima requisição faz `poll` em fatias de 100 ms). Requisições encadeadas (pipelining): `read_request_head(socket, max, initial)` examina a sobra antes de ler do socket, e `read_request_body` devolve o excedente em `surplus`. Timeout de leitura: `Socket::set_recv_timeout` (`SO_RCVTIMEO`, por `recv`) + `ReadStatus::TIMEOUT` → `408` com `Connection: close` (`Server::set_read_timeout`, padrão 10 s; vale também para o corpo). Corpo: `HttpRequest::content_length` (parser) e `body` (preenchido pelo `Server` com `read_request_body`). `Content-Length` aceita **só dígitos, uma vez** (sinal, lista `5, 5`, repetido ou vazio = 400, para não haver duas leituras do tamanho: request smuggling); acima de `HttpParser::kMaxBodyLength` (1 MB) = `413` sem ler o corpo. `Transfer-Encoding`: 501 sozinho, **400 junto com `Content-Length`**. O limite de 8 KB da cabeça vale também quando ela chega inteira de uma vez. `listen` usa `SOMAXCONN` (com backlog 10, uma rajada de ~30 clientes perdia conexões no macOS e deixava um teste instável). Porta 0 = o SO escolhe (`Server::port()`), e `serve_one()` atende um cliente: é o que os testes de integração em `tests/integration/` usam (thread + cliente real). `Router` (`Router.hpp`): `get/post/put/del/add(path, handler)` com casamento exato (query ignorada ao casar, sem percent-decoding, sem parâmetros tipo `/users/:id`); caminho existente com outro método dá 405 com `Allow` (o fallback não é consultado); caminho sem rota vai para `set_fallback` ou 404; registrar rota inválida (sem `/` inicial, com `?`) ou handler vazio lança `std::invalid_argument`. `Server::router()` expõe o roteador (registrar antes de servir); o fallback é `serve_static`, e exceção de handler vira 500 (`route_safely`, sem vazar a mensagem). `/ping` é registrado em `main.cpp`. Decisão: o parser nunca devolve 405 (depende do recurso; é do `Router`); método desconhecido é 501. Arquivos estáticos: `decode_target_path` (query, `%XX` uma vez só) + `resolve_static_path` (recusa `..`/NUL, `weakly_canonical` + checagem por componentes dentro da raiz) em `StaticPath`; `serve_static` (`StaticFiles`) devolve 200/400/403/404/405(`Allow: GET`)/500 (arquivo > 10 MB, lido inteiro na memória); `mime_type` em `Mime`. Fora da raiz, diretório e inexistente dão todos 404 (não revela o que existe). A raiz (`Server(port, root = "www")`) é relativa ao diretório de trabalho: iniciar o servidor na raiz do projeto.

Dívidas (resolver uma por commit, com teste quando possível):

1. `Server`: o timeout de leitura é **por `recv`**, não um prazo total por requisição: um slowloris que manda 1 byte a cada poucos segundos ainda prende uma thread (e atrasa o encerramento gracioso, limitado pelo timeout); sem *lingering close* (cliente que ainda envia dados quando respondemos 431/503 pode receber RST em vez da resposta); se o cliente abortar entre o `poll` e o `accept`, o `accept` (bloqueante) pode esperar o próximo cliente e atrasar a parada (correção: socket de escuta não bloqueante, o que mexe no `Socket`); um erro persistente em `accept` só tem uma pausa de 10 ms (sem backoff).
2. `serve_static` tem uma janela TOCTOU entre checar o caminho e abrir o arquivo (aceitável: quem escreve em `www/` é confiável) e lê o arquivo inteiro (sem streaming; Fase 7).
3. `Log.hpp` (`log_info`/`log_error`, mutex global) é um paliativo; virar `Logger` com níveis na Fase 6. Só `main.cpp` ainda usa `std::cout`/`std::cerr` direto (roda numa thread só).
4. `Server::start` ainda cria o socket de escuta direto com `socket()`/`bind()`; mover para `net/` (`Listener`) na migração de pastas.
5. Cada conexão keep-alive **prende uma thread do pool** até fechar (até 5 s ociosa): com poucas threads, clientes parados ocupam vagas; o event loop da Fase 7 resolve. `Transfer-Encoding: chunked` não é suportado (501); `Expect: 100-continue` é ignorado.
6. `CMakeLists.txt`: fontes listadas à mão (sem `GLOB`/lista por diretório) e sem `CMAKE_BUILD_TYPE` padrão.
7. `std::format` na libc++ da Apple pode exigir deployment target recente (compila no Xcode 26.6; Linux não verificado).
8. Código ainda não passou pelo `clang-format` (arquivo `.clang-format` existe; formatar numa mudança só de formatação).

## Testes

- Framework: GoogleTest. Estrutura: `tests/unit/` (parser, router, utilitários — sem rede) e `tests/integration/` (sobe o servidor numa porta efêmera e conecta de verdade).
- **Escrever o teste primeiro** para parser e router; cobrir casos válidos, inválidos, vazios, gigantes e maliciosos.
- Todo bug corrigido ganha um teste que o reproduz.
- Antes de considerar pronto, rodar também com `-fsanitize=address,undefined` (e `thread` para código concorrente).

## Git

- Branch `main` estável. Trabalho em `feat/<assunto>` ou `fix/<assunto>`, uma por fase/tarefa.
- Commits no padrão **Conventional Commits** em português ou inglês, imperativo e curtos: `feat: adiciona classe Socket RAII`, `fix: corrige mapeamento do método DELETE`.
- Commits pequenos e atômicos (um motivo por commit). **Não commitar nem dar push sem o usuário pedir.**

## Definição de pronto (vale para toda tarefa)

- [ ] Compila sem warnings.
- [ ] Testes novos e antigos passam (`ctest`).
- [ ] Sem vazamento de recursos; nenhum erro dos sanitizers.
- [ ] Segue as convenções e regras de rede acima.
- [ ] Usuário entendeu o que foi feito (explicação dada).
- [ ] `README.md`/`CLAUDE.md` atualizados se uma decisão, comando ou estrutura mudou.

## Fora de escopo (por enquanto)

HTTPS/TLS, HTTP/2 e HTTP/3, WebSocket, compressão, IPv6 e suporte a Windows. Não introduzir sem o usuário pedir.

## Manutenção deste arquivo

Mantenha-o curto e verificável. Quando uma decisão importante for tomada ou uma armadilha for descoberta, **proponha** adicioná-la aqui. Remova itens resolvidos da lista de dívidas.
