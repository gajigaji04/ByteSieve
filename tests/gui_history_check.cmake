# GUI 폴더별 용량 변화 자체 검사(bytesieve-gui --self-test / --self-test-changes).
#
#   cmake -DEXE=<bytesieve-gui> -DFIXTURE=<빈 폴더로 만들 경로> -DHIST=<기록 폴더> -P gui_history_check.cmake
#
# 첫 스캔은 기준 스냅숏을 저장하고, 픽스처를 바꾼 뒤 두 번째 스캔이 변화 순위 보기·변화 열을
# 검증한 다음 변화 목록을 stdout 에 쓴다. 기록은 HIST 에만 쓴다(BYTESIEVE_HISTORY_DIR).

foreach(var EXE FIXTURE HIST)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "${var} is required")
    endif()
endforeach()

function(run_gui out_var)
    file(TO_NATIVE_PATH "${HIST}" native_hist)
    execute_process(COMMAND ${CMAKE_COMMAND} -E env "BYTESIEVE_HISTORY_DIR=${native_hist}"
                            "${EXE}" ${ARGN}
                    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 110)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "exit code ${rc}\nstdout:\n${out}\nstderr:\n${err}")
    endif()
    set(${out_var} "${out}" PARENT_SCOPE)
endfunction()

function(expect text regex)
    if(NOT text MATCHES "${regex}")
        message(FATAL_ERROR "output does not match '${regex}':\n${text}")
    endif()
endfunction()

file(REMOVE_RECURSE "${FIXTURE}" "${HIST}")
string(REPEAT "x" 100 hundred)
string(REPEAT "y" 1000 thousand)
file(WRITE "${FIXTURE}/a/one.bin" "${hundred}")
file(WRITE "${FIXTURE}/b/two.bin" "${hundred}")
file(TO_NATIVE_PATH "${FIXTURE}" native_fixture)

# 1) 기준 스냅숏(첫 스캔)
run_gui(out --self-test "${native_fixture}")
file(GLOB snaps "${HIST}/*.snap")
list(LENGTH snaps snap_count)
if(NOT snap_count EQUAL 1)
    message(FATAL_ERROR "expected one snapshot in ${HIST}, found ${snap_count}")
endif()

# 2) 변화 후 비교
file(WRITE "${FIXTURE}/a/more.bin" "${thousand}")
file(REMOVE_RECURSE "${FIXTURE}/b")
file(WRITE "${FIXTURE}/c/new.bin" "${hundred}")
run_gui(out --self-test-changes "${native_fixture}")
expect("${out}" "change: grown \\+1000 B a\n")
expect("${out}" "change: removed -100 B b\n")
expect("${out}" "change: added \\+100 B c\n")
expect("${out}" "changes: grown 1 shrunk 0 added 1 removed 1\n")

file(REMOVE_RECURSE "${FIXTURE}" "${HIST}")
