# 0009. Configuração por arquivo e log em arquivo

- Status: Aceita
- Data: 2026-11-03

## Contexto

Só havia linha de comando e o log ia sempre para o console. Um servidor que roda em segundo plano precisa de uma
configuração versionável e de um log que não cresça sem limite nem se perca.

## Decisão

- **`--config ARQUIVO`** lê linhas `chave = valor` (as mesmas opções da linha de comando, sem o `--`), com linhas em branco e
  comentários `#` no início da linha. O valor é o resto da linha, sem os espaços das pontas. Até 64 KB; erros citam
  `arquivo:linha`. `config` dentro do arquivo é erro (evita inclusões encadeadas). Sem dependências: o formato é o mais simples
  que serve.
- **Precedência: padrões < arquivo < linha de comando**, qualquer que seja a ordem dos argumentos. A linha de comando é
  varrida primeiro (cada valor validado onde aparece), o arquivo entra depois e as opções da linha de comando são reaplicadas
  por cima. `--help` sempre funciona, mesmo com um `--config` quebrado.
- **Log em arquivo** (`--log-file`, `--log-max-bytes`, `--log-keep`): `RotatingFile` rotaciona por tamanho (`app.log` →
  `app.log.1` → ...), sem partir uma linha ao meio; o que passa de `keep` é apagado. Com arquivo, **todas** as mensagens vão
  para ele e o console fica quieto.
- **Nunca perder uma linha em silêncio.** Erro de disco em tempo de execução não lança exceção: `RotatingFile::write`
  devolve `false` e o `Logger` escreve a linha no `stderr`. Já abrir o arquivo na inicialização é falha de ambiente e lança
  (ver 0002): `main` responde com mensagem clara e código de saída 1.
- `main` só usa `std::cout` para o texto do `--help` (saída do programa, não log); erros de uso passam pelo `Logger`.

## Alternativas consideradas

- **TOML/JSON/YAML.** Exigiriam parser próprio (ou dependência, vetada pelo projeto) para um ganho que não precisamos.
- **Rotação por tempo** (diária). Útil em produção, mas por tamanho já garante o teto de disco, que era o problema.
- **Reabrir o arquivo em `SIGHUP`** (o jeito do `logrotate`). Fica para quando houver necessidade.

## Consequências

- `Config.cpp` ficou dirigido por tabela (cada opção numérica é uma linha com mínimo, máximo e setter): acrescentar uma opção
  não cria mais um ramo numa cadeia de `if/else`.
- O arquivo é lido uma vez, na inicialização: mudar a configuração exige reiniciar.
