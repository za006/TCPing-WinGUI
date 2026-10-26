#define UNICODE
#define _UNICODE
#include <windows.h>
#include <commctrl.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <stdarg.h>
#include <process.h>
#include <stdint.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")

#define ID_EDIT_A      101
#define ID_EDIT_B      102
#define ID_EDIT_PA     103
#define ID_EDIT_PB     104
#define ID_EDIT_DUR    105
#define ID_BTN_START   106
#define ID_BTN_STOP    107
#define ID_CMB_MODE    108
#define ID_LISTVIEW    109
#define ID_LOG         110
#define ID_STATUS      111

#define WM_APP_APPEND_LOG   (WM_APP + 1)
#define WM_APP_ADD_RESULT   (WM_APP + 2)
#define WM_APP_DONE         (WM_APP + 3)

typedef struct {
    HWND hWnd;
    HWND hEditA, hEditB, hEditPA, hEditPB, hEditDur;
    HWND hBtnStart, hBtnStop, hCmbMode;
    HWND hList, hLog, hStatus;
    HANDLE hThread;
    volatile LONG stopFlag;
    volatile LONG running;
    HFONT hFont;
    HICON hIconLarge, hIconSmall;
} APPSTATE;

typedef struct {
    int seq;
    int side; // 0=A 1=B
    double ms;
    int ok;
    WCHAR target[256];
    WCHAR port[32];
} RESULTITEM;

static APPSTATE g;

static void append_log(const wchar_t* fmt, ...) {
    wchar_t buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vswprintf(buf, 1024, fmt, ap);
    va_end(ap);

    size_t len = wcslen(buf);
    wchar_t* heap = (wchar_t*)HeapAlloc(GetProcessHeap(), 0, (len + 3) * sizeof(wchar_t));
    if (!heap) return;
    wcscpy(heap, buf);
    wcscat(heap, L"\r\n");
    PostMessageW(g.hWnd, WM_APP_APPEND_LOG, 0, (LPARAM)heap);
}

static void list_add_result(const RESULTITEM* r) {
    wchar_t seq[32], side[8], ms[64], ok[16];
    swprintf(seq, 32, L"%d", r->seq);
    wcscpy(side, r->side == 0 ? L"A" : L"B");
    if (r->ok) swprintf(ms, 64, L"%.2f", r->ms);
    else wcscpy(ms, L"Timeout");
    wcscpy(ok, r->ok ? L"OK" : L"Fail");

    int idx = ListView_GetItemCount(g.hList);
    LVITEMW it = {0};
    it.mask = LVIF_TEXT;
    it.iItem = idx;
    it.pszText = seq;
    ListView_InsertItemW(g.hList, &it);
    ListView_SetItemText(g.hList, idx, 1, side);
    ListView_SetItemText(g.hList, idx, 2, (WCHAR*)r->target);
    ListView_SetItemText(g.hList, idx, 3, (WCHAR*)r->port);
    ListView_SetItemText(g.hList, idx, 4, ms);
    ListView_SetItemText(g.hList, idx, 5, ok);
    ListView_EnsureVisible(g.hList, idx, FALSE);
}

static int get_text(HWND h, wchar_t* buf, int cch) {
    return GetWindowTextW(h, buf, cch);
}

static int get_int(HWND h, int defv) {
    wchar_t buf[64];
    get_text(h, buf, 64);
    if (buf[0] == 0) return defv;
    return _wtoi(buf);
}

static int tcp_connect_ms(const wchar_t* host, const wchar_t* port, double* out_ms) {
    struct addrinfoW hints, *res = NULL, *p = NULL;
    ZeroMemory(&hints, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    hints.ai_protocol = IPPROTO_TCP;

    DWORD t0 = GetTickCount();
    int ret = GetAddrInfoW(host, port, &hints, &res);
    if (ret != 0 || !res) return 0;

    int ok = 0;
    for (p = res; p; p = p->ai_next) {
        SOCKET s = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (s == INVALID_SOCKET) continue;

        u_long nb = 1;
        ioctlsocket(s, FIONBIO, &nb);

        int cr = connect(s, p->ai_addr, (int)p->ai_addrlen);
        if (cr == SOCKET_ERROR) {
            int e = WSAGetLastError();
            if (e != WSAEWOULDBLOCK && e != WSAEINPROGRESS) {
                closesocket(s);
                continue;
            }
        }

        fd_set wfds, efds;
        FD_ZERO(&wfds); FD_ZERO(&efds);
        FD_SET(s, &wfds); FD_SET(s, &efds);

        TIMEVAL tv;
        tv.tv_sec = 2;
        tv.tv_usec = 0;

        ret = select(0, NULL, &wfds, &efds, &tv);
        if (ret > 0 && FD_ISSET(s, &wfds)) {
            int soerr = 0;
            int len = sizeof(soerr);
            getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&soerr, &len);
            if (soerr == 0) {
                DWORD t1 = GetTickCount();
                *out_ms = (double)(t1 - t0);
                ok = 1;
                closesocket(s);
                break;
            }
        }
        closesocket(s);
    }

    FreeAddrInfoW(res);
    return ok;
}

