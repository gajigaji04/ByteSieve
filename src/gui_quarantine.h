#ifndef BYTESIEVE_GUI_QUARANTINE_H
#define BYTESIEVE_GUI_QUARANTINE_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stddef.h>

/*
 * 격리 폴더 패널(자식 창). 휴지통이 없는 드라이브에서 정리해 격리 폴더(quarantine.h)로
 * 옮긴 항목을 드라이브별로 모아 보여 주고, 고른 항목을 원래 위치로 복원하거나 두 번 확인한 뒤
 * 영구 삭제한다. 복원은 덮어쓰지 않으며, 같은 이름이 있으면 처리 방법을 안내하고 다른 이름으로
 * 복원할지 묻는다. 영구 삭제는 작업 스레드에서 한다.
 */

/* 영구 삭제가 끝나면 부모에게 WM_COMMAND(MAKEWPARAM(id, QPN_PURGED), panel) 를 보낸다. */
#define QPN_PURGED 1

/* 창 클래스를 등록한다. 성공 0. */
int quarantine_panel_register(HINSTANCE inst);

/* 패널을 만든다(처음에는 숨김). 글꼴은 호출자가 소유하며 패널보다 오래 살아야 한다. */
HWND quarantine_panel_create(HWND parent, int id, HFONT font, int dpi);

/*
 * 목록을 다시 읽는다. 영구 삭제 중이면 아무것도 하지 않고 0. 목록을 읽었으면 1.
 * 보통은 모든 고정·이동식 드라이브를 읽는다.
 */
int quarantine_panel_refresh(HWND panel);

/*
 * 자체 검사용: 이 볼륨 루트의 격리 폴더만 보이게 한다(NULL 이면 모든 드라이브).
 * 검사가 사용자의 실제 격리 항목을 건드리지 않게 한다. 목록을 다시 읽는다.
 */
void quarantine_panel_set_root(HWND panel, const wchar_t *volume_root);

/* 영구 삭제 중이면 1. */
int quarantine_panel_busy(HWND panel);

/* 보이는 항목을 모두 선택한다. 선택한 수를 반환한다. */
size_t quarantine_panel_select_all(HWND panel);

/*
 * 선택한 항목을 원래 위치로 복원한다. interactive 가 1 이면 같은 이름이 있는 항목에 대해
 * 처리 방법을 안내하고 다른 이름으로 복원할지 묻고, 실패를 창으로 알린다. 0 이면 묻지 않고
 * 그 항목을 건너뛴다. 복원한 수를 반환한다.
 */
size_t quarantine_panel_restore(HWND panel, int interactive);

/*
 * 선택한 항목의 영구 삭제를 시작한다. confirm 이 1 이면 대상·용량과 복구할 수 없다는 안내를
 * 보여 주고 두 번 확인받는다(기본 버튼은 [아니요]). 시작했으면 1.
 */
int quarantine_panel_purge(HWND panel, int confirm);

/*
 * 자체 검사: 목록이 expect_rows 개이고 그중 정상(복원 가능) 항목이 expect_ok 개이며,
 * 화면(열 글자·요약·버튼 상태)이 목록과 맞는지 확인한다. 실패 수를 반환한다.
 */
int quarantine_panel_verify(HWND panel, size_t expect_rows, size_t expect_ok);

/* 자체 검사: 마지막 복원에서 같은 이름 때문에 건너뛴 항목 수. */
size_t quarantine_panel_conflicts(HWND panel);

#endif /* BYTESIEVE_GUI_QUARANTINE_H */
