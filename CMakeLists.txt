cmake_minimum_required(VERSION 3.20)
project(CitiusHTTP LANGUAGES CXX)

# Biblioteca com quase todo o código (sem main), reaproveitada pelo servidor e pelos testes
add_library(citius_core STATIC
    src/HttpParser.cpp
    src/Server.cpp
)

# Padrao C++20 puro (sem extensoes GNU). PUBLIC: quem linka a biblioteca herda o requisito
target_compile_features(citius_core PUBLIC cxx_std_20)
set_target_properties(citius_core PROPERTIES CXX_EXTENSIONS OFF)

# Incluindo o diretório de headers
# #include "citius/Server.hpp"
target_include_directories(citius_core PUBLIC include)

# Habilitando os warnings do compilador para manter um código limpo
target_compile_options(citius_core PRIVATE -Wall -Wextra -Wpedantic)

# Executavel fino: so o main, ligado a biblioteca
add_executable(citius_server src/main.cpp)
set_target_properties(citius_server PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(citius_server PRIVATE citius_core)
target_compile_options(citius_server PRIVATE -Wall -Wextra -Wpedantic)
