# GUI 휴지통 이동 자체 검사(bytesieve-gui --self-test-trash).
#
#   cmake -DEXE=<bytesieve-gui> -DFIXTURE=<빈 폴더로 만들 경로> -P gui_trash_check.cmake
#
# 픽스처를 만들고 GUI 가 탐지 → 모두 선택 → 휴지통 이동 → 화면 검증을 마치게 한 뒤,
# 옮긴 항목은 원래 자리에 없고 나머지 파일과 보호 대상은 그대로인지 확인한다.
# GUI 가 stdout 에 알려 준 휴지통 항목($R… 과 짝인 $I…)만 지워 사용자 휴지통을 정리한다.
#
# FIXTURE\
#   app\package.json, app\src\main.c       (남아야 함)
#   app\node_modules\a\i.js                → 휴지통
#   app\build\o.bin                        → 휴지통
#   app\server.log                         → 휴지통
#   locked\node_modules\locked\            (접근 거부 → 일부 누락 → 보호, 남아야 함)

foreach(var EXE FIXTURE)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "${var} is required")
    endif()
endforeach()

set(LOCKED "${FIXTURE}/locked/node_modules/locked")

function(unlock)
    if(EXISTS "${LOCKED}")
        file(TO_NATIVE_PATH "${LOCKED}" native)
        execute_process(COMMAND icacls "${native}" /remove:d *S-1-1-0 OUTPUT_QUIET ERROR_QUIET)
    endif()
endfunction()

unlock()
file(REMOVE_RECURSE "${FIXTURE}")
file(WRITE "${FIXTURE}/app/package.json" "{}")
file(WRITE "${FIXTURE}/app/src/main.c" "int main(void){return 0;}")
file(WRITE "${FIXTURE}/app/node_modules/a/i.js" "module.exports = 1;")
file(WRITE "${FIXTURE}/app/build/o.bin" "0123456789")
file(WRITE "${FIXTURE}/app/server.log" "log line")
file(WRITE "${FIXTURE}/locked/node_modules/index.js" "x")
file(MAKE_DIRECTORY "${LOCKED}")
file(TO_NATIVE_PATH "${LOCKED}" native_locked)
execute_process(COMMAND icacls "${native_locked}" /deny "*S-1-1-0:(OI)(CI)F"
                RESULT_VARIABLE deny_rc OUTPUT_QUIET ERROR_QUIET)
if(NOT deny_rc EQUAL 0)
    message(FATAL_ERROR "could not deny access to ${LOCKED}")
endif()

file(TO_NATIVE_PATH "${FIXTURE}" native_fixture)
execute_process(COMMAND "${EXE}" --self-test-trash "${native_fixture}"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 110)

# 결과와 상관없이 테스트가 휴지통에 넣은 항목만 지운다.
set(recycled_count 0)
string(REGEX MATCHALL "recycled: [^\r\n]+" lines "${out}")
foreach(line IN LISTS lines)
    string(REGEX REPLACE "^recycled: " "" path "${line}")
    file(TO_CMAKE_PATH "${path}" path)
    if(NOT path MATCHES "/\\$[Rr][Ee][Cc][Yy][Cc][Ll][Ee]\\.[Bb][Ii][Nn]/.+/\\$R[^/]*$")
        message(SEND_ERROR "unexpected recycled path: ${path}")
        continue()
    endif()
    if(NOT EXISTS "${path}")
        message(SEND_ERROR "recycled item not found in recycle bin: ${path}")
    endif()
    string(REGEX REPLACE "/\\$R([^/]*)$" "/$I\\1" info "${path}")
    file(REMOVE_RECURSE "${path}")
    file(REMOVE "${info}")
    math(EXPR recycled_count "${recycled_count} + 1")
endforeach()

set(failed 0)
if(NOT rc EQUAL 0)
    message(SEND_ERROR "bytesieve-gui --self-test-trash exited with ${rc}\n${err}")
    set(failed 1)
endif()
if(NOT recycled_count EQUAL 3)
    message(SEND_ERROR "expected 3 recycled items, got ${recycled_count}\n${out}")
endif()
foreach(gone app/node_modules app/build app/server.log)
    if(EXISTS "${FIXTURE}/${gone}")
        message(SEND_ERROR "${gone} should have been moved to the recycle bin")
    endif()
endforeach()
foreach(kept app/package.json app/src/main.c locked/node_modules/index.js)
    if(NOT EXISTS "${FIXTURE}/${kept}")
        message(SEND_ERROR "${kept} should not have been touched")
    endif()
endforeach()

unlock()
file(REMOVE_RECURSE "${FIXTURE}")
