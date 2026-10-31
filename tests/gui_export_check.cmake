# GUI 스캔 결과 내보내기 자체 검사(bytesieve-gui --self-test-export).
#
#   cmake -DEXE=<bytesieve-gui> -DFIXTURE=<빈 폴더로 만들 경로> -DOUT=<출력 폴더> -DHIST=<기록 폴더>
#         -P gui_export_check.cmake
#
# 픽스처를 스캔한 뒤 GUI 가 [CSV 내보내기]·[HTML 보고서]와 같은 경로로 OUT/result.csv·result.html 을
# 쓰게 하고(저장 대화 상자 없이), 파일 내용(BOM·한글·행 수·순위·이스케이프)을 확인한다.
# 스냅숏은 HIST 에만 쓴다(BYTESIEVE_HISTORY_DIR).
#
# FIXTURE\
#   big\a.bin 2000, big\sub\b.bin 1000
#   R&D 한글\h.txt 500
#   =cmd\x.txt 10

foreach(var EXE FIXTURE OUT HIST)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "${var} is required")
    endif()
endforeach()

function(expect text regex what)
    if(NOT text MATCHES "${regex}")
        message(FATAL_ERROR "${what} does not match '${regex}'")
    endif()
endfunction()

file(REMOVE_RECURSE "${FIXTURE}" "${OUT}" "${HIST}")
string(REPEAT "x" 1000 thousand)
string(REPEAT "y" 500 five_hundred)
file(WRITE "${FIXTURE}/big/a.bin" "${thousand}${thousand}")
file(WRITE "${FIXTURE}/big/sub/b.bin" "${thousand}")
file(WRITE "${FIXTURE}/R&D 한글/h.txt" "${five_hundred}")
file(WRITE "${FIXTURE}/=cmd/x.txt" "0123456789")
file(MAKE_DIRECTORY "${OUT}")
file(TO_NATIVE_PATH "${FIXTURE}" native_fixture)
file(TO_NATIVE_PATH "${OUT}/result" native_prefix)
file(TO_NATIVE_PATH "${HIST}" native_hist)

execute_process(COMMAND ${CMAKE_COMMAND} -E env "BYTESIEVE_HISTORY_DIR=${native_hist}"
                        "${EXE}" --self-test-export "${native_fixture}" "${native_prefix}"
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 110)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "exit code ${rc}\nstdout:\n${out}\nstderr:\n${err}")
endif()
# 루트 + big, sub, R&D 한글, =cmd
expect("${out}" "exported folders: 5\n" "stdout")

# CSV: UTF-8 BOM, 머리글 + 5행(CRLF), 크기 내림차순, 수식 방지
file(READ "${OUT}/result.csv" csv_hex HEX LIMIT 3)
if(NOT csv_hex STREQUAL "efbbbf")
    message(FATAL_ERROR "CSV does not start with a UTF-8 BOM: ${csv_hex}")
endif()
# 줄 수: file(READ) 는 CR 을 빼고 읽으므로 바이트(HEX)로 CR·LF 를 센다(CRLF 짝은 단위 테스트가 확인).
file(READ "${OUT}/result.csv" csv_hex HEX)
string(REGEX MATCHALL ".." csv_bytes "${csv_hex}")
set(cr_list ${csv_bytes})
set(lf_list ${csv_bytes})
list(FILTER cr_list INCLUDE REGEX "^0d$")
list(FILTER lf_list INCLUDE REGEX "^0a$")
list(LENGTH cr_list cr_count)
list(LENGTH lf_list lf_count)
if(NOT cr_count EQUAL 6 OR NOT lf_count EQUAL 6)
    message(FATAL_ERROR "CSV should have 6 CRLF lines (header + 5 folders), found ${cr_count} CR / "
                        "${lf_count} LF")
endif()
file(READ "${OUT}/result.csv" csv)
expect("${csv}" "경로,폴더명,깊이,크기\\(바이트\\),크기," "CSV header")
expect("${csv}" ",\"big\",1,3000,\"2\\.93 KiB\",[0-9]+,2,1,1,2000,[0-9.]+,\"정상\"\n[^\n]*,\"sub\",2,1000,"
       "CSV big/sub rows")
expect("${csv}" ",\"sub\",.*,\"R&D 한글\",1,500,.*,\"'=cmd\",1,10," "CSV order")

# HTML: 구역, 이스케이프, 순위
file(READ "${OUT}/result.html" html)
expect("${html}" "^<!DOCTYPE html>" "HTML")
expect("${html}" "<h2 id=\"summary\">전체 요약</h2>" "HTML summary")
expect("${html}" "<h2 id=\"stats\">파일·폴더 통계</h2>" "HTML stats")
expect("${html}" "<h2 id=\"rank\">폴더별 용량 순위\\(상위 4개\\)</h2>" "HTML rank heading")
expect("${html}" "R&amp;D 한글" "HTML escaping")
expect("${html}" "<td class=\"n\">1</td><td class=\"p\">[^<]*big</td><td class=\"n\" title=\"3,000 B\">"
       "HTML rank 1")
expect("${html}" "<th scope=\"row\">파일 수</th><td>4</td>" "HTML file count")
if(html MATCHES "R&D")
    message(FATAL_ERROR "HTML contains an unescaped folder name")
endif()

# 실패한 내보내기(없는 폴더)는 아무것도 남기지 않는다.
if(EXISTS "${OUT}/result-missing")
    message(FATAL_ERROR "failed export created a folder")
endif()
file(GLOB leftovers "${OUT}/*.tmp")
if(leftovers)
    message(FATAL_ERROR "temporary files left: ${leftovers}")
endif()

file(REMOVE_RECURSE "${FIXTURE}" "${OUT}" "${HIST}")
