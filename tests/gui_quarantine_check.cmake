# GUI 격리 폴더 자체 검사(bytesieve-gui --self-test-quarantine).
#
#   cmake -DEXE=<bytesieve-gui> -DFIXTURE=<빈 폴더로 만들 경로> -P gui_quarantine_check.cmake
#
# BYTESIEVE_QUARANTINE_ROOT=FIXTURE 로 픽스처를 "휴지통이 없는 드라이브"처럼 다루게 한 뒤,
# GUI 가 탐지 → 모두 선택 → 정리(격리 폴더로) → 격리 폴더 탭 확인 → 같은 이름 파일을 만든 뒤
# 모두 복원(그 항목만 건너뜀) → 남은 항목 영구 삭제를 마치게 한다. 그 뒤:
#   - 복원한 항목은 원래 자리에 내용 그대로 돌아왔고
#   - 같은 이름이 있던 파일은 덮어쓰이지 않았고(격리 사본만 영구 삭제됨)
#   - 격리 폴더에는 남은 항목이 없고, 사용자 휴지통은 쓰지 않았고
#   - 나머지 파일과 보호 대상은 그대로인지 확인한다.
#
# FIXTURE\
#   app\package.json, app\src\main.c       (남아야 함)
#   app\node_modules\a\i.js                → 격리 → 복원
#   app\build\o.bin                        → 격리 → 복원
#   app\server.log                         → 격리 → 같은 이름("conflict")이 생겨 복원 건너뜀 → 영구 삭제
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
file(TO_NATIVE_PATH "${FIXTURE}/app/server.log" native_conflict)
set(ENV{BYTESIEVE_QUARANTINE_ROOT} "${native_fixture}")
execute_process(COMMAND "${EXE}" --self-test-quarantine "${native_fixture}" "${native_conflict}"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 110)
unset(ENV{BYTESIEVE_QUARANTINE_ROOT})

if(NOT rc EQUAL 0)
    message(SEND_ERROR "bytesieve-gui --self-test-quarantine exited with ${rc}\n${err}\n${out}")
endif()

# 정리한 3개는 모두 픽스처의 격리 폴더로 갔어야 한다(휴지통 아님).
string(REGEX MATCHALL "recycled: [^\r\n]+" lines "${out}")
list(LENGTH lines moved_count)
if(NOT moved_count EQUAL 3)
    message(SEND_ERROR "expected 3 quarantined items, got ${moved_count}\n${out}")
endif()
foreach(line IN LISTS lines)
    string(REGEX REPLACE "^recycled: " "" path "${line}")
    file(TO_CMAKE_PATH "${path}" path)
    string(FIND "${path}" "${FIXTURE}/.bytesieve-trash/" at)
    if(NOT at EQUAL 0)
        message(SEND_ERROR "item was not moved to the fixture quarantine folder: ${path}")
    endif()
endforeach()

# 복원된 항목
file(READ "${FIXTURE}/app/node_modules/a/i.js" restored_js)
file(READ "${FIXTURE}/app/build/o.bin" restored_bin)
if(NOT restored_js STREQUAL "module.exports = 1;" OR NOT restored_bin STREQUAL "0123456789")
    message(SEND_ERROR "restored items do not match the originals")
endif()
# 같은 이름이 있던 파일은 덮어쓰지 않았다
file(READ "${FIXTURE}/app/server.log" kept_log)
if(NOT kept_log STREQUAL "conflict")
    message(SEND_ERROR "app/server.log was overwritten: ${kept_log}")
endif()
# 격리 폴더에 남은 항목이 없다(영구 삭제됨)
file(GLOB left LIST_DIRECTORIES true "${FIXTURE}/.bytesieve-trash/*")
if(left)
    message(SEND_ERROR "quarantine folder is not empty: ${left}")
endif()
foreach(kept app/package.json app/src/main.c locked/node_modules/index.js)
    if(NOT EXISTS "${FIXTURE}/${kept}")
        message(SEND_ERROR "${kept} should not have been touched")
    endif()
endforeach()

unlock()
file(REMOVE_RECURSE "${FIXTURE}")
