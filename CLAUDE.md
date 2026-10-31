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

Estado atual: ainda é plano (`include/citius/*.hpp`, `src/*.cpp`). Migrar para subpastas de forma gradual, uma camada por commit, quando a fase pedir.

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

Fase 0 concluída. **Fase 1 em andamento** (branch `feat/fase-1-rede-parser`): já existem `Socket` RAII move-only (`SO_REUSEADDR`, `SO_NOSIGPIPE`/`MSG_NOSIGNAL`, `send_all`, `recv_some`), `read_request_head` (lê até `\r\n\r\n` com limite; `Server` responde 431 se passar de 8 KB) e `main` captura exceções. Faltam: parser v2 com motivo de erro e cabeçalhos (passo 4), `HttpResponse`/`StatusCode` (5) e ligar tudo no `Server` (6). `Server` ainda atende **um cliente por vez** e responde texto fixo 200; `HttpResponse.hpp` vazio; `www/` vazio; sem testes de integração com o `Server`.

Dívidas (resolver uma por commit, com teste quando possível):

1. `HttpParser`: não lê cabeçalhos, não valida a versão (505) nem o formato do caminho; `std::optional` não diz o motivo do erro (precisa distinguir 400/405/505). → passo 4.
2. `Server::run`: ignora o parser e responde sempre 200 (passo 6); sem timeout de leitura (Fase 5); laço infinito sem encerramento gracioso (Fase 4); `accept` com erro não distingue `EINTR`.
3. `Server::start` ainda cria o socket de escuta direto com `socket()`/`bind()`; mover para `net/` (`Listener`) na migração de pastas.
4. `CMakeLists.txt`: fontes listadas à mão (sem `GLOB`/lista por diretório) e sem `CMAKE_BUILD_TYPE` padrão.
5. `std::format` na libc++ da Apple pode exigir deployment target recente (compila no Xcode 26.6; Linux não verificado).
6. Código ainda não passou pelo `clang-format` (arquivo `.clang-format` existe; formatar numa mudança só de formatação).

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