static DWORD WINAPI worker_thread(LPVOID param) {
    (void)param;

    wchar_t a[256], b[256], pa[32], pb[32];
    get_text(g.hEditA, a, 256);
    get_text(g.hEditB, b, 256);
    get_text(g.hEditPA, pa, 32);
    get_text(g.hEditPB, pb, 32);

    int dur = get_int(g.hEditDur, 50);
    if (dur <= 0) dur = 50;

    append_log(L"开始测试：%d 秒", dur);
    SetWindowTextW(g.hStatus, L"Running...");

    DWORD start = GetTickCount();
    int seq = 1;

    while (!g.stopFlag) {
        DWORD now = GetTickCount();
        if ((now - start) >= (DWORD)(dur * 1000)) break;

        RESULTITEM r;
        ZeroMemory(&r, sizeof(r));

        r.seq = seq++;
        r.side = 0;
        wcscpy(r.target, a);
        wcscpy(r.port, pa);

        double ms = 0.0;
        int ok = tcp_connect_ms(a, pa, &ms);
        r.ok = ok;
        r.ms = ms;
        PostMessageW(g.hWnd, WM_APP_ADD_RESULT, 0, (LPARAM)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(RESULTITEM)));
        RESULTITEM* copy = (RESULTITEM*)GetWindowLongPtrW(g.hWnd, GWLP_USERDATA);
        (void)copy;
        PostMessageW(g.hWnd, WM_APP_ADD_RESULT, 0, (LPARAM)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(RESULTITEM)));

        RESULTITEM* p1 = (RESULTITEM*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(RESULTITEM));
        if (p1) {
            *p1 = r;
            PostMessageW(g.hWnd, WM_APP_ADD_RESULT, 0, (LPARAM)p1);
        }

        r.seq = seq++;
        r.side = 1;
        wcscpy(r.target, b);
        wcscpy(r.port, pb);
        ms = 0.0;
        ok = tcp_connect_ms(b, pb, &ms);
        r.ok = ok;
        r.ms = ms;

        RESULTITEM* p2 = (RESULTITEM*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(RESULTITEM));
        if (p2) {
            *p2 = r;
            PostMessageW(g.hWnd, WM_APP_ADD_RESULT, 0, (LPARAM)p2);
        }

        Sleep(50);
    }

    PostMessageW(g.hWnd, WM_APP_DONE, 0, 0);
    return 0;
}

static void set_font(HWND h) {
    SendMessageW(h, WM_SETFONT, (WPARAM)g.hFont, TRUE);
}

