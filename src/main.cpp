#include "citius/Logger.hpp"
#include "citius/Server.hpp"
#include <atomic>
#include <csignal>
#include <exception>
#include <string>

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

} // namespace

int main(){
    // falhas de inicializacao (ex.: porta ocupada) viram mensagem clara e codigo de saida 1
    try {
        citius::Server server{8080};
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
