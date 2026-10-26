#define _WIN32_WINNT 0x0600

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <commctrl.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "comctl32.lib")

#define APP_NAME        L"TCPing_GUI"
#define DEFAULT_PORT    38000
#define PORT_FALLBACK   100
#define RUN_SECONDS     50

#define ID_TRAY_ICON    1001
#define WM_TRAYICON     (WM_APP + 1)
#define WM_APP_LOG      (WM_APP + 2)
#define WM_APP_ROW      (WM_APP + 3)
#define WM_APP_DONE     (WM_APP + 4)

#define ID_MENU_SHOW    2001
#define ID_MENU_START   2002
#define ID_MENU_STOP    2003
#define ID_MENU_EXIT    2004

#define ID_EDIT_A_HOST  101
#define ID_EDIT_A_PORT  102
#define ID_EDIT_B_HOST  103
#define ID_EDIT_B_PORT  104
#define ID_EDIT_DUR     105
#define ID_BTN_START    201
#define ID_BTN_STOP     202
#define ID_LIST         301
#define ID_LOG_OP       401
#define ID_LOG_DBG      402
#define ID_STATUS       500

typedef struct {
    char time[32];
    double a_ms;
    double b_ms;
    int has_a;
    int has_b;
    char note[128];
} RowItem;

typedef struct {
    char target[256];
    char port[32];
} TargetCfg;

static HINSTANCE g_hInst = NULL;
static HWND g_hWnd = NULL;
static HWND g_hEditAHost = NULL;
static HWND g_hEditAPort = NULL;
static HWND g_hEditBHost = NULL;
static HWND g_hEditBPort = NULL;
static HWND g_hEditDur = NULL;
static HWND g_hLogOp = NULL;
static HWND g_hLogDbg = NULL;
static HWND g_hList = NULL;
static HWND g_hStatus = NULL;

static NOTIFYICONDATAW g_nid = {0};
static HANDLE g_worker = NULL;
static HANDLE g_stop_event = NULL;

static volatile LONG g_running = 0;
static volatile LONG g_exit_app = 0;

static CRITICAL_SECTION g_log_cs;
static CRITICAL_SECTION g_data_cs;

static char g_log_op[32768] = {0};
static char g_log_dbg[32768] = {0};

static RowItem g_rows[1024];
static int g_row_count = 0;
static char g_last_log[256] = "-";

static TargetCfg g_cfg_a = {"1.1.1.1", "80"};
static TargetCfg g_cfg_b = {"8.8.8.8", "53"};
static int g_duration_sec = RUN_SECONDS;

static void utf8_to_wide(const char* s, wchar_t* out, int outc) {
    MultiByteToWideChar(CP_UTF8, 0, s, -1, out, outc);
}

static void wide_to_utf8(const wchar_t* s, char* out, int outc) {
    WideCharToMultiByte(CP_UTF8, 0, s, -1, out, outc, NULL, NULL);
}

static void append_text(char* buf, size_t cap, const char* line) {
    size_t len = strlen(buf);
    size_t add = strlen(line);
    if (len + add + 1 >= cap) return;
    memcpy(buf + len, line, add);
    buf[len + add] = 0;
}

static void log_op(const char* fmt, ...) {
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    EnterCriticalSection(&g_log_cs);
    append_text(g_log_op, sizeof(g_log_op), line);
    append_text(g_log_op, sizeof(g_log_op), "\r\n");
    strncpy(g_last_log, line, sizeof(g_last_log) - 1);
    g_last_log[sizeof(g_last_log) - 1] = 0;
    LeaveCriticalSection(&g_log_cs);

    if (g_hWnd) PostMessageW(g_hWnd, WM_APP_LOG, 0, 0);
}

static void log_dbg(const char* fmt, ...) {
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    EnterCriticalSection(&g_log_cs);
    append_text(g_log_dbg, sizeof(g_log_dbg), line);
    append_text(g_log_dbg, sizeof(g_log_dbg), "\r\n");
    LeaveCriticalSection(&g_log_cs);

    if (g_hWnd) PostMessageW(g_hWnd, WM_APP_LOG, 0, 0);
}

