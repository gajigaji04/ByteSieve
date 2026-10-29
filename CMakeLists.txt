cmake_minimum_required(VERSION 3.21)
project(ByteSieve LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

function(bytesieve_configure target)
    target_compile_definitions(${target} PRIVATE UNICODE _UNICODE)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /utf-8 /std:c11)
    endif()
endfunction()

add_library(bytesieve_path STATIC src/path.c)
target_include_directories(bytesieve_path PUBLIC src)
bytesieve_configure(bytesieve_path)

add_executable(disk-scanner src/main.c)
bytesieve_configure(disk-scanner)

enable_testing()

# 인자 없이 실행하면 종료 코드 2(비정상)여야 한다.
add_test(NAME no_args COMMAND disk-scanner)
set_tests_properties(no_args PROPERTIES WILL_FAIL TRUE)

add_test(NAME help COMMAND disk-scanner --help)

add_executable(test_path tests/test_path.c)
target_link_libraries(test_path PRIVATE bytesieve_path)
bytesieve_configure(test_path)
add_test(NAME path COMMAND test_path)
