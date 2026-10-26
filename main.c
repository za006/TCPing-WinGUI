#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <stdio.h>

#define IDC_HOSTA   101
#define IDC_PORTA   102
#define IDC_HOSTB   103
#define IDC_PORTB   104
#define IDC_DUR     105
#define IDC_START   106
#define IDC_STOP    107
#define IDC_LOG     108
#define IDC_LIST    109

#define WM_APP_ROW  (WM_APP + 1)
#define WM_APP_LOG  (WM_APP + 2)
#define WM_APP_DONE (WM_APP + 3)

typedef struct {
    int id;
    wchar_t name[128];
    int sent, ok, lost;
    int last, min, avg, max;
    int pct;
    wchar_t state[32];
} ROW;

static HINSTANCE g_hInst;
static HWND g_hwnd;
static HWND g_hHostA, g_hPortA, g_hHostB, g_hPortB, g_hDur;
static HWND g_hStart, g_hStop, g_hLog, g_hList;
static HANDLE g_hThread;
static volatile LONG g_running, g_stop;
static HICON g_hIcon;

static void post_log(const wchar_t *s) { if (g_hwnd) PostMessageW(g_hwnd, WM_APP_LOG, 0, (LPARAM)s); }
static void post_done(void) { if (g_hwnd) PostMessageW(g_hwnd, WM_APP_DONE, 0, 0); }

static void append_log(const wchar_t *s) {
    int len = GetWindowTextLengthW(g_hLog);
    SendMessageW(g_hLog, EM_SETSEL, len, len);
    SendMessageW(g_hLog, EM_REPLACESEL, 0, (LPARAM)s);
    SendMessageW(g_hLog, EM_REPLACESEL, 0, (LPARAM)L"\r\n");
    SendMessageW(g_hLog, EM_SCROLLCARET, 0, 0);
}

static void list_init(void) {
    LVCOLUMNW c = {0};
    c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    c.cx = 120; c.pszText = L"目标"; ListView_InsertColumn(g_hList, 0, &c);
    c.cx = 55;  c.pszText = L"发送"; ListView_InsertColumn(g_hList, 1, &c);
    c.cx = 55;  c.pszText = L"成功"; ListView_InsertColumn(g_hList, 2, &c);
    c.cx = 55;  c.pszText = L"丢失"; ListView_InsertColumn(g_hList, 3, &c);
    c.cx = 65;  c.pszText = L"最后"; ListView_InsertColumn(g_hList, 4, &c);
    c.cx = 65;  c.pszText = L"最小"; ListView_InsertColumn(g_hList, 5, &c);
    c.cx = 65;  c.pszText = L"平均"; ListView_InsertColumn(g_hList, 6, &c);
    c.cx = 65;  c.pszText = L"最大"; ListView_InsertColumn(g_hList, 7, &c);
    c.cx = 95;  c.pszText = L"状态"; ListView_InsertColumn(g_hList, 8, &c);
    ListView_SetExtendedListViewStyle(g_hList,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
}

static void set_row(const ROW *r) {
    wchar_t b[64];
    LVITEMW it = {0};
    it.mask = LVIF_TEXT;
    it.iItem = r->id;
    it.iSubItem = 0;
    it.pszText = (LPWSTR)r->name;

    if (ListView_GetItemCount(g_hList) <= r->id) ListView_InsertItem(g_hList, &it);
    else ListView_SetItemText(g_hList, r->id, 0, (LPWSTR)r->name);

    swprintf(b, 64, L"%d", r->sent); ListView_SetItemText(g_hList, r->id, 1, b);
    swprintf(b, 64, L"%d", r->ok);   ListView_SetItemText(g_hList, r->id, 2, b);
    swprintf(b, 64, L"%d%%", r->lost); ListView_SetItemText(g_hList, r->id, 3, b);
    swprintf(b, 64, L"%d", r->last); ListView_SetItemText(g_hList, r->id, 4, b);
    swprintf(b, 64, L"%d", r->min == 0x7fffffff ? 0 : r->min); ListView_SetItemText(g_hList, r->id, 5, b);
    swprintf(b, 64, L"%d", r->avg);  ListView_SetItemText(g_hList, r->id, 6, b);
    swprintf(b, 64, L"%d", r->max);  ListView_SetItemText(g_hList, r->id, 7, b);
    ListView_SetItemText(g_hList, r->id, 8, (LPWSTR)r->state);
}

static DWORD ping_once(const wchar_t *host, unsigned short port) {
    ADDRINFOW hints, *res = NULL, *p = NULL;
    wchar_t ps[16];
    LARGE_INTEGER f, t1, t2;
    DWORD best = (DWORD)-1;

    ZeroMemory(&hints, sizeof(hints));
    swprintf(ps, 16, L"%u", port);

    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_family = AF_UNSPEC;

    if (GetAddrInfoW(host, ps, &hints, &res) != 0) return (DWORD)-1;
    if (!QueryPerformanceFrequency(&f)) {
        FreeAddrInfoW(res);
        return (DWORD)-1;
    }

    for (p = res; p; p = p->ai_next) {
        SOCKET s = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (s == INVALID_SOCKET) continue;

        u_long nb = 1;
        ioctlsocket(s, FIONBIO, &nb);

        QueryPerformanceCounter(&t1);
        int r = connect(s, p->ai_addr, (int)p->ai_addrlen);
        if (r == 0) {
            QueryPerformanceCounter(&t2);
            closesocket(s);
            best = (DWORD)((t2.QuadPart - t1.QuadPart) * 1000.0 / f.QuadPart + 0.5);
            break;
        }

        int e = WSAGetLastError();
        if (e == WSAEWOULDBLOCK || e == WSAEINPROGRESS || e == WSAEINVAL) {
            fd_set wf;
            FD_ZERO(&wf);
            FD_SET(s, &wf);
            struct timeval tv;
            tv.tv_sec = 1;
            tv.tv_usec = 0;

            r = select(0, NULL, &wf, NULL, &tv);
            if (r > 0) {
                int so = 0, sl = sizeof(so);
                getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&so, &sl);
                if (so == 0) {
                    QueryPerformanceCounter(&t2);
                    best = (DWORD)((t2.QuadPart - t1.QuadPart) * 1000.0 / f.QuadPart + 0.5);
                    closesocket(s);
                    break;
                }
            }
        }

        closesocket(s);
    }

    FreeAddrInfoW(res);
    return best;
}

