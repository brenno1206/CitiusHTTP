#include "citius/http/HttpRequest.hpp"
#include "citius/http/HttpResponse.hpp"
#include "citius/server/Config.hpp"
#include "citius/server/Server.hpp"
#include "citius/util/Logger.hpp"
#include <algorithm>
#include <atomic>
#include <climits> // IWYU pragma: keep (OPEN_MAX, so existe no macOS)
#include <cstddef>
#include <exception>
#include <format>
#include <iostream>
#include <limits> // IWYU pragma: keep (usado no ramo sem OPEN_MAX)
#include <signal.h>
#include <string>
#include <string_view>
#include <sys/resource.h> // getrlimit(), setrlimit()
#include <vector>

namespace {

// O handler de sinal nao recebe argumentos, entao precisa achar o servidor por uma variavel global.
// std::atomic de ponteiro e lock-free: ler e escrever dentro de um handler e permitido.
std::atomic<citius::Server*> g_server = nullptr; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables): o
                                                 // handler de sinal so alcanca o servidor por aqui

// Dentro de um handler quase nada e seguro (nada de cout, malloc, mutex); request_stop()
// so grava um std::atomic<bool>, que e.
extern "C" void handle_stop_signal(int) {
    if (citius::Server* server = g_server.load()) {
        server->request_stop();
    }
}

void install_stop_handlers() {
    struct sigaction action{};
    action.sa_handler = handle_stop_signal;
    sigemptyset(&action.sa_mask);
    // SA_RESETHAND: o primeiro Ctrl+C pede parada graciosa; um segundo, se o encerramento travar,
    // volta ao comportamento padrao e mata o processo
    action.sa_flags = SA_RESETHAND;
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);
}

// Cada conexao usa um descritor de arquivo, e o limite padrao do processo e baixo (256 no macOS):
// sem subi-lo ate o maximo permitido, algumas centenas de clientes ja dariam "Too many open files".
void raise_fd_limit(std::size_t max_connections) {
    rlimit limit{};
    if (getrlimit(RLIMIT_NOFILE, &limit) != 0) return;

    // macOS recusa mais que OPEN_MAX mesmo quando rlim_max e "ilimitado"; no Linux nao ha esse teto
#ifdef OPEN_MAX
    constexpr rlim_t kPlatformCap = OPEN_MAX;
#else
    constexpr rlim_t kPlatformCap = std::numeric_limits<rlim_t>::max();
#endif
    const rlim_t wanted = std::min(limit.rlim_max, kPlatformCap);
    if (wanted > limit.rlim_cur) {
        limit.rlim_cur = wanted;
        if (setrlimit(RLIMIT_NOFILE, &limit) != 0) getrlimit(RLIMIT_NOFILE, &limit);
    }
    // alem das conexoes ha o socket de escuta, o kqueue/epoll, stdin/out/err e arquivos servidos
    constexpr rlim_t kReserved = 16;
    if (limit.rlim_cur < max_connections + kReserved) {
        citius::log_warn(std::format(
            "--max-connections {} nao cabe no limite de descritores ({}); acima dele os clientes falham com EMFILE.",
            max_connections, static_cast<unsigned long long>(limit.rlim_cur)));
    }
}

int run(int argc, char** argv) {
    const char* program = argc > 0 ? argv[0] : "citius_server";
    // argv[0] e o nome do programa; os argumentos de verdade comecam em 1 (argc pode ser 0 em execucoes exoticas)
    std::vector<std::string_view> args;
    if (argc > 1) args.assign(argv + 1, argv + argc);

    citius::ConfigResult const parsed = citius::parse_config(args);
    if (parsed.status == citius::ParseStatus::HELP) {
        // o texto de ajuda e a SAIDA do programa (nao um log): vai para o stdout, sem hora nem nivel
        std::cout << citius::config_usage(program);
        return 0;
    }
    if (parsed.status == citius::ParseStatus::INVALID) {
        // o Logger ainda esta no padrao (console, nivel INFO): um ERROR sai no stderr, como todo erro do servidor
        citius::log_error(std::format("{} (use --help para ver as opcoes)", parsed.error));
        return 2; // 2 = uso incorreto da linha de comando; 1 fica para falhas em execucao
    }
    const citius::Config& config = parsed.config;
    citius::Logger::instance().set_level(config.log_level);

    // falhas de inicializacao (ex.: porta ocupada) viram mensagem clara e codigo de saida 1
    try {
        if (config.log_file) {
            // lanca se nao puder abrir o arquivo (pasta inexistente, sem permissao): falha de inicializacao, exit 1
            citius::Logger::instance().log_to_file(*config.log_file, config.log_max_bytes, config.log_keep);
        }
        raise_fd_limit(config.max_connections);
        citius::Server server{config.port, config.root, config.max_connections};
        server.set_read_timeout(config.read_timeout);
        server.set_idle_timeout(config.idle_timeout);
        server.set_max_requests_per_connection(config.max_requests);
        server.router().get("/ping", [](const citius::HttpRequest&) {
            citius::HttpResponse response;
            response.set_body("pong\n");
            return response;
        });
        g_server = &server;
        install_stop_handlers();
        server.run();
        g_server = nullptr; // o servidor sai de escopo logo abaixo; o handler nao pode mais usa-lo
        citius::log_info("Servidor encerrado.");
    } catch (const std::exception& e) {
        citius::log_error(std::string("Erro fatal: ") + e.what());
        return 1;
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    // Rede de seguranca: excecao escapando de main chamaria std::terminate sem nenhuma mensagem. Aqui o cerr e
    // proposital: o proprio Logger pode ser o que falhou.
    try {
        return run(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "Erro fatal: " << e.what() << '\n';
    } catch (...) {
        std::cerr << "Erro fatal desconhecido.\n";
    }
    return 1;
}
