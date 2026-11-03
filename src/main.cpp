#include "citius/Config.hpp"
#include "citius/Logger.hpp"
#include "citius/Server.hpp"
#include <atomic>
#include <algorithm>
#include <climits> // OPEN_MAX (macOS)
#include <csignal>
#include <cstddef>
#include <exception>
#include <format>
#include <iostream>
#include <string>
#include <string_view>
#include <sys/resource.h> // getrlimit(), setrlimit()
#include <vector>

namespace {

// O handler de sinal nao recebe argumentos, entao precisa achar o servidor por uma variavel global.
// std::atomic de ponteiro e lock-free: ler e escrever dentro de um handler e permitido.
std::atomic<citius::Server*> g_server = nullptr;

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

    rlim_t wanted = limit.rlim_max;
#ifdef OPEN_MAX
    wanted = std::min<rlim_t>(wanted, OPEN_MAX); // macOS recusa mais que OPEN_MAX mesmo com rlim_max "ilimitado"
#endif
    if (wanted > limit.rlim_cur) {
        limit.rlim_cur = wanted;
        if (setrlimit(RLIMIT_NOFILE, &limit) != 0) getrlimit(RLIMIT_NOFILE, &limit);
    }
    // alem das conexoes ha o socket de escuta, o kqueue/epoll, stdin/out/err e arquivos servidos
    constexpr rlim_t kReserved = 16;
    if (limit.rlim_cur < max_connections + kReserved) {
        citius::log_warn(std::format("--max-connections {} nao cabe no limite de descritores ({}); acima dele os clientes falham com EMFILE.",
                                     max_connections, static_cast<unsigned long long>(limit.rlim_cur)));
    }
}

} // namespace

int main(int argc, char** argv) {
    const char* program = argc > 0 ? argv[0] : "citius_server";
    // argv[0] e o nome do programa; os argumentos de verdade comecam em 1 (argc pode ser 0 em execucoes exoticas)
    std::vector<std::string_view> args;
    if (argc > 1) args.assign(argv + 1, argv + argc);

    // saida do operador (ajuda/erro de uso) vai direto ao terminal: o Logger ainda nao foi configurado
    citius::ConfigResult parsed = citius::parse_config(args);
    if (parsed.status == citius::ParseStatus::HELP) {
        std::cout << citius::config_usage(program);
        return 0;
    }
    if (parsed.status == citius::ParseStatus::INVALID) {
        std::cerr << "Erro: " << parsed.error << "\nUse --help para ver as opcoes.\n";
        return 2; // 2 = uso incorreto da linha de comando; 1 fica para falhas em execucao
    }
    const citius::Config& config = parsed.config;
    citius::Logger::instance().set_level(config.log_level);

    // falhas de inicializacao (ex.: porta ocupada) viram mensagem clara e codigo de saida 1
    try {
        raise_fd_limit(config.max_connections);
        citius::Server server{config.port, config.root, config.max_connections};
        server.set_read_timeout(config.read_timeout);
        server.set_idle_timeout(config.idle_timeout);
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