static DWORD WINAPI worker(LPVOID lp) {
    HWND hwnd = (HWND)lp;
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) {
        post_log(L"WSAStartup failed");
        post_done();
        InterlockedExchange(&g_running, 0);
        return 0;
    }

    wchar_t hostA[128], hostB[128], logbuf[256];
    int portA = GetDlgItemInt(hwnd, IDC_PORTA, NULL, FALSE);
    int portB = GetDlgItemInt(hwnd, IDC_PORTB, NULL, FALSE);
    int duration = GetDlgItemInt(hwnd, IDC_DUR, NULL, FALSE);
    if (duration <= 0) duration = 50;

    GetWindowTextW(g_hHostA, hostA, 128);
    GetWindowTextW(g_hHostB, hostB, 128);

    ROW rows[2];
    ZeroMemory(rows, sizeof(rows));
    rows[0].id = 0; lstrcpynW(rows[0].name, hostA, 127); rows[0].min = 0x7fffffff;
    rows[1].id = 1; lstrcpynW(rows[1].name, hostB, 127); rows[1].min = 0x7fffffff;

    swprintf(logbuf, 256, L"开始: %s:%d vs %s:%d, duration=%ds", hostA, portA, hostB, portB, duration);
    post_log(logbuf);

    for (int sec = 0; sec < duration && !g_stop; sec++) {
        const wchar_t *hosts[2] = { hostA, hostB };
        int ports[2] = { portA, portB };

        for (int i = 0; i < 2 && !g_stop; i++) {
            ROW *r = &rows[i];
            r->sent++;

            DWORD ms = ping_once(hosts[i], (unsigned short)ports[i]);
            if (ms != (DWORD)-1) {
                r->ok++;
                r->last = (int)ms;
                if (r->last < r->min) r->min = r->last;
                if (r->last > r->max) r->max = r->last;
                r->avg = (r->avg * (r->ok - 1) + r->last) / r->ok;
                wcscpy(r->state, L"OK");
            } else {
                wcscpy(r->state, L"TIMEOUT");
            }

            r->lost = r->sent ? (r->sent - r->ok) * 100 / r->sent : 0;
            r->pct = r->sent ? r->ok * 100 / r->sent : 0;

            PostMessageW(hwnd, WM_APP_ROW, 0, (LPARAM)r);
        }

        Sleep(1000);
    }

    post_log(L"完成");
    WSACleanup();
    InterlockedExchange(&g_running, 0);
    post_done();
    return 0;
}

static void start_test(void) {
    if (InterlockedCompareExchange(&g_running, 1, 0) != 0) return;
    InterlockedExchange(&g_stop, 0);
    SendMessageW(g_hList, LVM_DELETEALLITEMS, 0, 0);
    SetWindowTextW(g_hLog, L"");

    g_hThread = CreateThread(NULL, 0, worker, g_hwnd, 0, NULL);
    if (!g_hThread) {
        InterlockedExchange(&g_running, 0);
        return;
    }
    CloseHandle(g_hThread);
    g_hThread = NULL;
}

static void stop_test(void) {
    InterlockedExchange(&g_stop, 1);
}

