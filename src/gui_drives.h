#ifndef BYTESIEVE_GUI_DRIVES_H
#define BYTESIEVE_GUI_DRIVES_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/*
 * 드라이브 사용량 대시보드 패널(자식 창).
 * [새로고침] 버튼, 상태 줄, 드라이브별 용량·사용률 막대 목록으로 이루어진다.
 * 조회는 작업 스레드에서 하므로 응답이 느린 네트워크·이동식 드라이브가 있어도 UI 가 멈추지 않는다.
 */

/* 새로고침이 끝나면 부모에게 WM_COMMAND(MAKEWPARAM(id, DPN_REFRESHED), panel) 를 보낸다. */
#define DPN_REFRESHED 1

/* 창 클래스를 등록한다. 성공 0. */
int drive_panel_register(HINSTANCE inst);

/* 패널을 만든다(처음에는 숨김). 글꼴은 호출자가 소유하며 패널보다 오래 살아야 한다. */
HWND drive_panel_create(HWND parent, int id, HFONT font, int dpi);

/* 드라이브 목록을 다시 조회한다. 이미 조회 중이면 아무것도 하지 않는다. */
void drive_panel_refresh(HWND panel);

/* 한 번이라도 조회를 마쳤으면 1. */
int drive_panel_loaded(HWND panel);

/* 자체 검사: 화면 목록이 시스템 드라이브 정보와 맞는지 확인한다. 실패한 항목 수를 반환한다. */
int drive_panel_verify(HWND panel);

#endif /* BYTESIEVE_GUI_DRIVES_H */
