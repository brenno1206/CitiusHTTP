# 0002. Erros em dois níveis: exceção para o ambiente, valor para o cliente

- Status: Aceita
- Data: 2026-11-03

## Contexto

Um servidor recebe lixo o tempo todo: requisições malformadas, cabeçalhos enormes, argumentos de
linha de comando digitados errado. Isso é rotina, não situação excepcional. Já "a porta está ocupada"
ou "não consegui criar o kqueue" são falhas do ambiente em que o programa não tem como continuar.

## Decisão

- **Entrada externa inválida** (cliente ou operador) **não lança exceção**: a função devolve um tipo
  com o resultado ou o motivo do erro (`ParseResult`/`ParseError`, `AssembleStatus`, `ConfigResult`).
  O motivo vira um status HTTP (400, 413, 431, 501, 505) ou uma mensagem de uso.
- **Falha de inicialização ou do ambiente lança** (`Server` com porta ocupada, `Poller` sem recurso do
  SO), e `main` a transforma em mensagem e código de saída 1. Argumento inválido sai com 2.
- Um handler que lança durante uma requisição é falha *do código*, não do cliente: a `Connection` a
  captura, responde 500 sem vazar a mensagem e segue atendendo os outros.

## Alternativas consideradas

- **Exceções para tudo.** Um cliente malicioso passaria a controlar o caminho mais caro do programa, e
  o fluxo normal ficaria cheio de `try/catch`.
- **Códigos de erro para tudo (inclusive inicialização).** Quem chama `Server(...)` precisaria checar
  cada passo; esquecer um deixaria um servidor meio inicializado.

## Consequências

- O compilador ajuda: `[[nodiscard]]` nos retornos impede ignorar um erro de cliente.
- Cada tipo de resultado novo é um pouco de código a mais, e quem o recebe precisa tratar todos os casos.
