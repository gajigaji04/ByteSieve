#ifndef BYTESIEVE_GUI_CLEANUP_H
#define BYTESIEVE_GUI_CLEANUP_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/*
 * 정리 후보(개발 캐시·빌드 결과물·임시·로그) 탐지·정리 패널(자식 창).
 * 경로 입력·찾아보기·탐지·취소, 진행률, 유형 필터, 용량 내림차순 체크 목록,
 * 선택 요약과 [휴지통으로 이동]으로 이루어진다.
 * 탐지는 작업 스레드(cleanup_job)에서 읽기 전용으로 하고, 정리는 사용자가 고르고 확인한
 * 항목만 작업 스레드(cleanup_trash)에서 Windows 휴지통으로 옮긴다(영구 삭제 없음).
 * 휴지통이 없는 드라이브의 항목은 같은 드라이브의 격리 폴더로 옮긴다(gui_quarantine).
 */

/* 탐지가 끝나면 부모에게 WM_COMMAND(MAKEWPARAM(id, CPN_FINISHED), panel) 를 보낸다. */
#define CPN_FINISHED 1
/* 휴지통 이동이 끝나면 부모에게 WM_COMMAND(MAKEWPARAM(id, CPN_TRASHED), panel) 를 보낸다. */
#define CPN_TRASHED 2

/* 창 클래스를 등록한다. 성공 0. */
int cleanup_panel_register(HINSTANCE inst);

/* 패널을 만든다(처음에는 숨김). 글꼴은 호출자가 소유하며 패널보다 오래 살아야 한다. */
HWND cleanup_panel_create(HWND parent, int id, HFONT font, int dpi);

/*
 * 탐지를 시작한다. root 가 NULL 이면 경로 칸의 값을 쓰고, 아니면 경로 칸을 root 로 채운다.
 * 시작했으면 1, 이미 진행 중이거나 경로가 비었거나 시작하지 못했으면 0.
 */
int cleanup_panel_start(HWND panel, const wchar_t *root);

/* 진행 중인 탐지를 취소한다(기다리지 않음). */
void cleanup_panel_cancel(HWND panel);

/* 탐지 중이거나 휴지통으로 옮기는 중이면 1. */
int cleanup_panel_busy(HWND panel);

/*
 * 현재 필터에 보이는 항목 중 옮길 수 있는 것(보호 대상·이미 옮긴 항목 제외)을 모두
 * 선택(check=1)하거나 보이는 항목을 모두 해제(check=0)한다. 전체 선택 개수를 반환한다.
 */
size_t cleanup_panel_check_all(HWND panel, int check);

/*
 * 선택한 항목을 휴지통으로 옮기기 시작한다. confirm 이 1 이면 항목의 경로·유형·용량과
 * 예상 확보 용량을 보여 주는 확인 창을 띄우고 [예]일 때만 시작한다(기본 버튼은 [아니요]).
 * 시작했으면 1, 선택이 없거나 사용자가 거절했거나 바쁘거나 시작하지 못했으면 0.
 */
int cleanup_panel_trash(HWND panel, int confirm);

/*
 * 자체 검사: 목록·합계·정렬·필터가 결과와 맞는지 확인한다. 실패한 항목 수를 반환한다.
 * expect_cancelled 가 1 이면 취소 직후 상태를, 0 이면 끝까지 탐지한 상태를 기대한다.
 */
int cleanup_panel_verify(HWND panel, int expect_cancelled);

/*
 * 자체 검사: 휴지통 이동이 끝난 뒤 선택한 expect_moved 개가 모두 옮겨져 원래 경로에 없고,
 * 목록·선택 요약·상태 줄이 결과와 맞는지 확인한다. 옮긴 항목의 휴지통(또는 격리 폴더) 안
 * 위치를 stdout 에 "recycled: <경로>" 줄로 쓴다(검사 스크립트가 정리용으로 읽음).
 * 실패 수를 반환한다.
 */
int cleanup_panel_verify_trash(HWND panel, size_t expect_moved);

#endif /* BYTESIEVE_GUI_CLEANUP_H */
