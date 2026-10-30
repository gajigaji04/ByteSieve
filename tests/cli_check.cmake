# disk-scanner 를 실행해 종료 코드와 출력을 정확히 검사한다.
#
#   cmake -DEXE=<disk-scanner> -DARGS="a|b|c" -DEXPECT_EXIT=<n>
#         [-DEXPECT_STDOUT=<regex>] [-DEXPECT_STDERR=<regex>]
#         [-DDENY_FIXTURE=<dir>] -P cli_check.cmake
#
# ARGS 는 '|' 로 구분한다(CTest 인자에서 ';' 는 목록 구분자라서).
# 정규식은 '|' 로 나눠 각각이 모두 일치해야 한다.
# DENY_FIXTURE 가 있으면 <dir>\ok\f.txt 와 접근 거부된 <dir>\locked\ 를 만들고
# 실행 뒤 지운다(ARGS 의 @FIXTURE@ 는 <dir> 로 바뀐다).

if(NOT DEFINED EXE OR NOT DEFINED EXPECT_EXIT)
    message(FATAL_ERROR "EXE and EXPECT_EXIT are required")
endif()

set(args "")
if(DEFINED ARGS AND NOT ARGS STREQUAL "")
    string(REPLACE "|" ";" args "${ARGS}")
endif()

if(DEFINED DENY_FIXTURE)
    file(TO_NATIVE_PATH "${DENY_FIXTURE}" fixture_native)
    file(REMOVE_RECURSE "${DENY_FIXTURE}")
    file(MAKE_DIRECTORY "${DENY_FIXTURE}/ok" "${DENY_FIXTURE}/locked")
    file(WRITE "${DENY_FIXTURE}/ok/f.txt" "12345")
    execute_process(COMMAND icacls "${fixture_native}\\locked" /deny "*S-1-1-0:(F)"
                    RESULT_VARIABLE icacls_rc OUTPUT_QUIET ERROR_QUIET)
    if(NOT icacls_rc EQUAL 0)
        message(FATAL_ERROR "icacls deny failed: ${icacls_rc}")
    endif()
    string(REPLACE "@FIXTURE@" "${fixture_native}" args "${args}")
endif()

execute_process(COMMAND "${EXE}" ${args}
                RESULT_VARIABLE rc
                OUTPUT_VARIABLE out
                ERROR_VARIABLE err)

if(DEFINED DENY_FIXTURE)
    execute_process(COMMAND icacls "${fixture_native}\\locked" /remove:d "*S-1-1-0"
                    OUTPUT_QUIET ERROR_QUIET)
    file(REMOVE_RECURSE "${DENY_FIXTURE}")
endif()

set(failed FALSE)
if(NOT rc STREQUAL "${EXPECT_EXIT}")
    message(SEND_ERROR "exit code: expected ${EXPECT_EXIT}, got ${rc}")
    set(failed TRUE)
endif()

foreach(stream STDOUT STDERR)
    if(stream STREQUAL "STDOUT")
        set(text "${out}")
    else()
        set(text "${err}")
    endif()
    if(DEFINED EXPECT_${stream})
        string(REPLACE "|" ";" patterns "${EXPECT_${stream}}")
        foreach(pattern IN LISTS patterns)
            if(NOT text MATCHES "${pattern}")
                message(SEND_ERROR "${stream} does not match: ${pattern}")
                set(failed TRUE)
            endif()
        endforeach()
    endif()
endforeach()

if(failed)
    message(FATAL_ERROR "--- stdout ---\n${out}\n--- stderr ---\n${err}")
endif()
