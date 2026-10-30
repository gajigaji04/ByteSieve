#ifndef BYTESIEVE_GUI_CLEANUP_H
#define BYTESIEVE_GUI_CLEANUP_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/*
 * 정리 후보(개발 캐시·빌드 결과물·임시·로그) 탐지 패널(자식 창).
 * 경로 입력·찾아보기·탐지·취소, 진행률, 유형 필터, 용량 내림차순 목록으로 이루어진다.
 * 탐지는 작업 스레드(cleanup_job)에서 하며 파일을 삭제·수정하지 않는다.
 */

/* 탐지가 끝나면 부모에게 WM_COMMAND(MAKEWPARAM(id, CPN_FINISHED), panel) 를 보낸다. */
#define CPN_FINISHED 1

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

/* 탐지 중이면 1. */
int cleanup_panel_busy(HWND panel);

/*
 * 자체 검사: 목록·합계·정렬·필터가 결과와 맞는지 확인한다. 실패한 항목 수를 반환한다.
 * expect_cancelled 가 1 이면 취소 직후 상태를, 0 이면 끝까지 탐지한 상태를 기대한다.
 */
int cleanup_panel_verify(HWND panel, int expect_cancelled);

#endif /* BYTESIEVE_GUI_CLEANUP_H */
