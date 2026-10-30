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

add_library(bytesieve_scanner STATIC src/scanner.c)
target_include_directories(bytesieve_scanner PUBLIC src)
target_link_libraries(bytesieve_scanner PUBLIC bytesieve_path)
bytesieve_configure(bytesieve_scanner)

add_library(bytesieve_jsonl STATIC src/jsonl.c)
target_include_directories(bytesieve_jsonl PUBLIC src)
target_link_libraries(bytesieve_jsonl PUBLIC bytesieve_scanner)
bytesieve_configure(bytesieve_jsonl)

add_executable(disk-scanner src/main.c)
target_link_libraries(disk-scanner PRIVATE bytesieve_scanner bytesieve_jsonl)
bytesieve_configure(disk-scanner)

enable_testing()

# 인자 없이 실행하면 종료 코드 2(비정상)여야 한다.
add_test(NAME no_args COMMAND disk-scanner)
set_tests_properties(no_args PROPERTIES WILL_FAIL TRUE)

add_test(NAME help COMMAND disk-scanner --help)

# 소스 디렉터리 스캔은 성공, 존재하지 않는 경로는 실패(종료 코드 2)해야 한다.
add_test(NAME scan_source_dir COMMAND disk-scanner ${CMAKE_CURRENT_SOURCE_DIR}/src)
add_test(NAME scan_missing_dir COMMAND disk-scanner ${CMAKE_CURRENT_SOURCE_DIR}/no-such-dir)
set_tests_properties(scan_missing_dir PROPERTIES WILL_FAIL TRUE)

# --max-depth 는 0 이상의 정수만 허용한다.
add_test(NAME scan_max_depth COMMAND disk-scanner --max-depth 0 ${CMAKE_CURRENT_SOURCE_DIR}/src)
add_test(NAME scan_bad_max_depth COMMAND disk-scanner --max-depth -1 ${CMAKE_CURRENT_SOURCE_DIR}/src)
set_tests_properties(scan_bad_max_depth PROPERTIES WILL_FAIL TRUE)

add_executable(test_path tests/test_path.c)
target_link_libraries(test_path PRIVATE bytesieve_path)
bytesieve_configure(test_path)
add_test(NAME path COMMAND test_path)

add_executable(test_scanner tests/test_scanner.c)
target_link_libraries(test_scanner PRIVATE bytesieve_scanner)
bytesieve_configure(test_scanner)
add_test(NAME scanner COMMAND test_scanner)

add_executable(test_jsonl tests/test_jsonl.c)
target_link_libraries(test_jsonl PRIVATE bytesieve_jsonl)
bytesieve_configure(test_jsonl)
add_test(NAME jsonl COMMAND test_jsonl)