static void ui_status_update(void) {
    if (!g_hStatus) return;

    char text[256];
    EnterCriticalSection(&g_data_cs);
    _snprintf(text, sizeof(text), "Running: %s | A:%s:%s | B:%s:%s | Last: %s",
              g_running ? "Yes" : "No",
              g_cfg_a.target, g_cfg_a.port,
              g_cfg_b.target, g_cfg_b.port,
              g_last_log);
    LeaveCriticalSection(&g_data_cs);

    wchar_t w[512];
    utf8_to_wide(text, w, 512);
    SendMessageW(g_hStatus, SB_SETTEXTW, 0, (LPARAM)w);
}

static void add_row(const char* t, double a_ms, int has_a, double b_ms, int has_b, const char* note) {
    EnterCriticalSection(&g_data_cs);
    if (g_row_count < 1024) {
        RowItem* r = &g_rows[g_row_count++];
        ZeroMemory(r, sizeof(*r));
        strncpy(r->time, t, sizeof(r->time) - 1);
        r->a_ms = a_ms;
        r->b_ms = b_ms;
        r->has_a = has_a;
        r->has_b = has_b;
        strncpy(r->note, note ? note : "", sizeof(r->note) - 1);
    }
    LeaveCriticalSection(&g_data_cs);

    if (g_hWnd) PostMessageW(g_hWnd, WM_APP_ROW, 0, 0);
}

static void clear_rows(void) {
    EnterCriticalSection(&g_data_cs);
    g_row_count = 0;
    ZeroMemory(g_rows, sizeof(g_rows));
    LeaveCriticalSection(&g_data_cs);
}

static int get_edit_text(HWND h, char* out, int cap) {
    wchar_t w[256];
    GetWindowTextW(h, w, 256);
    wide_to_utf8(w, out, cap);
    return (int)strlen(out);
}

