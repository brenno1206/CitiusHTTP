# CitiusHTTP

[![CI](https://github.com/brenno1206/CitiusHTTP/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/brenno1206/CitiusHTTP/actions/workflows/ci.yml)

Servidor HTTP/1.1 escrito do zero em **C++20**, sem frameworks, usando apenas a biblioteca padrão e as APIs POSIX de sockets. O objetivo é duplo: ter um servidor funcional e, ao mesmo tempo, aprender (e demonstrar em portfólio) como software de rede de qualidade profissional é projetado.

> **Status:** em construção (Fases 0 a 8 concluídas, mais uma rodada de dívidas técnicas). Um event loop de uma thread (`kqueue`/`epoll`) atende todos os clientes, com keep-alive, prazos por conexão (leitura total, ociosidade, escrita), *lingering close*, backoff do `accept` e `503` acima de `--max-connections`. O HTTP é estrito: `Content-Length` e `Transfer-Encoding: chunked` (decodificado, sem brecha de *request smuggling*), `Expect: 100-continue`, limites de tamanho e os status 400, 408, 413, 414, 417, 431, 501 e 505. Serve arquivos de `www/` em streaming (sem limite de tamanho, com o caminho conferido no descritor contra path traversal e TOCTOU), tem `Router` com 405/`Allow`, log em arquivo com rotação e configuração por linha de comando ou arquivo. Código em grupos (`util`, `net`, `http`, `parse`, `routing`, `files`, `server`) com a regra de camadas verificada no CI, ~470 testes (unitários e de integração) e CI em macOS e Linux. Veja o [Roadmap](#6-roadmap-por-fases).

---

## Índice

1. [Glossário](#1-glossário)
2. [Como um servidor HTTP funciona](#2-como-um-servidor-http-funciona)
3. [Arquitetura](#3-arquitetura)
4. [Padrões e práticas profissionais aplicados](#4-padrões-e-práticas-profissionais-aplicados)
5. [Compilar, rodar e testar](#5-compilar-rodar-e-testar)
6. [Roadmap por fases](#6-roadmap-por-fases)
7. [Claude Code](#7-como-o-claude-code-funciona-neste-projeto-claudemd)
8. [Guia: como desenvolver este projeto com o Claude](#8-guia-como-desenvolver-este-projeto-com-o-claude)
9. [Referências](#9-referências)

---

## 1. Glossário

| Termo | Explicação curta |
|---|---|
| **HTTP** | Protocolo de texto que navegadores e servidores usam para conversar. O cliente manda uma *requisição*, o servidor devolve uma *resposta*. |
| **Socket** | "Tomada" do sistema operacional por onde bytes entram e saem pela rede. Em C/C++ é só um número inteiro (*file descriptor*). |
| **Porta** | Número (0–65535) que identifica *qual programa* da máquina recebe a conexão. Nosso servidor usa a 8080. |
| **TCP** | Protocolo abaixo do HTTP que garante que os bytes chegam inteiros e em ordem. HTTP "viaja" dentro de uma conexão TCP. |
| **bind / listen / accept** | As três chamadas que transformam um socket em servidor: *bind* reserva a porta, *listen* abre a fila de espera, *accept* pega o próximo cliente. |
| **Parser** | Código que transforma texto bruto (`"GET /index.html HTTP/1.1\r\n..."`) em uma estrutura organizada (`HttpRequest`). |
| **Router** | Decide *qual código* responde a cada caminho (`/`, `/api/x`, arquivo estático...). |
| **Handler** | Função que recebe um `HttpRequest` e devolve um `HttpResponse`. |
| **RAII** | Técnica central do C++ moderno: recursos (sockets, memória, arquivos) são liberados automaticamente pelo destrutor do objeto que os possui. Você não esquece de dar `close()`. |
| **Thread pool** | Grupo fixo de threads que processam conexões em paralelo, para atender vários clientes ao mesmo tempo. |
| **Keep-alive** | Reaproveitar a mesma conexão TCP para várias requisições, em vez de abrir uma nova a cada vez. |
| **Path traversal** | Ataque em que o cliente pede `/../../etc/passwd` para ler arquivos fora da pasta pública. Precisamos bloquear. |

---

## 2. Como um servidor HTTP funciona

```
Cliente (navegador / curl)
   │  1. abre conexão TCP na porta 8080
   ▼
[ Listener ]     socket() → bind() → listen() → accept()
   │  2. accept() devolve um socket novo só para este cliente
   ▼
[ Connection ]   recv(): lê os bytes brutos que o cliente enviou
   │
   ▼
[ HttpParser ]   "GET /style.css HTTP/1.1\r\nHost: ...\r\n\r\n"  →  HttpRequest
   │
   ▼
[ Router ]       escolhe quem responde: arquivo estático em www/? uma rota?
   │
   ▼
[ Handler ]      monta um HttpResponse (status, cabeçalhos, corpo)
   │
   ▼
[ Serializer ]   HttpResponse → bytes → send() → cliente
```

Uma requisição HTTP real é só texto:

```
GET /index.html HTTP/1.1\r\n        ← linha de requisição: método, caminho, versão
Host: localhost:8080\r\n            ← cabeçalhos (um por linha)
User-Agent: curl/8.0\r\n
\r\n                                ← linha vazia = fim dos cabeçalhos
```

E a resposta também:

```
HTTP/1.1 200 OK\r\n
Content-Type: text/html\r\n
Content-Length: 13\r\n
\r\n
Hello Citius!                       ← corpo
```

Tudo o que o projeto faz é construir, com qualidade, o caminho entre esses dois blocos de texto.

---

## 3. Arquitetura

O código é dividido em **camadas**. Cada camada só conhece a de baixo, nunca a de cima. Isso permite testar o parser sem abrir rede e trocar a estratégia de concorrência sem mexer no HTTP. A regra é **verificada**: `scripts/check-layers.sh` (roda no CI) falha se um grupo incluir um de cima, ou se `http/`, `parse/`, `routing/` ou `files/` incluírem um header de socket.

```
┌─────────────────────────────────────────────┐
│  main.cpp        (monta e liga as peças)    │
├─────────────────────────────────────────────┤
│  server/   Server, Connection, Config       │  orquestração
├─────────────────────────────────────────────┤
│  files/    StaticFiles, StaticOpen, Mime    │  regras do protocolo e dos
│  routing/  Router                           │  arquivos (nenhuma chamada
│  parse/    HttpParser, RequestAssembler,    │  de rede)
│            ChunkedDecoder                   │
│  http/     Request, Response, StatusCode    │
├─────────────────────────────────────────────┤
│  net/      Socket, Listener, Poller         │  sistema operacional / POSIX
├─────────────────────────────────────────────┤
│  util/     Logger, RotatingFile,            │  transversal
│            FileDescriptor, AsciiText        │
└─────────────────────────────────────────────┘
```

### Estrutura de pastas

Cada grupo tem a mesma pasta em `include/citius/`, `src/` e `tests/unit/`, e o seu próprio `CMakeLists.txt` com a lista dos arquivos.

```
CitiusHTTP/
├── CLAUDE.md                 instruções para o Claude Code
├── README.md                 este arquivo
├── CMakeLists.txt            build (biblioteca + executável + testes)
├── .clang-format             estilo de código automático
├── .clang-tidy               análise estática
├── .github/workflows/        CI (GitHub Actions)
├── scripts/                  check-layers.sh (regra de camadas)
├── include/citius/           headers públicos, um grupo por pasta
│   ├── util/                 Logger, RotatingFile, FileDescriptor, AsciiText
│   ├── net/                  Socket, Listener, Poller
│   ├── http/                 HttpRequest, HttpResponse, HttpSyntax, StatusCode
│   ├── parse/                HttpParser, RequestAssembler, ChunkedDecoder
│   ├── routing/              Router
│   ├── files/                StaticFiles, StaticOpen, StaticPath, Mime
│   └── server/               Server, Connection, Config, ThreadPool
├── src/                      implementações (.cpp) espelhando include/
│   └── main.cpp
├── tests/
│   ├── unit/<grupo>/         testes unitários (sem rede), espelhando src/
│   └── integration/          sobe o servidor numa porta efêmera e conecta de verdade
├── www/                      arquivos estáticos servidos (index.html, style.css)
└── docs/                     adr/ (decisões de arquitetura) e benchmarks.md
```

### Decisão de design importante: biblioteca + executável

O CMake gera uma **biblioteca** (`citius_core`) com quase todo o código e um executável fino (`citius_server`) que só chama `main()`. Os testes linkam contra a biblioteca. Assim o código é testável sem rodar o servidor.

---

## 4. Padrões e práticas profissionais aplicados

Esta é a lista do que o projeto vai usar, o que significa e por quê.

### 4.1 Código

| Prática | O que é | Por que importa | Onde aparece |
|---|---|---|---|
| **RAII** | Todo recurso tem um dono cujo destrutor o libera. | Sem vazamento de sockets, mesmo se der erro no meio. | Classe `Socket` (move-only, fecha o fd no destrutor) |
| **Regra do zero / cinco** | Preferir classes que não precisam de destrutor próprio; quando precisam, definir move e proibir cópia. | Evita *double close* e bugs sutis de cópia. | `Socket`, `ThreadPool` |
| **Sem `new`/`delete` manual** | Usar `std::unique_ptr`, `std::vector`, `std::string`. | Elimina quase todo vazamento de memória. | Projeto inteiro |
| **`const`, `[[nodiscard]]`, `enum class`** | Tipos que dizem a intenção e impedem mau uso. | O compilador pega erros por você. | `HttpMethod`, retornos do parser |
| **`std::string_view` e `std::span`** | Ver dados sem copiá-los. | Parser rápido, sem alocações desnecessárias. | `HttpParser::parse` |
| **`std::optional` / `std::variant`** | Representar "pode não existir" e "um destes tipos" no tipo. | Sem `nullptr` surpresa nem códigos mágicos. | Retorno do parser (evolui para carregar o motivo do erro) |
| **Tratamento de erro em dois níveis** | Exceções para falhas de *inicialização* (não deu para abrir a porta). Retorno tipado para falhas *esperadas* (requisição malformada do cliente). | Cliente mandar lixo não é exceção, é rotina. | `Server` vs `HttpParser` |
| **Namespace `citius`** | Tudo dentro de um namespace próprio. | Evita colisão de nomes. | Todos os headers |
| **Separação de responsabilidades** | Uma classe, um motivo para mudar. | Código testável e legível. | Camadas da seção 3 |
| **Concepts e `std::format` (C++20)** | Recursos modernos da linguagem onde fazem sentido. | Código mais seguro e expressivo. | Logger, Router |

### 4.2 Robustez de rede

| Prática | Por quê |
|---|---|
| **Laço de `recv`/`send`** | O TCP é um fluxo de bytes: uma requisição pode chegar em pedaços, e `send` pode enviar só parte. Um único `recv` não basta. |
| **`SO_REUSEADDR`** | Sem isso, reiniciar o servidor logo após fechá-lo falha com "Address already in use". |
| **Tratamento de `EINTR` e `SIGPIPE`** | Sinais podem interromper chamadas ou derrubar o processo se o cliente fechar a conexão no meio. |
| **Limites de tamanho** | Cabeçalhos e corpo têm teto máximo; sem isso um cliente malicioso esgota a memória. |
| **Timeouts** | Cliente lento ou parado não pode prender uma thread para sempre (ataque *slowloris*). |
| **Proteção contra path traversal** | Normalizar o caminho e garantir que ele fica dentro de `www/`. |
| **Respostas de erro corretas** | 400 (requisição inválida), 404, 405, 413, 431, 505 etc., conforme as RFCs. |
| **Encerramento gracioso** | `SIGINT`/`SIGTERM` param de aceitar clientes, terminam os em andamento e liberam tudo. |
| **Fechamento *lingering*** | Depois de uma resposta que fecha a conexão, o servidor manda o FIN e continua lendo até o cliente fechar: fechar de uma vez com dados por ler gera um RST que apaga a resposta. |
| **Backoff do `accept`** | Sem descritores (`EMFILE`), o servidor para de vigiar o socket de escuta por um tempo crescente em vez de travar ou girar em 100% de CPU. |
| **Request smuggling** | `chunked` estrito (CRLF obrigatório, hex puro, limites) e qualquer ambiguidade entre `Content-Length` e `Transfer-Encoding` é 400. |
| **Validação no descritor** | O arquivo é aberto e o caminho **real** do descritor é conferido contra `www/`: sem janela TOCTOU, e `O_NONBLOCK` evita travar num FIFO. |

### 4.3 Concorrência

Evolução planejada, de simples a avançado:

1. ~~**Sequencial**: um cliente por vez.~~ (`serve_one()` ainda existe, usado nos testes)
2. ~~**Thread pool** com fila de tarefas (`std::jthread`, `std::mutex`, `std::condition_variable`)~~ (Fase 4; o `ThreadPool` continua na biblioteca, mas o `Server` não o usa mais: com keep-alive, cada conexão aberta prendia uma thread até fechar).
3. **Event loop** com `kqueue` (macOS) / `epoll` (Linux) atrás de uma interface comum (`Poller`) ✅ (hoje). Uma thread atende milhares de conexões; com 3000 clientes simultâneos o thread pool respondia 503 a praticamente todos e o loop atende todos (ver [docs/benchmarks.md](docs/benchmarks.md)). Para um download grande não atrasar os demais, os arquivos vão em streaming (pedaços de 64 KB, [ADR 0006](docs/adr/0006-arquivos-estaticos-em-streaming.md)); um handler lento de verdade ainda atrasaria todos, e o caminho possível é entregá-los a um pool.

### 4.4 Engenharia e processo

| Prática | Ferramenta |
|---|---|
| **Build moderno** | CMake com *targets* (`target_*`), sem flags globais |
| **Warnings como disciplina** | `-Wall -Wextra -Wpedantic` (e `-Werror` no CI) |
| **Sanitizers** | AddressSanitizer + UndefinedBehaviorSanitizer em builds de teste; ThreadSanitizer para concorrência |
| **Testes automatizados** | GoogleTest: unitários (parser, router) e de integração (servidor real + cliente de teste) |
| **Formatação automática** | `clang-format` (versão fixada; o CI confere) ✅ |
| **Análise estática** | `clang-tidy`, checks focados em bugs; o CI exige zero achados ✅ |
| **Integração contínua** | GitHub Actions: macOS e Ubuntu (gcc e clang), ASan/UBSan, TSan, `-Werror`, `clang-format` e `clang-tidy` ✅ |
| **Commits padronizados** | Conventional Commits (`feat:`, `fix:`, `test:`, `docs:`, `refactor:`) |
| **Branch por fase** | Uma branch e um PR por etapa do roadmap |
| **Registro de decisões** | Arquivos curtos em [`docs/adr/`](docs/adr/README.md) explicando *por que* escolhemos algo ✅ |
| **Medição** | Benchmark com `wrk` antes e depois de otimizar, em [docs/benchmarks.md](docs/benchmarks.md) ✅ |

---

## 5. Compilar, rodar e testar

Pré-requisitos no macOS:

```bash
xcode-select --install     # compilador (Apple Clang) e ferramentas
brew install cmake         # sistema de build
```

Compilar e executar:

```bash
cmake -S . -B build        # configura (gera os arquivos de build)
cmake --build build        # compila
./build/citius_server      # roda, escuta na porta 8080
```

Em outro terminal:

```bash
curl -v http://localhost:8080/          # -v mostra a requisição e a resposta
```

Ou abra `http://localhost:8080` no navegador. Inicie o servidor **na raiz do projeto**: a pasta pública `www/` é relativa ao diretório de trabalho (de outro lugar, tudo responde 404). Para parar o servidor: `Ctrl+C`.

Opções de linha de comando (`./build/citius_server --help` mostra a lista completa):

```bash
./build/citius_server --port 9090 --root site --max-connections 1000 --log-level debug
```

| Opção | O que faz | Padrão |
|---|---|---|
| `--port N` | porta TCP (0 = o SO escolhe) | 8080 |
| `--root PASTA` | pasta dos arquivos públicos | `www` |
| `--max-connections N` | conexões abertas ao mesmo tempo; acima disso, 503 | 4096 |
| `--max-requests N` | requisições por conexão keep-alive antes de fechá-la | 1000 |
| `--read-timeout-ms N` | prazo para a requisição chegar inteira (não é por leitura); estourou, 408 | 10000 |
| `--idle-timeout-ms N` | espera da próxima requisição numa conexão keep-alive | 5000 |
| `--log-level NIVEL` | `debug`, `info`, `warn` ou `error` | `info` |
| `--log-file ARQUIVO` | escreve o log neste arquivo, com rotação, em vez de no console | console |
| `--log-max-bytes N` | tamanho a partir do qual o arquivo de log rotaciona (1024 a 1 GiB) | 10 MB |
| `--log-keep N` | arquivos de log antigos guardados (`app.log.1` ...) | 5 |
| `--config ARQUIVO` | lê as opções de um arquivo (veja abaixo); a linha de comando vence | — |

Em vez da linha de comando, as mesmas opções podem vir de um arquivo (`./build/citius_server --config citius.conf`). O formato é
`chave = valor` (o nome da opção sem o `--`), com linhas em branco e comentários `#` no início da linha. **Precedência: padrões < arquivo
< linha de comando**, qualquer que seja a ordem dos argumentos:

```
# citius.conf
port = 9090
root = site
max-connections = 2000
log-level = info
log-file = /var/log/citius.log
log-max-bytes = 5242880
log-keep = 3
```

Cada linha de log tem hora e nível (`2026-11-03 14:20:01 [INFO] ...`); com `--log-level debug` aparece também uma linha por requisição (`GET /ping -> 200`). Códigos de saída: 0 sucesso, 1 falha ao iniciar (ex.: porta ocupada), 2 argumento inválido.

Testes (GoogleTest, baixado automaticamente pelo CMake):

```bash
ctest --test-dir build --output-on-failure
```

Com sanitizers (AddressSanitizer + UndefinedBehaviorSanitizer):

```bash
cmake -S . -B build-asan -DCITIUS_SANITIZE=ON
cmake --build build-asan
ctest --test-dir build-asan --output-on-failure
```

Com ThreadSanitizer (detecta corridas de dados; não combina com o ASan, por isso é outro diretório de build):

```bash
cmake -S . -B build-tsan -DCITIUS_TSAN=ON
cmake --build build-tsan
ctest --test-dir build-tsan --output-on-failure
```

### Desempenho

Medido com `wrk` (Release, Apple M3), antes e depois do event loop da Fase 7. Método completo, outros
cenários e limitações em [docs/benchmarks.md](docs/benchmarks.md):

| `/ping` | Thread pool (Fase 4-6) | Event loop (Fase 7) |
|---|---|---|
| 64 conexões | 183.817 req/s | **253.989 req/s** |
| 1000 conexões | 84.303 req/s, 214 mil respostas 503 | **263.246 req/s**, nenhuma falha |
| 3000 conexões | 15.280 req/s, quase tudo 503 | **234.142 req/s**, nenhuma falha |

Na Fase 7 servir arquivo estático ficou ~2× mais lento (o disco passou a rodar na thread única do loop). Isso foi
corrigido com streaming e validação no descritor ([ADR 0006](docs/adr/0006-arquivos-estaticos-em-streaming.md)):

| Arquivo estático | Fase 8 | Hoje |
|---|---|---|
| `index.html` (~0,5 KB), 64 conexões | 25 mil req/s | **58 mil req/s** |
| arquivo de 5 MB, 16 conexões | 312 MB/s | **8 GB/s** |
| arquivo de 25 MB | `500` (limite de 10 MB) | servido, íntegro |

O raciocínio original está em [docs/adr/0003](docs/adr/0003-event-loop-em-vez-de-thread-pool.md).

### Qualidade e CI

Todo push roda o [CI](.github/workflows/ci.yml): compila com warnings como erro e testa em macOS (Apple
Clang) e Ubuntu (gcc e clang), roda ASan+UBSan e ThreadSanitizer, confere o `clang-format`, roda o
`clang-tidy` (zero achados, incluindo `misc-include-cleaner`) e confere a regra de camadas (`scripts/check-layers.sh`). Para repetir localmente, use as mesmas versões fixadas no CI:

```bash
pip install clang-format==23.1.3 clang-tidy==23.1.4        # ou: brew install llvm
clang-format -i $(git ls-files 'src/*.cpp' 'include/*.hpp' 'tests/*.cpp')

cmake -S . -B build-tidy                                    # gera compile_commands.json
export SDKROOT=$(xcrun --show-sdk-path)                     # só no macOS: o clang-tidy do pip precisa do SDK
git ls-files 'src/*.cpp' | grep -v PollerEpoll | xargs clang-tidy -p build-tidy   # PollerEpoll só compila no Linux

cmake -S . -B build-ci -DCITIUS_WERROR=ON                   # warnings como erro, como no CI
cmake --build build-ci
```

As decisões de arquitetura estão registradas em [docs/adr/](docs/adr/README.md).

---

## 6. Roadmap por fases

Cada fase termina com o projeto compilando, testado e commitado. **Não pule fases.**

| Fase | Objetivo | Conceitos que você aprende |
|---|---|---|
| **0. Base saudável** ✅ | Corrigir bugs do código atual, `CMakeLists` modernizado (biblioteca + executável), namespace, `.clang-format`, primeiro teste | CMake, namespaces, testes |
| **1. Núcleo de rede e parser** ✅ | Classe `Socket` RAII, `SO_REUSEADDR`, laço de leitura, parser completo da linha de requisição e cabeçalhos, `HttpResponse` com serialização | RAII, TCP como fluxo, `string_view`, parsing |
| **2. Arquivos estáticos** ✅ | Servir `www/`, tipos MIME, 404, proteção contra path traversal, `Content-Length` correto | Sistema de arquivos (`std::filesystem`), segurança |
| **3. Roteamento** ✅ | `Router` com handlers registráveis (`router.get("/ping", ...)`), 405 Method Not Allowed | `std::function`, design de API |
| **4. Concorrência** ✅ | Thread pool; tratamento de múltiplos clientes; encerramento gracioso | Threads, mutex, condition variable, sinais |
| **5. HTTP robusto** ✅ | Keep-alive, timeouts, limites de tamanho, corpo da requisição (POST) com `Content-Length`, códigos de erro corretos | RFC 9112, defesa contra abuso |
| **6. Observabilidade e configuração** ✅ | Logger com níveis, configuração por linha de comando/arquivo | Design de módulos transversais |
| **7. Alta performance** ✅ | Event loop com `kqueue`/`epoll`, benchmarks, perfil | I/O assíncrono, medição |
| **8. Acabamento profissional** ✅ | CI no GitHub Actions, sanitizers, clang-tidy, ADRs, README com resultados de benchmark | Engenharia de software |

---

## 7. Como o Claude Code funciona neste projeto (CLAUDE.md)

### O que é o `CLAUDE.md`

É um arquivo de texto na raiz do projeto que o **Claude Code lê automaticamente no início de cada sessão**. Ele funciona como a "memória permanente" do projeto: o Claude não lembra de conversas anteriores, mas sempre relê esse arquivo e portanto sempre sabe:

- o que o projeto é e qual a stack;
- como compilar e testar (os comandos exatos);
- quais convenções de código seguir;
- quais armadilhas conhecer;
- **como você quer que ele se comporte** (neste projeto: como mentor, explicando o porquê de cada passo).

### Como ele é carregado

- Dá para incluir outros arquivos com `@caminho/do/arquivo` dentro do `CLAUDE.md`.
- O comando `/memory` abre os arquivos de memória para edição, e `/init` gera um `CLAUDE.md` inicial analisando o código (aqui já foi escrito à mão).

### Regras para mantê-lo bom

1. **Curto e objetivo.** Ele consome parte do contexto do Claude em toda sessão. Instruções vagas ou longas demais são ignoradas com mais facilidade.
2. **Comandos exatos.** "Compile com `cmake --build build`" funciona; "compile o projeto" não.
3. **Regras verificáveis.** "Use `snake_case` para funções" é seguível; "escreva código limpo" não.
4. **Vivo.** Quando você tomar uma decisão ou descobrir uma armadilha, peça: *"atualize o CLAUDE.md com isso"*. Remova o que ficou obsoleto.
5. **Não duplique o README.** O `CLAUDE.md` diz *como trabalhar*; o README explica *o que e por quê*.

### O que o `CLAUDE.md` deste projeto configura

- Papel do Claude: **mentor** que explica conceitos em português, trabalha em passos pequenos e não reescreve seus arquivos inteiros sem avisar.
- Stack: C++20, CMake, Apple Clang no macOS, POSIX sockets, sem bibliotecas externas além do GoogleTest.
- Comandos de build, execução e teste.
- Arquitetura em camadas e convenções de nome: classes `PascalCase`, funções `snake_case`, membros `m_`.
- Lista de bugs conhecidos no código atual.
- Regras de segurança de rede e a "definição de pronto" de cada tarefa.

---

## 8. Guia: como desenvolver este projeto com o Claude

### Preparação (uma vez)

1. Instale as ferramentas: `xcode-select --install` e `brew install cmake`.
2. Abra um terminal na pasta do projeto e inicie o Claude Code:
   ```bash
   cd ~/Desktop/CitiusHTTP
   claude
   ```
3. Confirme que o compile funciona antes de pedir qualquer coisa nova:
   ```bash
   cmake -S . -B build && cmake --build build
   ```
4. Faça um commit com o `CLAUDE.md` e o `README.md`.

### O ciclo de trabalho para cada fase

Repita este ciclo. Ele é a parte mais importante deste guia.

1. **Comece uma sessão limpa** (`/clear` ou um novo `claude`) para cada fase. O `CLAUDE.md` já dá o contexto.
2. **Peça o conceito antes do código.** Entender é o objetivo; o código é consequência.
3. **Peça um plano, sem código.** Use o modo de plano (alterne com `Shift+Tab`) ou escreva "só planeje, não altere arquivos". Leia, questione, ajuste.
4. **Implemente em passos pequenos**, um item do plano por vez. Prefira "implemente só o passo 1" a "faça tudo".
5. **Compile e teste a cada passo.** Peça ao Claude para rodar o build e os testes e mostrar a saída.
6. **Leia o que mudou** (`git diff`). Se não entendeu uma linha, pergunte: *"explique essa parte linha por linha"*.
7. **Commite** com mensagem no padrão (`feat: ...`) e siga para o próximo passo.
8. **Feche a fase:** *"atualize o CLAUDE.md e o README com o que mudou nesta fase"*.

### Prompts prontos, fase a fase

Copie e cole. Eles pressupõem que o Claude leu o `CLAUDE.md`.

**Antes de tudo — auditoria do que existe**
> Leia o código em `src/` e `include/` e liste todos os bugs e problemas que encontrar, ordenados por gravidade, explicando cada um de forma simples. Não corrija nada ainda.

**Fase 0 — Base saudável**
> Vamos fazer a Fase 0 do roadmap. Primeiro me explique o que é uma biblioteca estática no CMake e por que separamos `citius_core` do executável. Depois proponha um plano, sem alterar arquivos.

> Execute o passo 1 do plano: corrija os bugs apontados na auditoria, um por commit.

**Fase 1 — Núcleo de rede e parser**
> Explique o que é RAII usando o `close(m_server_fd)` do nosso `Server` como exemplo. Depois escreva a classe `Socket` move-only e mostre como o `Server` passa a usá-la.

> Explique por que um único `recv` não garante receber a requisição inteira. Implemente a leitura em laço até encontrar `\r\n\r\n`, com limite de tamanho.

> Termine o `HttpParser`: método, caminho, versão e cabeçalhos. Escreva os testes primeiro (casos válidos e inválidos) e depois a implementação.

**Fase 2 — Arquivos estáticos**
> Implemente servir arquivos de `www/` com `std::filesystem`. Antes, explique o ataque de path traversal e escreva um teste que tenta `GET /../CMakeLists.txt` e deve receber 403 ou 404.

**Fase 3 — Roteamento**
> Projete a API do `Router` para que eu possa escrever `router.get("/ping", handler)`. Mostre duas alternativas de design com prós e contras antes de implementar.

**Fase 4 — Concorrência**
> Explique o que é uma condition variable com uma analogia. Implemente um `ThreadPool` simples e rode o teste de integração com ThreadSanitizer ligado.

**Fase 5 — HTTP robusto**
> Adicione keep-alive e timeouts. Liste primeiro quais seções da RFC 9112 se aplicam, e quais casos de borda precisam de teste.

**Fase 7 — Performance**
> Rode um benchmark com `wrk` no estado atual e guarde o resultado em `docs/benchmarks.md`. Só depois proponha a migração para `kqueue` atrás de uma interface, para ainda funcionar em Linux com `epoll`.

**Fase 8 — Acabamento**
> Crie o workflow do GitHub Actions para compilar e testar em macOS e Ubuntu, com sanitizers. Explique cada bloco do arquivo YAML.

### Prompts de uso geral (servem sempre)

- *"Explique como se eu estivesse vendo isso pela primeira vez."*
- *"Quais são as alternativas e por que você escolheu esta?"*
- *"O que pode dar errado nesse código? Quais testes cobririam isso?"*
- *"Revise meu código desta fase como um revisor sênior de C++ faria."*
- *"Tente quebrar meu parser com entradas maliciosas."*
- *"Atualize o CLAUDE.md com a decisão que acabamos de tomar."*

### Armadilhas comuns ao trabalhar com IA

- **Pedir demais de uma vez.** O resultado fica grande, difícil de revisar e cheio de erros acumulados. Divida.
- **Aceitar sem entender.** Você é o dono do código e vai explicá-lo em entrevistas. Se algo não ficou claro, pergunte até ficar.
- **Pular os testes.** Em código de rede os bugs aparecem em casos de borda. Escreva o teste primeiro.
- **Deixar o `CLAUDE.md` envelhecer.** Se ele mentir sobre o projeto, o Claude trabalha com informação errada.
- **Fazer tudo numa sessão só.** Sessões muito longas perdem foco. Uma fase, uma sessão.

### Sugestão para aprender de verdade

Alterne: numa tarefa o Claude escreve e você revisa; na seguinte **você escreve e o Claude revisa**. O parser (Fase 1) e o `Router` (Fase 3) são ótimos candidatos para você escrever sozinho, pedindo ao Claude só testes e revisão.

---

## 9. Referências

- [RFC 9110 — HTTP Semantics](https://www.rfc-editor.org/rfc/rfc9110) e [RFC 9112 — HTTP/1.1](https://www.rfc-editor.org/rfc/rfc9112): a especificação oficial.
- [Beej's Guide to Network Programming](https://beej.us/guide/bgnet/): o melhor material introdutório sobre sockets.
- [C++ Core Guidelines](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines): boas práticas de C++ moderno.
- [cppreference.com](https://en.cppreference.com/): referência da linguagem e da biblioteca padrão.
- [Modern CMake](https://cliutils.gitlab.io/modern-cmake/): como escrever `CMakeLists.txt` do jeito atual.
- [Documentação do Claude Code](https://docs.claude.com/en/docs/claude-code/overview): memória (`CLAUDE.md`), comandos e modos.

---

## Licença

Veja o arquivo [LICENSE](LICENSE).
