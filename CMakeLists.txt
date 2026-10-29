cmake_minimum_required(VERSION 3.21)
project(ByteSieve LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_executable(disk-scanner src/main.c)

target_compile_definitions(disk-scanner PRIVATE UNICODE _UNICODE)

if(MSVC)
    target_compile_options(disk-scanner PRIVATE /W4 /utf-8 /std:c11)
endif()

enable_testing()

# 인자 없이 실행하면 종료 코드 2(비정상)여야 한다.
add_test(NAME no_args COMMAND disk-scanner)
set_tests_properties(no_args PROPERTIES WILL_FAIL TRUE)

add_test(NAME help COMMAND disk-scanner --help)
