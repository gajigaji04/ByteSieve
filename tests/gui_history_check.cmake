# GUI 폴더별 용량 변화·기록 설정 자체 검사(bytesieve-gui --self-test / --self-test-changes /
# --self-test-no-autosave / --self-test-clear-history).
#
#   cmake -DEXE=<bytesieve-gui> -DFIXTURE=<빈 폴더로 만들 경로> -DHIST=<기록 폴더> -P gui_history_check.cmake
#
# 첫 스캔은 기준 스냅숏을 저장하고, 픽스처를 바꾼 뒤 두 번째 스캔이 변화 순위 보기·변화 열을
# 검증한 다음 변화 목록을 stdout 에 쓴다. 이어서 자동 저장을 끄면 스냅숏이 바뀌지 않고 재실행해도
# 설정이 유지되는지, 기록 삭제가 스냅숏만 지우고 화면에 반영되는지 확인한다.
# 기록·설정은 HIST 에만 쓴다(BYTESIEVE_HISTORY_DIR).

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

# 3) 자동 저장 끄기: 처음엔 켜져 있고, 끈 뒤 스캔해도 스냅숏이 바뀌지 않는다.
file(GLOB snaps "${HIST}/*.snap")
file(SHA256 "${snaps}" snap_hash)
file(WRITE "${FIXTURE}/c/later.bin" "${thousand}")
run_gui(out --self-test-no-autosave "${native_fixture}")
expect("${out}" "autosave: 1\n")
file(READ "${HIST}/settings.txt" settings)
expect("${settings}" "\nautosave\t0\n")
file(SHA256 "${snaps}" after_hash)
if(NOT snap_hash STREQUAL after_hash)
    message(FATAL_ERROR "snapshot changed while autosave was off")
endif()

# 4) 다시 실행해도 꺼진 설정이 유지된다(여전히 저장하지 않음).
run_gui(out --self-test-no-autosave "${native_fixture}")
expect("${out}" "autosave: 0\n")
file(SHA256 "${snaps}" after_hash)
if(NOT snap_hash STREQUAL after_hash)
    message(FATAL_ERROR "snapshot changed after restart with autosave off")
endif()

# 5) 기록 삭제: 스냅숏(+남은 임시 파일)만 지우고 설정·다른 파일은 남긴다.
file(WRITE "${HIST}/0000000000000001.snap.tmp" "partial")
file(WRITE "${HIST}/notes.snap" "user file")
file(WRITE "${HIST}/keep.txt" "keep")
run_gui(out --self-test-clear-history "${native_fixture}")
expect("${out}" "cleared: 1\n")
file(GLOB snaps "${HIST}/*.snap" "${HIST}/*.snap.tmp")
if(NOT snaps STREQUAL "${HIST}/notes.snap")
    message(FATAL_ERROR "unexpected files left after clearing history: ${snaps}")
endif()
foreach(kept settings.txt keep.txt)
    if(NOT EXISTS "${HIST}/${kept}")
        message(FATAL_ERROR "${kept} was removed by clearing history")
    endif()
endforeach()

file(REMOVE_RECURSE "${FIXTURE}" "${HIST}")
