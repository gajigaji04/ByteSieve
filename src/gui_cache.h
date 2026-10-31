#ifndef BYTESIEVE_GUI_CACHE_H
#define BYTESIEVE_GUI_CACHE_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/*
 * 공용 개발 캐시(npm·pip·Gradle·Cargo·NuGet) 탐지·정리 패널(자식 창). 프로젝트 안의 캐시를 찾는
 * [정리 후보] 탭과 따로, 사용자 폴더에 하나씩 있는 도구 캐시를 보여 준다.
 * [다시 탐지]·취소, 진행률, 체크 목록(종류·캐시·경로·용량·파일 수·정리 가능 여부·위치 근거·주의 사항,
 * 머리글을 눌러 정렬, 기본은 용량 내림차순), 고른 항목의 주의 사항, 선택 요약과 [휴지통으로 이동]
 * 으로 이루어진다. 탐지는 작업 스레드(dev_cache)에서 읽기 전용으로 하고, 정리는 사용자가 고르고
 * 확인한 항목만 [정리 후보] 탭과 같은 안전 장치(cleanup_trash)를 거쳐 휴지통(없는 드라이브는 격리
 * 폴더)으로 옮긴다. 영구 삭제하지 않는다.
 */

/* 탐지가 끝나면 부모에게 WM_COMMAND(MAKEWPARAM(id, KPN_FINISHED), panel) 를 보낸다. */
#define KPN_FINISHED 1
/* 휴지통 이동이 끝나면 부모에게 WM_COMMAND(MAKEWPARAM(id, KPN_TRASHED), panel) 를 보낸다. */
#define KPN_TRASHED 2

/* 창 클래스를 등록한다. 성공 0. */
int cache_panel_register(HINSTANCE inst);

/* 패널을 만든다(처음에는 숨김). 글꼴은 호출자가 소유하며 패널보다 오래 살아야 한다. */
HWND cache_panel_create(HWND parent, int id, HFONT font, int dpi);

/* 탐지를 시작한다. 시작했으면 1, 바쁘거나 시작하지 못했으면 0. */
int cache_panel_refresh(HWND panel);

/* 한 번이라도 탐지를 시작했으면 1(탭을 처음 열 때만 자동으로 탐지하는 데 쓴다). */
int cache_panel_loaded(HWND panel);

/* 진행 중인 탐지·휴지통 이동을 취소한다(기다리지 않음). */
void cache_panel_cancel(HWND panel);

/* 탐지 중이거나 휴지통으로 옮기는 중이면 1. */
int cache_panel_busy(HWND panel);

/* 정리할 수 있는 항목을 모두 선택(check=1)하거나 모두 해제(check=0)한다. 선택 개수를 반환한다. */
size_t cache_panel_check_all(HWND panel, int check);

/*
 * 선택한 캐시를 휴지통으로 옮기기 시작한다. confirm 이 1 이면 종류·용량·경로·주의 사항을 보여 주는
 * 확인 창을 띄우고 [예]일 때만 시작한다(기본 버튼은 [아니요]).
 * 시작했으면 1, 선택이 없거나 사용자가 거절했거나 바쁘거나 시작하지 못했으면 0.
 */
int cache_panel_trash(HWND panel, int confirm);

/*
 * 자체 검사: 탐지가 끝난 목록이 결과와 맞는지(정렬·칸·정리 가능 여부), 그리고 찾은 캐시가 모두
 * expect_root 아래에 있는지 확인한다(아니면 실제 사용자 캐시를 볼 수 있으므로 정리하면 안 된다).
 * 실패 수를 반환한다.
 */
int cache_panel_verify(HWND panel, const wchar_t *expect_root);

/*
 * 자체 검사: 휴지통 이동이 끝난 뒤 선택한 expect_moved 개가 모두 옮겨져 원래 경로에 없고, 정리할 수
 * 없던 항목은 그대로인지 확인한다. 옮긴 항목의 휴지통 안 위치를 stdout 에 "recycled: <경로>" 로 쓴다.
 * 실패 수를 반환한다.
 */
int cache_panel_verify_trash(HWND panel, size_t expect_moved);

#endif /* BYTESIEVE_GUI_CACHE_H */
