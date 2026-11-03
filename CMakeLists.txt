cmake_minimum_required(VERSION 3.20)
project(CitiusHTTP LANGUAGES CXX)

# Sanitizers (ASan + UBSan): cmake -S . -B build-asan -DCITIUS_SANITIZE=ON
option(CITIUS_SANITIZE "Compila com AddressSanitizer e UndefinedBehaviorSanitizer" OFF)

# ThreadSanitizer (detecta corridas de dados): cmake -S . -B build-tsan -DCITIUS_TSAN=ON
option(CITIUS_TSAN "Compila com ThreadSanitizer (nao combina com CITIUS_SANITIZE)" OFF)

# Biblioteca com quase todo o código (sem main), reaproveitada pelo servidor e pelos testes
add_library(citius_core STATIC
    src/HttpParser.cpp
    src/HttpResponse.cpp
    src/Mime.cpp
    src/StaticPath.cpp
    src/StaticFiles.cpp
    src/Router.cpp
    src/ThreadPool.cpp
    src/Logger.cpp
    src/Config.cpp
    src/Socket.cpp
    src/RequestReader.cpp
    src/RequestAssembler.cpp
    src/Connection.cpp
    src/Server.cpp
)

# Poller: kqueue no macOS, epoll no Linux (mesma interface em Poller.hpp)
if(APPLE)
    target_sources(citius_core PRIVATE src/PollerKqueue.cpp)
else()
    target_sources(citius_core PRIVATE src/PollerEpoll.cpp)
endif()

# Padrao C++20 puro (sem extensoes GNU). PUBLIC: quem linka a biblioteca herda o requisito
target_compile_features(citius_core PUBLIC cxx_std_20)
set_target_properties(citius_core PROPERTIES CXX_EXTENSIONS OFF)

# Incluindo o diretório de headers
# #include "citius/Server.hpp"
target_include_directories(citius_core PUBLIC include)

# Habilitando os warnings do compilador para manter um código limpo
target_compile_options(citius_core PRIVATE -Wall -Wextra -Wpedantic)
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
target_compile_options(citius_server PRIVATE -Wall -Wextra -Wpedantic)

# Testes (ctest --test-dir build)
enable_testing()
add_subdirectory(tests)
