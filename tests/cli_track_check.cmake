# 폴더별 용량 변화 추적 검사(disk-scanner --track).
#
#   cmake -DEXE=<disk-scanner> -DFIXTURE=<빈 폴더로 만들 경로> -DHIST=<기록 폴더> -P cli_track_check.cmake
#
# 첫 실행은 기준 스냅숏만 저장하고, 픽스처를 바꾼 뒤 다시 실행하면 늘어난·새로 생긴·사라진
# 폴더를 보여 줘야 한다. 마지막으로 JSON Lines 모드의 changes 이벤트를 확인한다.

foreach(var EXE FIXTURE HIST)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "${var} is required")
    endif()
endforeach()

function(run_scanner out_var)
    execute_process(COMMAND "${EXE}" ${ARGN}
                    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 60)
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
file(WRITE "${FIXTURE}/b/inner/three.bin" "${hundred}")
file(TO_NATIVE_PATH "${FIXTURE}" native_fixture)
file(TO_NATIVE_PATH "${HIST}" native_hist)

# 1) 기준 스냅숏
run_scanner(out --track --history-dir "${native_hist}" "${native_fixture}")
expect("${out}" "Total size: +300 bytes")
expect("${out}" "On disk: +")
expect("${out}" "Changes: +no previous snapshot \\(baseline\\)")
expect("${out}" "History: +saved to ")
file(GLOB snaps "${HIST}/*.snap")
list(LENGTH snaps snap_count)
if(NOT snap_count EQUAL 1)
    message(FATAL_ERROR "expected one snapshot in ${HIST}, found ${snap_count}")
endif()

# 2) 변화: a 에 1000 B 추가, c 새 폴더(100 B), b 폴더 삭제(200 B, 하위 inner 는 따로 안 나옴)
file(WRITE "${FIXTURE}/a/more.bin" "${thousand}")
file(WRITE "${FIXTURE}/c/new.bin" "${hundred}")
file(REMOVE_RECURSE "${FIXTURE}/b")
run_scanner(out --track --history-dir "${native_hist}" --threads 1 "${native_fixture}")
expect("${out}" "Changes since [0-9-]+ [0-9:]+: \\+900 B \\(300 B -> 1.17 KiB\\)")
expect("${out}" "grown 1, shrunk 0, added 1, removed 1")
expect("${out}" "\\+1000 B +grown +a\n")
expect("${out}" "-200 B +removed +b\n")
expect("${out}" "\\+100 B +added +c\n")
if(out MATCHES "inner")
    message(FATAL_ERROR "removed subfolder listed separately:\n${out}")
endif()

# 3) JSON Lines: 바로 직전과 같으므로 변화 항목이 없다
run_scanner(out --jsonl --track --history-dir "${native_hist}" "${native_fixture}")
expect("${out}" "\"alloc_bytes\":[0-9]+,\"disk\":{")
expect("${out}" "{\"event\":\"changes\",\"previous\":true,\"since\":[0-9]+,\"old_bytes\":1200,\"new_bytes\":1200,\"items\":\\[\\],\"more\":0,\"saved_to\":\"[^\"]+\\.snap\"}")
expect("${out}" "{\"event\":\"complete\",\"status\":\"complete\",\"exit_code\":0}\n$")

file(REMOVE_RECURSE "${FIXTURE}" "${HIST}")