static void layout(HWND hWnd) {
    RECT rc;
    GetClientRect(hWnd, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;

    int m = 10;
    int top = 10;
    int rowh = 26;
    int labelw = 55;
    int editw = 130;

    MoveWindow(GetDlgItem(hWnd, ID_EDIT_A), m + labelw, top, editw, rowh, TRUE);
    MoveWindow(GetDlgItem(hWnd, ID_EDIT_PA), m + labelw + editw + 8 + 40, top, 70, rowh, TRUE);
    MoveWindow(GetDlgItem(hWnd, ID_EDIT_B), m + labelw + editw + 8 + 40 + 70 + 80, top, editw, rowh, TRUE);
    MoveWindow(GetDlgItem(hWnd, ID_EDIT_PB), m + labelw + editw + 8 + 40 + 70 + 80 + editw + 8, top, 70, rowh, TRUE);

    top += rowh + 8;
    MoveWindow(GetDlgItem(hWnd, ID_EDIT_DUR), m + labelw, top, 70, rowh, TRUE);
    MoveWindow(GetDlgItem(hWnd, ID_CMB_MODE), m + labelw + 110, top, 130, rowh, TRUE);
    MoveWindow(GetDlgItem(hWnd, ID_BTN_START), m + labelw + 260, top, 90, rowh, TRUE);
    MoveWindow(GetDlgItem(hWnd, ID_BTN_STOP), m + labelw + 360, top, 90, rowh, TRUE);

    top += rowh + 10;
    int logh = 110;
    int listh = h - top - logh - 25;
    if (listh < 120) listh = 120;

    MoveWindow(g.hList, m, top, w - 2*m, listh, TRUE);
    top += listh + 8;
    MoveWindow(g.hLog, m, top, w - 2*m, logh, TRUE);
    MoveWindow(g.hStatus, m, h - 20, w - 2*m, 18, TRUE);
}

static HICON load_icon_from_file(void) {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    wchar_t* p = wcsrchr(path, L'\\');
    if (p) *(p + 1) = 0;
    wcscat(path, L"TCPing_GUI.ico");

    HICON ic = (HICON)LoadImageW(NULL, path, IMAGE_ICON, 32, 32, LR_LOADFROMFILE | LR_DEFAULTSIZE);
    if (!ic) ic = (HICON)LoadImageW(NULL, path, IMAGE_ICON, 16, 16, LR_LOADFROMFILE | LR_DEFAULTSIZE);
    return ic;
}

static void init_controls(HWND hWnd) {
    g.hFont = CreateFontW(-16, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                          OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                          DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    CreateWindowW(L"STATIC", L"Target A", WS_CHILD | WS_VISIBLE, 10, 12, 50, 24, hWnd, 0, 0, 0);
    g.hEditA = CreateWindowW(L"EDIT", L"127.0.0.1", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                             65, 10, 130, 24, hWnd, (HMENU)ID_EDIT_A, 0, 0);
    CreateWindowW(L"STATIC", L"Port", WS_CHILD | WS_VISIBLE, 205, 12, 35, 24, hWnd, 0, 0, 0);
    g.hEditPA = CreateWindowW(L"EDIT", L"80", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                              245, 10, 70, 24, hWnd, (HMENU)ID_EDIT_PA, 0, 0);

    CreateWindowW(L"STATIC", L"Target B", WS_CHILD | WS_VISIBLE, 325, 12, 55, 24, hWnd, 0, 0, 0);
    g.hEditB = CreateWindowW(L"EDIT", L"1.1.1.1", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                             385, 10, 130, 24, hWnd, (HMENU)ID_EDIT_B, 0, 0);
    CreateWindowW(L"STATIC", L"Port", WS_CHILD | WS_VISIBLE, 525, 12, 35, 24, hWnd, 0, 0, 0);
    g.hEditPB = CreateWindowW(L"EDIT", L"80", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                              565, 10, 70, 24, hWnd, (HMENU)ID_EDIT_PB, 0, 0);

    CreateWindowW(L"STATIC", L"Duration", WS_CHILD | WS_VISIBLE, 10, 46, 55, 24, hWnd, 0, 0, 0);
    g.hEditDur = CreateWindowW(L"EDIT", L"50", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                               65, 44, 70, 24, hWnd, (HMENU)ID_EDIT_DUR, 0, 0);
    g.hCmbMode = CreateWindowW(WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
                               175, 44, 120, 120, hWnd, (HMENU)ID_CMB_MODE, 0, 0);
    SendMessageW(g.hCmbMode, CB_ADDSTRING, 0, (LPARAM)L"Ordinary");
    SendMessageW(g.hCmbMode, CB_SETCURSEL, 0, 0);

    g.hBtnStart = CreateWindowW(L"BUTTON", L"Start", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                305, 44, 90, 24, hWnd, (HMENU)ID_BTN_START, 0, 0);
    g.hBtnStop = CreateWindowW(L"BUTTON", L"Stop", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                               405, 44, 90, 24, hWnd, (HMENU)ID_BTN_STOP, 0, 0);

    g.hList = CreateWindowW(WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | WS_BORDER | LVS_REPORT | LVS_SINGLESEL,
                            10, 80, 760, 220, hWnd, (HMENU)ID_LISTVIEW, 0, 0);
    g.hLog = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
                           10, 310, 760, 110, hWnd, (HMENU)ID_LOG, 0, 0);
    g.hStatus = CreateWindowW(L"STATIC", L"Ready", WS_CHILD | WS_VISIBLE,
                              10, 430, 760, 18, hWnd, (HMENU)ID_STATUS, 0, 0);

    set_font(g.hEditA); set_font(g.hEditB); set_font(g.hEditPA); set_font(g.hEditPB);
    set_font(g.hEditDur); set_font(g.hBtnStart); set_font(g.hBtnStop); set_font(g.hCmbMode);
    set_font(g.hList); set_font(g.hLog); set_font(g.hStatus);

    ListView_SetExtendedListViewStyle(g.hList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    LVCOLUMNW col = {0};
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;

    col.cx = 60;  col.pszText = L"Seq";    ListView_InsertColumnW(g.hList, 0, &col);
    col.cx = 50;  col.pszText = L"Side";   ListView_InsertColumnW(g.hList, 1, &col);
    col.cx = 180; col.pszText = L"Target"; ListView_InsertColumnW(g.hList, 2, &col);
    col.cx = 80;  col.pszText = L"Port";    ListView_InsertColumnW(g.hList, 3, &col);
    col.cx = 90;  col.pszText = L"RTT(ms)"; ListView_InsertColumnW(g.hList, 4, &col);
    col.cx = 70;  col.pszText = L"Result";  ListView_InsertColumnW(g.hList, 5, &col);

    if (g.hIconLarge) {
        SendMessageW(hWnd, WM_SETICON, ICON_BIG, (LPARAM)g.hIconLarge);
        SendMessageW(hWnd, WM_SETICON, ICON_SMALL, (LPARAM)g.hIconSmall);
    }
}

static void start_test(HWND hWnd) {
    if (InterlockedCompareExchange(&g.running, 1, 0) != 0) return;
    g.stopFlag = 0;
    ListView_DeleteAllItems(g.hList);
    SetWindowTextW(g.hLog, L"");
    EnableWindow(g.hBtnStart, FALSE);
    EnableWindow(g.hBtnStop, TRUE);
    g.hThread = CreateThread(NULL, 0, worker_thread, NULL, 0, NULL);
    if (!g.hThread) {
        g.running = 0;
        EnableWindow(g.hBtnStart, TRUE);
        EnableWindow(g.hBtnStop, FALSE);
        append_log(L"线程创建失败");
    }
}

static void stop_test(void) {
    g.stopFlag = 1;
    append_log(L"正在停止...");
}

static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        g.hWnd = hWnd;
        init_controls(hWnd);
        layout(hWnd);
        append_log(L"就绪。默认模式：Ordinary，默认时长：50 秒");
        return 0;

    case WM_SIZE:
        layout(hWnd);
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case ID_BTN_START:
            start_test(hWnd);
            return 0;
        case ID_BTN_STOP:
            stop_test();
            return 0;
        }
        return 0;

    case WM_APP_APPEND_LOG: {
        wchar_t* text = (wchar_t*)lParam;
        int len = GetWindowTextLengthW(g.hLog);
        SendMessageW(g.hLog, EM_SETSEL, len, len);
        SendMessageW(g.hLog, EM_REPLACESEL, FALSE, (LPARAM)text);
        HeapFree(GetProcessHeap(), 0, text);
        return 0;
    }

    case WM_APP_ADD_RESULT: {
        RESULTITEM* r = (RESULTITEM*)lParam;
        if (r) {
            list_add_result(r);
            HeapFree(GetProcessHeap(), 0, r);
        }
        return 0;
    }

    case WM_APP_DONE:
        g.running = 0;
        EnableWindow(g.hBtnStart, TRUE);
        EnableWindow(g.hBtnStop, FALSE);
        SetWindowTextW(g.hStatus, L"Done");
        append_log(L"测试完成");
        if (g.hThread) {
            CloseHandle(g.hThread);
            g.hThread = NULL;
        }
        return 0;

    case WM_DESTROY:
        g.stopFlag = 1;
        if (g.hThread) {
            WaitForSingleObject(g.hThread, 1000);
            CloseHandle(g.hThread);
        }
        if (g.hFont) DeleteObject(g.hFont);
        if (g.hIconLarge) DestroyIcon(g.hIconLarge);
        if (g.hIconSmall) DestroyIcon(g.hIconSmall);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

int APIENTRY wWinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR lpCmdLine, int nShowCmd) {
    (void)hPrev; (void)lpCmdLine; (void)nShowCmd;

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 1;

    INITCOMMONCONTROLSEX ic = { sizeof(ic), ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES };
    InitCommonControlsEx(&ic);

    g.hIconLarge = load_icon_from_file();
    g.hIconSmall = g.hIconLarge ? (HICON)CopyIcon(g.hIconLarge) : NULL;

    WNDCLASSW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"TCPingGUIClass";
    wc.hIcon = g.hIconLarge;
    wc.hIconSm = g.hIconSmall;

    if (!RegisterClassW(&wc)) return 1;

    HWND hWnd = CreateWindowExW(
        0, wc.lpszClassName, L"TCPing GUI",
        WS_OVERLAPPEDWINDOW ^ WS_THICKFRAME ^ WS_MAXIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 860, 520,
        NULL, NULL, hInst, NULL);

    if (!hWnd) return 1;

    ShowWindow(hWnd, SW_SHOW);
    UpdateWindow(hWnd);

    EnableWindow(g.hBtnStop, FALSE);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    WSACleanup();
    return 0;
}
