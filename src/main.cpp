#include "citius/Server.hpp"

int main(){
    citius::Server server{8080};
    server.run();
    return 0;
}