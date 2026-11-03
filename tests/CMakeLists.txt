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

add_executable(citius_tests
    unit/HttpParserTest.cpp
    unit/HttpRequestTest.cpp
    unit/HttpResponseTest.cpp
    unit/MimeTest.cpp
    unit/StaticPathTest.cpp
    unit/StaticFilesTest.cpp
    unit/RouterTest.cpp
    unit/ThreadPoolTest.cpp
    unit/SocketTest.cpp
    unit/RequestReaderTest.cpp
    integration/ServerTest.cpp
)
set_target_properties(citius_tests PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(citius_tests PRIVATE citius_core GTest::gtest_main)
target_compile_options(citius_tests PRIVATE -Wall -Wextra -Wpedantic)

# TIMEOUT: um teste de rede que trave nao pode prender o ctest
gtest_discover_tests(citius_tests PROPERTIES TIMEOUT 30)
