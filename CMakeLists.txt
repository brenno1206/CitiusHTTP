cmake_minimum_required(VERSION 3.20)
project(CitiusHTTP LANGUAGES CXX)

# Sem tipo de build escolhido, o CMake compila sem -g nem -O (fica pior que Debug e Release).
# Debug e o padrao para desenvolvimento; para medir desempenho use -DCMAKE_BUILD_TYPE=Release
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Tipo de build" FORCE)
endif()

# compile_commands.json no diretorio de build: e o que o clang-tidy (e os editores) usam para saber
# como cada arquivo e compilado
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Warnings como erro: cmake -S . -B build -DCITIUS_WERROR=ON. Fica desligado por padrao para que um
# compilador novo, com warnings novos, nao impeca ninguem de compilar; o CI liga.
option(CITIUS_WERROR "Trata warnings como erros" OFF)

# Lista unica de warnings, usada pela biblioteca, pelo servidor e pelos testes
set(CITIUS_WARNINGS -Wall -Wextra -Wpedantic)
if(CITIUS_WERROR)
    list(APPEND CITIUS_WARNINGS -Werror)
endif()

# Sanitizers (ASan + UBSan): cmake -S . -B build-asan -DCITIUS_SANITIZE=ON
option(CITIUS_SANITIZE "Compila com AddressSanitizer e UndefinedBehaviorSanitizer" OFF)

# ThreadSanitizer (detecta corridas de dados): cmake -S . -B build-tsan -DCITIUS_TSAN=ON
option(CITIUS_TSAN "Compila com ThreadSanitizer (nao combina com CITIUS_SANITIZE)" OFF)

# Biblioteca com quase todo o código (sem main), reaproveitada pelo servidor e pelos testes.
# Nasce vazia: cada grupo de src/ lista os proprios fontes no CMakeLists.txt da sua pasta, ao lado do
# codigo (e nao numa lista unica aqui). Ao criar um arquivo novo, basta acrescenta-lo no da pasta dele.
add_library(citius_core STATIC)
add_subdirectory(src/util)
add_subdirectory(src/net)
add_subdirectory(src/http)
add_subdirectory(src/parse)
add_subdirectory(src/routing)
add_subdirectory(src/files)
add_subdirectory(src/server)

# Padrao C++20 puro (sem extensoes GNU). PUBLIC: quem linka a biblioteca herda o requisito
target_compile_features(citius_core PUBLIC cxx_std_20)
set_target_properties(citius_core PROPERTIES CXX_EXTENSIONS OFF)

# Incluindo o diretório de headers
# #include "citius/server/Server.hpp"
target_include_directories(citius_core PUBLIC include)

# Habilitando os warnings do compilador para manter um código limpo
target_compile_options(citius_core PRIVATE ${CITIUS_WARNINGS})
if(CITIUS_SANITIZE AND CITIUS_TSAN)
    message(FATAL_ERROR "CITIUS_SANITIZE e CITIUS_TSAN sao incompativeis: o ThreadSanitizer nao combina com o AddressSanitizer. Use um diretorio de build para cada.")
endif()
if(CITIUS_SANITIZE)
    # PUBLIC: quem linka a biblioteca tambem precisa linkar com o sanitizer
    target_compile_options(citius_core PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(citius_core PUBLIC -fsanitize=address,undefined)
endif()
if(CITIUS_TSAN)
    target_compile_options(citius_core PUBLIC -fsanitize=thread -fno-omit-frame-pointer)
    target_link_options(citius_core PUBLIC -fsanitize=thread)
endif()

# Executavel fino: so o main, ligado a biblioteca
add_executable(citius_server src/main.cpp)
set_target_properties(citius_server PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(citius_server PRIVATE citius_core)
target_compile_options(citius_server PRIVATE ${CITIUS_WARNINGS})

# Testes (ctest --test-dir build)
enable_testing()
add_subdirectory(tests)
