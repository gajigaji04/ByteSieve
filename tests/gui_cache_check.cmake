# GUI 공용 캐시 탭 자체 검사(bytesieve-gui --self-test-cache).
#
#   cmake -DEXE=<bytesieve-gui> -DFIXTURE=<빈 폴더로 만들 경로> -P gui_cache_check.cmake
#
# USERPROFILE·LOCALAPPDATA·APPDATA 를 픽스처로 돌리고 도구 환경 변수를 지워 실제 캐시를 보지 않게
# 한 뒤, GUI 가 탐지 → 화면 확인(찾은 캐시가 모두 픽스처 안인지 포함) → 정리 가능한 캐시를 모두
# 휴지통으로 옮기게 한다. 옮긴 캐시는 원래 자리에 없고, 도구 폴더의 설정·설치한 도구와 캐시 구조가
# 아닌 폴더(NUGET_HTTP_CACHE_PATH 가 가리킴)는 그대로인지 확인한다.
# GUI 가 stdout 에 알려 준 휴지통 항목($R… 과 짝인 $I…)만 지워 사용자 휴지통을 정리한다.
#
# FIXTURE\
#   home\.gradle\caches\modules-2\…             → 휴지통
#   home\.gradle\wrapper\dists\gradle-8.5-bin\… → 휴지통
#   home\.gradle\gradle.properties              (남아야 함)
#   home\.cargo\registry\cache\…, git\db\…       → 휴지통
#   home\.cargo\bin\cargo.exe                   (남아야 함)
#   home\.nuget\packages\serilog\3.0.0\…         → 휴지통
#   local\npm-cache\_cacache\…                   → 휴지통
#   local\pip\Cache\http-v2\…                    → 휴지통
#   mydocs\report.docx                          (캐시 구조 아님, 남아야 함)

foreach(var EXE FIXTURE)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "${var} is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${FIXTURE}")
string(REPEAT "x" 100 hundred)
file(WRITE "${FIXTURE}/home/.gradle/caches/modules-2/files-2.1/m.jar" "${hundred}${hundred}${hundred}")
file(WRITE "${FIXTURE}/home/.gradle/wrapper/dists/gradle-8.5-bin/g.zip" "${hundred}${hundred}")
file(WRITE "${FIXTURE}/home/.gradle/gradle.properties" "secret=keep")
file(WRITE "${FIXTURE}/home/.cargo/registry/cache/c.crate" "${hundred}")
file(WRITE "${FIXTURE}/home/.cargo/git/db/d" "git")
file(WRITE "${FIXTURE}/home/.cargo/bin/cargo.exe" "keep")
file(WRITE "${FIXTURE}/home/.nuget/packages/serilog/3.0.0/.nupkg.metadata" "{}")
file(WRITE "${FIXTURE}/local/npm-cache/_cacache/index-v5/a" "${hundred}${hundred}")
file(WRITE "${FIXTURE}/local/pip/Cache/http-v2/b" "${hundred}")
file(WRITE "${FIXTURE}/mydocs/report.docx" "doc")
file(MAKE_DIRECTORY "${FIXTURE}/roaming")

file(TO_NATIVE_PATH "${FIXTURE}" native_fixture)
file(TO_NATIVE_PATH "${FIXTURE}/home" native_home)
file(TO_NATIVE_PATH "${FIXTURE}/local" native_local)
file(TO_NATIVE_PATH "${FIXTURE}/roaming" native_roaming)
file(TO_NATIVE_PATH "${FIXTURE}/mydocs" native_docs)
execute_process(COMMAND ${CMAKE_COMMAND} -E env
                        --unset=npm_config_cache --unset=NPM_CONFIG_USERCONFIG
                        --unset=PIP_CACHE_DIR --unset=PIP_CONFIG_FILE --unset=GRADLE_USER_HOME
                        --unset=CARGO_HOME --unset=NUGET_PACKAGES
                        "USERPROFILE=${native_home}" "LOCALAPPDATA=${native_local}"
                        "APPDATA=${native_roaming}" "NUGET_HTTP_CACHE_PATH=${native_docs}"
                        "${EXE}" --self-test-cache "${native_fixture}"
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

if(NOT rc EQUAL 0)
    message(SEND_ERROR "bytesieve-gui --self-test-cache exited with ${rc}\n${err}")
endif()
if(NOT recycled_count EQUAL 7)
    message(SEND_ERROR "expected 7 recycled caches, got ${recycled_count}\n${out}")
endif()
foreach(gone home/.gradle/caches home/.gradle/wrapper/dists home/.cargo/registry home/.cargo/git
             home/.nuget/packages local/npm-cache local/pip/Cache)
    if(EXISTS "${FIXTURE}/${gone}")
        message(SEND_ERROR "${gone} should have been moved to the recycle bin")
    endif()
endforeach()
foreach(kept home/.gradle/gradle.properties home/.cargo/bin/cargo.exe mydocs/report.docx)
    if(NOT EXISTS "${FIXTURE}/${kept}")
        message(SEND_ERROR "${kept} must not be touched")
    endif()
endforeach()

file(REMOVE_RECURSE "${FIXTURE}")