static double now_ms(void) {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

static void sleep_ms(int ms) {
    Sleep(ms);
}

static int ping_once(const char* host, const char* port, int timeout_ms, double* out_ms) {
    struct addrinfo hints, *res = NULL, *p = NULL;
    SOCKET s = INVALID_SOCKET;
    char tbuf[64];
    int ret = 0;
    double t0, t1;

    ZeroMemory(&hints, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;

    if (getaddrinfo(host, port, &hints, &res) != 0) return 0;

    for (p = res; p; p = p->ai_next) {
        s = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (s == INVALID_SOCKET) continue;

        u_long nb = 1;
        ioctlsocket(s, FIONBIO, &nb);

        t0 = now_ms();
        connect(s, p->ai_addr, (int)p->ai_addrlen);

        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(s, &wfds);

        struct timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;

        if (select(0, NULL, &wfds, NULL, &tv) > 0) {
            int err = 0;
            int len = sizeof(err);
            getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&err, &len);
            if (err == 0) {
                t1 = now_ms();
                *out_ms = t1 - t0;
                ret = 1;
                closesocket(s);
                break;
            }
        }

        closesocket(s);
        s = INVALID_SOCKET;
    }

    if (res) freeaddrinfo(res);
    return ret;
}

static DWORD WINAPI worker_thread(LPVOID lp) {
    (void)lp;

    int sec = 0;
    int total = g_duration_sec > 0 ? g_duration_sec : RUN_SECONDS;
    char timebuf[32];

    InterlockedExchange(&g_running, 1);
    clear_rows();
    log_op("Start compare: %d seconds", total);

    while (sec < total && WaitForSingleObject(g_stop_event, 0) == WAIT_TIMEOUT) {
        double a_ms = -1.0, b_ms = -1.0;
        int ha = ping_once(g_cfg_a.target, g_cfg_a.port, 1000, &a_ms);
        int hb = ping_once(g_cfg_b.target, g_cfg_b.port, 1000, &b_ms);

        _snprintf(timebuf, sizeof(timebuf), "%02d:%02d", sec / 60, sec % 60);

        if (ha && hb) {
            char note[128];
            _snprintf(note, sizeof(note), "A %.2f ms | B %.2f ms", a_ms, b_ms);
            add_row(timebuf, a_ms, 1, b_ms, 1, note);
            log_dbg("[%s] A=%.2fms B=%.2fms", timebuf, a_ms, b_ms);
        } else if (ha) {
            add_row(timebuf, a_ms, 1, 0.0, 0, "B timeout");
            log_dbg("[%s] A=%.2fms B timeout", timebuf, a_ms);
        } else if (hb) {
            add_row(timebuf, 0.0, 0, b_ms, 1, "A timeout");
            log_dbg("[%s] A timeout B=%.2fms", timebuf, b_ms);
        } else {
            add_row(timebuf, 0.0, 0, 0.0, 0, "A/B timeout");
            log_dbg("[%s] A/B timeout", timebuf);
        }

        sec++;
        ui_status_update();
        for (int i = 0; i < 10; i++) {
            if (WaitForSingleObject(g_stop_event, 100) == WAIT_OBJECT_0) break;
        }
    }

    log_op("Compare done");
    InterlockedExchange(&g_running, 0);
    PostMessageW(g_hWnd, WM_APP_DONE, 0, 0);
    return 0;
}

static void refresh_logs(void) {
    wchar_t wbuf[32768];

    EnterCriticalSection(&g_log_cs);
    utf8_to_wide(g_log_op, wbuf, 32768);
    SetWindowTextW(g_hLogOp, wbuf);
    utf8_to_wide(g_log_dbg, wbuf, 32768);
    SetWindowTextW(g_hLogDbg, wbuf);
    LeaveCriticalSection(&g_log_cs);
}

static void refresh_list(void) {
    if (!g_hList) return;

    ListView_DeleteAllItems(g_hList);

    EnterCriticalSection(&g_data_cs);
    for (int i = 0; i < g_row_count; i++) {
        wchar_t wtime[32], wa[64], wb[64], wnote[128];
        LVITEMW it;
        ZeroMemory(&it, sizeof(it));

        utf8_to_wide(g_rows[i].time, wtime, 32);

        if (g_rows[i].has_a) swprintf(wa, 64, L"%.2f", g_rows[i].a_ms);
        else lstrcpyW(wa, L"-");

        if (g_rows[i].has_b) swprintf(wb, 64, L"%.2f", g_rows[i].b_ms);
        else lstrcpyW(wb, L"-");

        utf8_to_wide(g_rows[i].note, wnote, 128);

        it.mask = LVIF_TEXT;
        it.iItem = i;
        it.iSubItem = 0;
        it.pszText = wtime;
        ListView_InsertItem(g_hList, &it);

        ListView_SetItemText(g_hList, i, 1, wa);
        ListView_SetItemText(g_hList, i, 2, wb);
        ListView_SetItemText(g_hList, i, 3, wnote);
    }
    LeaveCriticalSection(&g_data_cs);
}

static void start_ping(void) {
    if (InterlockedCompareExchange(&g_running, 1, 0) != 0) return;

    get_edit_text(g_hEditAHost, g_cfg_a.target, sizeof(g_cfg_a.target));
    get_edit_text(g_hEditAPort, g_cfg_a.port, sizeof(g_cfg_a.port));
    get_edit_text(g_hEditBHost, g_cfg_b.target, sizeof(g_cfg_b.target));
    get_edit_text(g_hEditBPort, g_cfg_b.port, sizeof(g_cfg_b.port));

    char dur[32] = {0};
    get_edit_text(g_hEditDur, dur, sizeof(dur));
    g_duration_sec = atoi(dur);
    if (g_duration_sec <= 0) g_duration_sec = RUN_SECONDS;

    if (g_cfg_a.target[0] == 0) strcpy(g_cfg_a.target, "1.1.1.1");
    if (g_cfg_b.target[0] == 0) strcpy(g_cfg_b.target, "8.8.8.8");
    if (g_cfg_a.port[0] == 0) strcpy(g_cfg_a.port, "80");
    if (g_cfg_b.port[0] == 0) strcpy(g_cfg_b.port, "53");

    ResetEvent(g_stop_event);
    log_op("Start requested");
    ui_status_update();
    g_worker = CreateThread(NULL, 0, worker_thread, NULL, 0, NULL);
}

static void stop_ping(void) {
    if (InterlockedCompareExchange(&g_running, 0, 1) == 0) return;
    SetEvent(g_stop_event);
    log_op("Stop requested");
    ui_status_update();
}

static void tray_add(HWND hwnd) {
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = ID_TRAY_ICON;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    lstrcpyW(g_nid.szTip, L"TCPing GUI");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

static void tray_remove(void) {
    if (g_nid.cbSize) Shell_NotifyIconW(NIM_DELETE, &g_nid);
}

static void tray_menu(HWND hwnd) {
    POINT pt;
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, ID_MENU_SHOW, L"Show Window");
    AppendMenuW(menu, MF_STRING, ID_MENU_START, L"Start");
    AppendMenuW(menu, MF_STRING, ID_MENU_STOP, L"Stop");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, ID_MENU_EXIT, L"Exit");
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
    DestroyMenu(menu);
}

