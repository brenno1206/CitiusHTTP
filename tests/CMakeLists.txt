include(FetchContent)

# GoogleTest: unica dependencia externa permitida (so para testes)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.15.2
)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

include(GoogleTest)

# Cada grupo de tests/unit lista os proprios testes no CMakeLists.txt da sua pasta (espelham src/)
add_executable(citius_tests integration/ServerTest.cpp)
add_subdirectory(unit/util)
add_subdirectory(unit/net)
add_subdirectory(unit/http)
add_subdirectory(unit/parse)
add_subdirectory(unit/routing)
add_subdirectory(unit/files)
add_subdirectory(unit/server)
set_target_properties(citius_tests PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(citius_tests PRIVATE citius_core GTest::gtest_main)
target_compile_options(citius_tests PRIVATE ${CITIUS_WARNINGS})

# TIMEOUT: um teste de rede que trave nao pode prender o ctest
gtest_discover_tests(citius_tests PROPERTIES TIMEOUT 30)
