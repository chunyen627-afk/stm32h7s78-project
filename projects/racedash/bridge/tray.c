/* 系統匣圖示：背景執行時唯一看得到的東西。滑鼠移上去顯示狀態，右鍵選單可以
 * 開紀錄檔或結束。主迴圈每圈呼叫 tray_pump() 處理視窗訊息。 */
#include "bridge.h"

#include <windows.h>
#include <shellapi.h>
#include <stdio.h>

#define WM_TRAY      (WM_APP + 1)
#define ID_OPEN_LOG  1
#define ID_EXIT      2

static HWND            s_wnd;
static NOTIFYICONDATAW s_nid;
static bool            s_quit;
static wchar_t         s_log_path[MAX_PATH];

static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_TRAY && (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU)) {
        POINT pt;
        HMENU m = CreatePopupMenu();
        AppendMenuW(m, MF_STRING, ID_OPEN_LOG, L"開啟紀錄檔");
        AppendMenuW(m, MF_SEPARATOR, 0, NULL);
        AppendMenuW(m, MF_STRING, ID_EXIT, L"結束 RaceDash Bridge");
        GetCursorPos(&pt);
        SetForegroundWindow(h);
        TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, h, NULL);
        DestroyMenu(m);
        return 0;
    }
    if (msg == WM_COMMAND) {
        if (LOWORD(wp) == ID_EXIT) { s_quit = true; }
        if (LOWORD(wp) == ID_OPEN_LOG) { ShellExecuteW(NULL, L"open", s_log_path, NULL, NULL, SW_SHOWNORMAL); }
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void tray_init(const char *log_path_utf8)
{
    WNDCLASSW wc = {0};

    MultiByteToWideChar(CP_UTF8, 0, log_path_utf8, -1, s_log_path, MAX_PATH);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"RaceDashBridgeTray";
    RegisterClassW(&wc);
    s_wnd = CreateWindowW(wc.lpszClassName, L"RaceDash Bridge", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, wc.hInstance, NULL);

    s_nid.cbSize = sizeof(s_nid);
    s_nid.hWnd = s_wnd;
    s_nid.uID = 1;
    s_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    s_nid.uCallbackMessage = WM_TRAY;
    s_nid.hIcon = LoadIconW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(1));
    if (!s_nid.hIcon) { s_nid.hIcon = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION); }
    wcscpy(s_nid.szTip, L"RaceDash Bridge");
    Shell_NotifyIconW(NIM_ADD, &s_nid);
}

/* 狀態文字（滑鼠移到圖示上看到的） */
void tray_status(const char *utf8)
{
    if (!s_wnd) { return; }
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, s_nid.szTip, sizeof(s_nid.szTip) / sizeof(wchar_t));
    s_nid.szTip[sizeof(s_nid.szTip) / sizeof(wchar_t) - 1] = 0;
    s_nid.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &s_nid);
}

/* 處理視窗訊息；回 true = 使用者選了結束 */
bool tray_pump(void)
{
    MSG m;
    while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return s_quit;
}

void tray_close(void)
{
    if (s_wnd) { Shell_NotifyIconW(NIM_DELETE, &s_nid); }
}