static void build_ui(HWND hwnd) {
    HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    int y = 10;

    CreateWindowW(L"STATIC", L"Target A", WS_CHILD | WS_VISIBLE, 10, y, 60, 20, hwnd, NULL, g_hInst, NULL);
    g_hEditAHost = CreateWindowW(L"EDIT", L"1.1.1.1", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                                 75, y - 2, 150, 24, hwnd, (HMENU)ID_EDIT_A_HOST, g_hInst, NULL);
    CreateWindowW(L"STATIC", L"Port A", WS_CHILD | WS_VISIBLE, 235, y, 45, 20, hwnd, NULL, g_hInst, NULL);
    g_hEditAPort = CreateWindowW(L"EDIT", L"80", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                                 285, y - 2, 60, 24, hwnd, (HMENU)ID_EDIT_A_PORT, g_hInst, NULL);
    CreateWindowW(L"STATIC", L"Target B", WS_CHILD | WS_VISIBLE, 360, y, 60, 20, hwnd, NULL, g_hInst, NULL);
    g_hEditBHost = CreateWindowW(L"EDIT", L"8.8.8.8", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                                 425, y - 2, 150, 24, hwnd, (HMENU)ID_EDIT_B_HOST, g_hInst, NULL);
    CreateWindowW(L"STATIC", L"Port B", WS_CHILD | WS_VISIBLE, 585, y, 45, 20, hwnd, NULL, g_hInst, NULL);
    g_hEditBPort = CreateWindowW(L"EDIT", L"53", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                                 635, y - 2, 60, 24, hwnd, (HMENU)ID_EDIT_B_PORT, g_hInst, NULL);
    CreateWindowW(L"STATIC", L"Duration", WS_CHILD | WS_VISIBLE, 710, y, 55, 20, hwnd, NULL, g_hInst, NULL);
    g_hEditDur = CreateWindowW(L"EDIT", L"50", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                               770, y - 2, 60, 24, hwnd, (HMENU)ID_EDIT_DUR, g_hInst, NULL);

    y += 34;
    CreateWindowW(L"BUTTON", L"Start", WS_CHILD | WS_VISIBLE, 10, y, 100, 28, hwnd, (HMENU)ID_BTN_START, g_hInst, NULL);
    CreateWindowW(L"BUTTON", L"Stop", WS_CHILD | WS_VISIBLE, 120, y, 100, 28, hwnd, (HMENU)ID_BTN_STOP, g_hInst, NULL);

    y += 40;
    CreateWindowW(L"STATIC", L"Operational Log", WS_CHILD | WS_VISIBLE, 10, y, 120, 20, hwnd, NULL, g_hInst, NULL);
    CreateWindowW(L"STATIC", L"Debug Log", WS_CHILD | WS_VISIBLE, 500, y, 100, 20, hwnd, NULL, g_hInst, NULL);

    y += 20;
    g_hLogOp = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
        10, y, 460, 170, hwnd, (HMENU)ID_LOG_OP, g_hInst, NULL);
    g_hLogDbg = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
        500, y, 470, 170, hwnd, (HMENU)ID_LOG_DBG, g_hInst, NULL);

    y += 190;
    CreateWindowW(L"STATIC", L"Results", WS_CHILD | WS_VISIBLE, 10, y, 80, 20, hwnd, NULL, g_hInst, NULL);
    y += 20;

    g_hList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL,
        10, y, 960, 300, hwnd, (HMENU)ID_LIST, g_hInst, NULL);

    ListView_SetExtendedListViewStyle(g_hList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

    LVCOLUMNW col;
    ZeroMemory(&col, sizeof(col));
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;

    col.pszText = L"Time"; col.cx = 100; ListView_InsertColumn(g_hList, 0, &col);
    col.pszText = L"A ms"; col.cx = 100; col.iSubItem = 1; ListView_InsertColumn(g_hList, 1, &col);
    col.pszText = L"B ms"; col.cx = 100; col.iSubItem = 2; ListView_InsertColumn(g_hList, 2, &col);
    col.pszText = L"Note"; col.cx = 620; col.iSubItem = 3; ListView_InsertColumn(g_hList, 3, &col);

    g_hStatus = CreateWindowExW(0, STATUSCLASSNAMEW, NULL,
        WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, (HMENU)ID_STATUS, g_hInst, NULL);

    SendMessageW(g_hEditAHost, WM_SETFONT, (WPARAM)hFont, TRUE);
    SendMessageW(g_hEditAPort, WM_SETFONT, (WPARAM)hFont, TRUE);
    SendMessageW(g_hEditBHost, WM_SETFONT, (WPARAM)hFont, TRUE);
    SendMessageW(g_hEditBPort, WM_SETFONT, (WPARAM)hFont, TRUE);
    SendMessageW(g_hEditDur, WM_SETFONT, (WPARAM)hFont, TRUE);
    SendMessageW(g_hLogOp, WM_SETFONT, (WPARAM)hFont, TRUE);
    SendMessageW(g_hLogDbg, WM_SETFONT, (WPARAM)hFont, TRUE);
    SendMessageW(g_hList, WM_SETFONT, (WPARAM)hFont, TRUE);
    ui_status_update();
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        build_ui(hwnd);
        tray_add(hwnd);
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case ID_BTN_START:
        case ID_MENU_START:
            start_ping();
            return 0;
        case ID_BTN_STOP:
        case ID_MENU_STOP:
            stop_ping();
            return 0;
        case ID_MENU_SHOW:
            ShowWindow(hwnd, SW_SHOW);
            SetForegroundWindow(hwnd);
            return 0;
        case ID_MENU_EXIT:
            DestroyWindow(hwnd);
            return 0;
        }
        break;

    case WM_APP_LOG:
        refresh_logs();
        ui_status_update();
        return 0;

    case WM_APP_ROW:
        refresh_list();
        ui_status_update();
        return 0;

    case WM_APP_DONE:
        refresh_list();
        refresh_logs();
        ui_status_update();
        return 0;

    case WM_TRAYICON:
        if (lParam == WM_RBUTTONUP || lParam == WM_LBUTTONDBLCLK) {
            tray_menu(hwnd);
        }
        return 0;

    case WM_SIZE:
        if (g_hStatus) SendMessageW(g_hStatus, WM_SIZE, 0, 0);
        return 0;

    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;

    case WM_DESTROY:
        tray_remove();
        SetEvent(g_stop_event);
        InterlockedExchange(&g_exit_app, 1);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static HWND create_main_window(HINSTANCE hInst) {
    WNDCLASSW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = APP_NAME;
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&wc);

    return CreateWindowExW(0, APP_NAME, L"TCPing GUI",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1000, 700,
        NULL, NULL, hInst, NULL);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR lpCmd, int nShow) {
    MSG msg;
    INITCOMMONCONTROLSEX icc;
    WSADATA wsa;

    (void)hPrev;
    (void)lpCmd;

    g_hInst = hInst;
    InitializeCriticalSection(&g_log_cs);
    InitializeCriticalSection(&g_data_cs);

    g_stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);

    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES | ICC_BAR_CLASSES | ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icc);

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 0;

    g_hWnd = create_main_window(hInst);
    if (!g_hWnd) return 0;

    ShowWindow(g_hWnd, nShow);
    UpdateWindow(g_hWnd);

    log_op("Ready");
    refresh_logs();
    ui_status_update();

    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_worker) {
        WaitForSingleObject(g_worker, 1000);
        CloseHandle(g_worker);
        g_worker = NULL;
    }

    if (g_stop_event) CloseHandle(g_stop_event);

    WSACleanup();
    DeleteCriticalSection(&g_log_cs);
    DeleteCriticalSection(&g_data_cs);
    return 0;
}
