# CitiusHTTP

Servidor HTTP/1.1 escrito do zero em **C++20**, sem frameworks, usando apenas a biblioteca padrão e as APIs POSIX de sockets. O objetivo é duplo: ter um servidor funcional e, ao mesmo tempo, aprender (e demonstrar em portfólio) como software de rede de qualidade profissional é projetado.

> **Status:** em construção (Fase 0 concluída). Hoje o servidor aceita uma conexão, lê a requisição, responde "Hello Citius!" e fecha. O parser entende a linha de requisição (ainda sem cabeçalhos) e tem testes unitários. Veja o [Roadmap](#6-roadmap-por-fases).

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

O código é dividido em **camadas**. Cada camada só conhece a de baixo, nunca a de cima. Isso permite testar o parser sem abrir rede e trocar a estratégia de concorrência sem mexer no HTTP.

```
┌─────────────────────────────────────────────┐
│  main.cpp        (monta e liga as peças)    │
├─────────────────────────────────────────────┤
│  core/   Server, ThreadPool, Config         │  orquestração
├─────────────────────────────────────────────┤
│  http/   Request, Response, Parser,         │  regras do protocolo
│          Router, StatusCode, Mime           │  (sem nenhuma chamada de rede)
├─────────────────────────────────────────────┤
│  net/    Socket (RAII), Listener, Connection│  sistema operacional / POSIX
├─────────────────────────────────────────────┤
│  util/   Logger                             │  transversal
└─────────────────────────────────────────────┘
```

### Estrutura de pastas alvo

```
CitiusHTTP/
├── CLAUDE.md                 instruções para o Claude Code
├── README.md                 este arquivo
├── CMakeLists.txt            build (biblioteca + executável + testes)
├── .clang-format             estilo de código automático
├── .clang-tidy               análise estática
├── include/citius/           headers públicos
│   ├── net/                  Socket.hpp, Listener.hpp, Connection.hpp
│   ├── http/                 HttpRequest.hpp, HttpResponse.hpp, HttpParser.hpp,
│   │                         Router.hpp, StatusCode.hpp, Mime.hpp
│   ├── core/                 Server.hpp, ThreadPool.hpp, Config.hpp
│   └── util/                 Logger.hpp
├── src/                      implementações (.cpp) espelhando include/
│   └── main.cpp
├── tests/                    testes unitários e de integração
├── www/                      arquivos estáticos servidos (index.html, style.css)
└── docs/                     decisões de arquitetura (ADRs), benchmarks
```

> O código atual ainda tem os headers soltos em `include/citius/`. A reorganização em subpastas acontece na Fase 1, de forma gradual.

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

### 4.3 Concorrência

Evolução planejada, de simples a avançado:

1. **Sequencial** (hoje): um cliente por vez.
2. **Thread pool** com fila de tarefas (`std::jthread`, `std::mutex`, `std::condition_variable`).
3. **Event loop** com `kqueue` (macOS) / `epoll` (Linux) atrás de uma interface comum, para milhares de conexões com poucas threads.

### 4.4 Engenharia e processo

| Prática | Ferramenta |
|---|---|
| **Build moderno** | CMake com *targets* (`target_*`), sem flags globais |
| **Warnings como disciplina** | `-Wall -Wextra -Wpedantic` (e `-Werror` no CI) |
| **Sanitizers** | AddressSanitizer + UndefinedBehaviorSanitizer em builds de teste; ThreadSanitizer para concorrência |
| **Testes automatizados** | GoogleTest: unitários (parser, router) e de integração (servidor real + cliente de teste) |
| **Formatação automática** | `clang-format` |
| **Análise estática** | `clang-tidy` |
| **Integração contínua** | GitHub Actions compilando e testando em macOS e Linux |
| **Commits padronizados** | Conventional Commits (`feat:`, `fix:`, `test:`, `docs:`, `refactor:`) |
| **Branch por fase** | Uma branch e um PR por etapa do roadmap |
| **Registro de decisões** | Arquivos curtos em `docs/` (ADRs) explicando *por que* escolhemos algo |
| **Medição** | Benchmark com `wrk` ou `ab` antes e depois de otimizar |

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

Ou abra `http://localhost:8080` no navegador. Para parar o servidor: `Ctrl+C`.

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

---

## 6. Roadmap por fases

Cada fase termina com o projeto compilando, testado e commitado. **Não pule fases.**

| Fase | Objetivo | Conceitos que você aprende |
|---|---|---|
| **0. Base saudável** ✅ | Corrigir bugs do código atual, `CMakeLists` modernizado (biblioteca + executável), namespace, `.clang-format`, primeiro teste | CMake, namespaces, testes |
| **1. Núcleo de rede e parser** | Classe `Socket` RAII, `SO_REUSEADDR`, laço de leitura, parser completo da linha de requisição e cabeçalhos, `HttpResponse` com serialização | RAII, TCP como fluxo, `string_view`, parsing |
| **2. Arquivos estáticos** | Servir `www/`, tipos MIME, 404, proteção contra path traversal, `Content-Length` correto | Sistema de arquivos (`std::filesystem`), segurança |
| **3. Roteamento** | `Router` com handlers registráveis (`router.get("/ping", ...)`), 405 Method Not Allowed | `std::function`, design de API |
| **4. Concorrência** | Thread pool; tratamento de múltiplos clientes; encerramento gracioso | Threads, mutex, condition variable, sinais |
| **5. HTTP robusto** | Keep-alive, timeouts, limites de tamanho, corpo da requisição (POST) com `Content-Length`, códigos de erro corretos | RFC 9112, defesa contra abuso |
| **6. Observabilidade e configuração** | Logger com níveis, configuração por linha de comando/arquivo | Design de módulos transversais |
| **7. Alta performance** | Event loop com `kqueue`/`epoll`, benchmarks, perfil | I/O assíncrono, medição |
| **8. Acabamento profissional** | CI no GitHub Actions, sanitizers, clang-tidy, ADRs, README com resultados de benchmark | Engenharia de software |

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
