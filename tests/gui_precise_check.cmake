# GUI 정밀 스캔(하드 링크 중복 보정) 자체 검사(bytesieve-gui --self-test-precise / --self-test).
#
#   cmake -DEXE=<bytesieve-gui> -DFIXTURE=<빈 폴더로 만들 경로> -DHIST=<기록 폴더> -P gui_precise_check.cmake
#
# 하드 링크가 있는 픽스처를 정밀 스캔해 보정 열·보기·분석 줄을 검증하고 stdout 의 집계를 확인한 뒤,
# 같은 픽스처를 기본 스캔해 보정 열이 숨겨지고 기본 동작이 그대로인지 확인한다.
# 스냅숏은 HIST 에만 쓴다(BYTESIEVE_HISTORY_DIR).
#
# FIXTURE\
#   a\data.bin 1000, a\copy.bin = data.bin 의 하드 링크
#   b\link.bin = a\data.bin 의 하드 링크, b\own.bin 300
# 경로마다: 파일 4개 3300 B. 실제 파일 2개 1300 B. 중복 경로 2개 2000 B.

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

file(REMOVE_RECURSE "${FIXTURE}" "${HIST}")
string(REPEAT "x" 1000 thousand)
string(REPEAT "y" 300 three_hundred)
file(WRITE "${FIXTURE}/a/data.bin" "${thousand}")
file(WRITE "${FIXTURE}/b/own.bin" "${three_hundred}")
file(TO_NATIVE_PATH "${FIXTURE}" native_fixture)
foreach(pair "a\\copy.bin" "b\\link.bin")
    execute_process(COMMAND cmd /c mklink /H "${native_fixture}\\${pair}"
                            "${native_fixture}\\a\\data.bin"
                    RESULT_VARIABLE link_rc OUTPUT_QUIET ERROR_QUIET)
    if(NOT link_rc EQUAL 0)
        message(FATAL_ERROR "could not create hard link ${pair} (fixture must be on NTFS)")
    endif()
endforeach()

# 1) 정밀 스캔: 기본 집계(경로마다)는 그대로, 중복 경로 2개 2000 B 를 뺀 1300 B
run_gui(out --self-test-precise "${native_fixture}")
if(NOT out MATCHES "precise: files 4 bytes 3300 dup_files 2 dup_bytes 2000 unique 1300 unknown 0\n")
    message(FATAL_ERROR "unexpected precise scan result:\n${out}")
endif()

# 2) 기본 스캔: 보정 열이 숨겨지고 정밀 스캔 요약이 없다(화면 검증은 GUI 가 한다)
run_gui(out --self-test "${native_fixture}")

file(REMOVE_RECURSE "${FIXTURE}" "${HIST}")