static void layout(void) {
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    int x = 10, y = 10, w = rc.right - 20;

    MoveWindow(g_hHostA, x + 40,  y, 170, 24, TRUE);
    MoveWindow(g_hPortA, x + 215, y, 70, 24, TRUE);
    MoveWindow(g_hHostB, x + 335, y, 170, 24, TRUE);
    MoveWindow(g_hPortB, x + 510, y, 70, 24, TRUE);
    MoveWindow(g_hDur,   x + 680, y, 70, 24, TRUE);
    MoveWindow(g_hStart, x + 770, y, 80, 24, TRUE);
    MoveWindow(g_hStop,  x + 860, y, 80, 24, TRUE);

    MoveWindow(g_hList, x, 45, w, rc.bottom - 180, TRUE);
    MoveWindow(g_hLog,  x, rc.bottom - 125, w, 95, TRUE);
}

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE: {
        INITCOMMONCONTROLSEX ic = { sizeof(ic), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES };
        InitCommonControlsEx(&ic);

        g_hHostA = CreateWindowW(L"EDIT", L"1.1.1.1", WS_CHILD|WS_VISIBLE|WS_BORDER|ES_AUTOHSCROLL, 0,0,0,0, h,(HMENU)IDC_HOSTA,g_hInst,NULL);
        g_hPortA = CreateWindowW(L"EDIT", L"80", WS_CHILD|WS_VISIBLE|WS_BORDER|ES_AUTOHSCROLL, 0,0,0,0, h,(HMENU)IDC_PORTA,g_hInst,NULL);
        g_hHostB = CreateWindowW(L"EDIT", L"8.8.8.8", WS_CHILD|WS_VISIBLE|WS_BORDER|ES_AUTOHSCROLL, 0,0,0,0, h,(HMENU)IDC_HOSTB,g_hInst,NULL);
        g_hPortB = CreateWindowW(L"EDIT", L"80", WS_CHILD|WS_VISIBLE|WS_BORDER|ES_AUTOHSCROLL, 0,0,0,0, h,(HMENU)IDC_PORTB,g_hInst,NULL);
        g_hDur   = CreateWindowW(L"EDIT", L"50", WS_CHILD|WS_VISIBLE|WS_BORDER|ES_AUTOHSCROLL, 0,0,0,0, h,(HMENU)IDC_DUR,g_hInst,NULL);
        g_hStart = CreateWindowW(L"BUTTON", L"开始", WS_CHILD|WS_VISIBLE, 0,0,0,0, h,(HMENU)IDC_START,g_hInst,NULL);
        g_hStop  = CreateWindowW(L"BUTTON", L"停止", WS_CHILD|WS_VISIBLE, 0,0,0,0, h,(HMENU)IDC_STOP,g_hInst,NULL);

        CreateWindowW(L"STATIC", L"A", WS_CHILD|WS_VISIBLE, 10,13,20,20, h, NULL, g_hInst, NULL);
        CreateWindowW(L"STATIC", L"B", WS_CHILD|WS_VISIBLE, 290,13,20,20, h, NULL, g_hInst, NULL);
        CreateWindowW(L"STATIC", L"秒", WS_CHILD|WS_VISIBLE, 655,13,20,20, h, NULL, g_hInst, NULL);

        g_hList = CreateWindowW(WC_LISTVIEWW, L"", WS_CHILD|WS_VISIBLE|WS_BORDER|LVS_REPORT,
                                0,0,0,0, h,(HMENU)IDC_LIST,g_hInst,NULL);
        g_hLog  = CreateWindowW(L"EDIT", L"", WS_CHILD|WS_VISIBLE|WS_BORDER|ES_MULTILINE|ES_READONLY|WS_VSCROLL,
                                0,0,0,0, h,(HMENU)IDC_LOG,g_hInst,NULL);

        list_init();
        layout();

        g_hIcon = (HICON)LoadImageW(NULL, L"TCPing_GUI.ico", IMAGE_ICON, 32, 32, LR_LOADFROMFILE);
        if (g_hIcon) {
            SendMessageW(h, WM_SETICON, ICON_BIG, (LPARAM)g_hIcon);
            SendMessageW(h, WM_SETICON, ICON_SMALL, (LPARAM)g_hIcon);
        }
        return 0;
    }
    case WM_SIZE:
        layout();
        return 0;

    case WM_COMMAND:
        switch (LOWORD(w)) {
        case IDC_START: start_test(); break;
        case IDC_STOP:  stop_test(); break;
        }
        return 0;

    case WM_APP_ROW:
        set_row((ROW *)l);
        return 0;

    case WM_APP_LOG:
        append_log((const wchar_t *)l);
        return 0;

    case WM_APP_DONE:
        InterlockedExchange(&g_running, 0);
        return 0;

    case WM_DESTROY:
        stop_test();
        if (g_hIcon) DestroyIcon(g_hIcon);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR lpCmdLine, int nShow) {
    (void)hPrev;
    (void)lpCmdLine;
    g_hInst = hInst;

    WNDCLASSW wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"TCPingMain";
    RegisterClassW(&wc);

    g_hwnd = CreateWindowW(wc.lpszClassName, L"TCPing GUI",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 980, 620,
        NULL, NULL, hInst, NULL);

    if (!g_hwnd) return 0;

    ShowWindow(g_hwnd, nShow);
    UpdateWindow(g_hwnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
