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

add_library(bytesieve_cli STATIC src/cli.c)
target_include_directories(bytesieve_cli PUBLIC src)
target_link_libraries(bytesieve_cli PUBLIC bytesieve_scanner)
bytesieve_configure(bytesieve_cli)

# GUI 연동용 데이터 모델(진행률·폴더 트리·용량 순위). 출력 코드에 의존하지 않는다.
add_library(bytesieve_model STATIC src/scan_model.c)
target_include_directories(bytesieve_model PUBLIC src)
target_link_libraries(bytesieve_model PUBLIC bytesieve_scanner)
bytesieve_configure(bytesieve_model)

add_executable(disk-scanner src/main.c)
target_link_libraries(disk-scanner PRIVATE bytesieve_cli bytesieve_jsonl)
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

# 종료 코드와 출력을 정확히 검사하는 CLI 테스트(tests/cli_check.cmake).
# args 는 '|' 로 구분, stdout/stderr 는 '|' 로 구분한 정규식(모두 일치해야 함).
function(bytesieve_cli_test name expect_exit args stdout_re stderr_re)
    set(defs -DEXE=$<TARGET_FILE:disk-scanner> "-DARGS=${args}" -DEXPECT_EXIT=${expect_exit})
    if(NOT stdout_re STREQUAL "")
        list(APPEND defs "-DEXPECT_STDOUT=${stdout_re}")
    endif()
    if(NOT stderr_re STREQUAL "")
        list(APPEND defs "-DEXPECT_STDERR=${stderr_re}")
    endif()
    list(APPEND defs ${ARGN})
    add_test(NAME cli_${name}
             COMMAND ${CMAKE_COMMAND} ${defs} -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_check.cmake)
endfunction()

set(SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)

bytesieve_cli_test(complete 0 "${SRC_DIR}"
    "Files: +[1-9]|Directories: +0|Total size: +[0-9.]+ KiB|Errors: +0\n|Status: +complete" "")
bytesieve_cli_test(options_after_target 0 "${SRC_DIR}|--max-depth=0"
    "Max depth: +0\n|Status: +complete" "")
bytesieve_cli_test(partial 1 "@FIXTURE@"
    "Files: +1\n|Directories: +2\n|Total size: +5 bytes|Errors: +1 .access_denied 1.|Skipped dirs: +1\n|Aborted: +no|Status: +partial .1 directory could not be read"
    "error: .*locked: access_denied .Windows error 5."
    -DDENY_FIXTURE=${CMAKE_CURRENT_BINARY_DIR}/cli_partial_fixture)
bytesieve_cli_test(fatal_missing 2 "${CMAKE_CURRENT_SOURCE_DIR}/no-such-dir"
    "Errors: +1 .not_found 1.|Aborted: +yes|Status: +fatal .scan aborted."
    "error: .*no-such-dir: not_found")
bytesieve_cli_test(fatal_not_dir 2 "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt"
    "Errors: +1 .invalid_path 1.|Status: +fatal" "")
bytesieve_cli_test(help 0 "--help" "Usage: disk-scanner|Exit codes:" "")
bytesieve_cli_test(help_wins 0 "--bogus|-h" "Usage: disk-scanner" "")
bytesieve_cli_test(no_args 2 "" "" "error: missing <directory>|Usage: disk-scanner")
bytesieve_cli_test(unknown_option 2 "--verbose|${SRC_DIR}" "" "error: unknown option: --verbose|Usage:")
bytesieve_cli_test(missing_value 2 "${SRC_DIR}|--max-depth" "" "error: --max-depth requires a value")
bytesieve_cli_test(bad_value 2 "--max-depth|abc|${SRC_DIR}" "" "error: invalid --max-depth value: 'abc'")
bytesieve_cli_test(duplicate_option 2 "--max-depth=1|--max-depth=2|${SRC_DIR}" ""
    "error: --max-depth given more than once")
bytesieve_cli_test(extra_argument 2 "${SRC_DIR}|${SRC_DIR}" "" "error: unexpected extra argument")

# JSON Lines 모드: stdout 은 UTF-8 이벤트 줄(start → result → complete).
bytesieve_cli_test(jsonl_scan 0 "--jsonl|${SRC_DIR}"
    "^{\"event\":\"start\",[^\n]*}\n{\"event\":\"result\",[^\n]*}\n{\"event\":\"complete\",\"status\":\"complete\",\"exit_code\":0}\n$" "")
bytesieve_cli_test(jsonl_max_depth 0 "${CMAKE_CURRENT_SOURCE_DIR}|--max-depth=0|--jsonl"
    "{\"event\":\"complete\",\"status\":\"complete\",\"exit_code\":0}" "")
bytesieve_cli_test(jsonl_missing_dir 2 "--jsonl|${CMAKE_CURRENT_SOURCE_DIR}/no-such-dir"
    "{\"event\":\"complete\",\"status\":\"failed\",\"exit_code\":2}" "")
bytesieve_cli_test(jsonl_partial 1 "--jsonl|@FIXTURE@"
    "\"error_count\":1,\"errors\":.{\"path\":\"[^\"]*locked\",\"code\":5}.|\"status\":\"partial\",\"exit_code\":1" ""
    -DDENY_FIXTURE=${CMAKE_CURRENT_BINARY_DIR}/cli_jsonl_partial_fixture)

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

add_executable(test_cli tests/test_cli.c)
target_link_libraries(test_cli PRIVATE bytesieve_cli)
bytesieve_configure(test_cli)
add_test(NAME cli COMMAND test_cli)

add_executable(test_scan_model tests/test_scan_model.c)
target_link_libraries(test_scan_model PRIVATE bytesieve_model)
bytesieve_configure(test_scan_model)
add_test(NAME scan_model COMMAND test_scan_model)
