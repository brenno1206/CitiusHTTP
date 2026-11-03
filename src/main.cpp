#include "citius/Server.hpp"
#include <exception>
#include <iostream>

int main(){
    // falhas de inicializacao (ex.: porta ocupada) viram mensagem clara e codigo de saida 1
    try {
        citius::Server server{8080};
        server.run();
    } catch (const std::exception& e) {
        std::cerr << "Erro fatal: " << e.what() << '\n';
        return 1;
    }
    return 0;
}
