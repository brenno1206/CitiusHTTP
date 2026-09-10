cmake_minimum_required(VERSION 3.10)
project(CitiusHTTP)

# Arquivos fonte
add_executable(citius_server 
    src/main.cpp
    src/HttpParser.cpp
    src/Server.cpp
)

# Padrao C++20
target_compile_features(citius_server PRIVATE cxx_std_20)

# Incluindo o diretório de headers
# #include "citius/Server.hpp"
target_include_directories(citius_server PRIVATE include)

# Habilitando os warnings do compilador para manter um código limpo
target_compile_options(citius_server PRIVATE -Wall -Wextra -Wpedantic)