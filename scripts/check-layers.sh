#!/usr/bin/env bash
# Confere a regra de camadas do projeto (ver CLAUDE.md): cada grupo so inclui os de baixo, e os grupos
# de protocolo nunca tocam em rede. Uso: scripts/check-layers.sh   (a partir da raiz do repositorio)
# Termina com erro e lista cada violacao. Sem arrays associativos: o bash 3.2 do macOS nao os tem.
set -u

# grupos que cada grupo pode incluir (alem de si mesmo)
allowed() {
    case "$1" in
        util) echo "util" ;;
        net) echo "util net" ;;
        http) echo "util http" ;;
        parse) echo "util http parse" ;;
        routing) echo "util http routing" ;;
        files) echo "util http files" ;;
        server) echo "util net http parse routing files server" ;;
    esac
}
# grupos que nunca podem incluir headers de socket/poll do sistema
NO_NETWORK=" util http parse routing files "
NETWORK_HEADERS='<(sys/socket|netinet/[a-z]+|arpa/inet|poll|sys/event|sys/epoll)\.h>'

fail=0
for group in util net http parse routing files server; do
    ok=" $(allowed "$group") "
    for f in $(find "include/citius/$group" "src/$group" \( -name '*.hpp' -o -name '*.cpp' \) 2>/dev/null); do
        # includes "citius/<grupo>/..." fora dos permitidos
        while IFS= read -r line; do
            target=$(sed -E 's|.*"citius/([a-z]+)/.*|\1|' <<<"$line")
            if [[ "$ok" != *" $target "* ]]; then
                echo "camada: '$group' nao pode incluir '$target' ($f: $line)"
                fail=1
            fi
        done < <(grep -E '#include "citius/[a-z]+/' "$f")
        # headers de rede em grupos de protocolo
        if [[ "$NO_NETWORK" == *" $group "* ]] && grep -Eq "#include $NETWORK_HEADERS" "$f"; then
            echo "rede: '$group' nao pode incluir headers de socket ($f)"
            fail=1
        fi
    done
done

if [[ $fail -eq 0 ]]; then echo "camadas ok"; fi
exit $fail
