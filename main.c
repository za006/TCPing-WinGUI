#define UNICODE
#define _UNICODE
#define _WIN32_WINNT 0x0600

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <commctrl.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define IDC_IPEDIT         101
#define IDC_PORTEDIT       102
#define IDC_COUNTEDIT      103
#define IDC_LOGEDIT        104
#define IDC_STARTBUTTON    105
#define IDC_STOPBUTTON     106
#define ID_MENU_COPY       107
#define ID_MENU_SELECT_ALL 108

#define MAX_LOG_CHARS      32768

static HWND hIpEdit, hPortEdit, hCountEdit, hLogEdit, hStartButton, hStopButton;
static HFONT hFont = NULL;
static HFONT hFontBold = NULL;
static volatile LONG g_running = 0;
static volatile LONG g_stop = 0;
static HANDLE g_thread = NULL;
static HWND g_hwnd = NULL;

typedef struct _PingStats {
    int pingCount, successCount, failCount;
    double totalTime, minTime, maxTime;
    int maxPingLimit;
} PingStats;

static void AppendLog(const wchar_t* text);
static void StartPing(void);
static void StopPing(void);
static void PingThreadCleanup(PingStats* stats);
static DWORD WINAPI PingThread(LPVOID lpParam);
static int TCPPing(const wchar_t* host, int port, double* elapsed);

static void SafeSetWindowText(HWND h, const wchar_t* s) {
    SetWindowTextW(h, s ? s : L"");
}

static void TrimLogIfNeeded(void) {
    int len = GetWindowTextLengthW(hLogEdit);
    if (len < MAX_LOG_CHARS) return;

    int cut = len / 2;
    SendMessageW(hLogEdit, EM_SETSEL, 0, cut);
    SendMessageW(hLogEdit, EM_REPLACESEL, TRUE, (LPARAM)L"");
}

static void AppendLog(const wchar_t* text) {
    if (!text) return;
    int oldLen = GetWindowTextLengthW(hLogEdit);
    SendMessageW(hLogEdit, EM_SETSEL, oldLen, oldLen);
    SendMessageW(hLogEdit, EM_REPLACESEL, FALSE, (LPARAM)text);
    SendMessageW(hLogEdit, EM_REPLACESEL, FALSE, (LPARAM)L"\r\n");
    TrimLogIfNeeded();
    SendMessageW(hLogEdit, EM_SCROLLCARET, 0, 0);
}

static void SetControlsRunning(BOOL running) {
    EnableWindow(hStartButton, !running);
    EnableWindow(hStopButton, running);
}

static void StartPing(void) {
    if (InterlockedCompareExchange(&g_running, 1, 0) != 0) return;

    wchar_t host[256] = {0};
    wchar_t portStr[32] = {0};
    wchar_t countStr[32] = {0};

    GetWindowTextW(hIpEdit, host, 255);
    GetWindowTextW(hPortEdit, portStr, 31);
    GetWindowTextW(hCountEdit, countStr, 31);

    int port = _wtoi(portStr);
    int maxCount = _wtoi(countStr);

    if (host[0] == 0 || port <= 0 || port > 65535) {
        MessageBoxW(g_hwnd, L"请输入有效的IP/域名和端口（1-65535）", L"错误", MB_ICONERROR);
        InterlockedExchange(&g_running, 0);
        return;
    }

    if (maxCount < 0) maxCount = 0;

    SetWindowTextW(hLogEdit, L"");
    AppendLog(L"开始 TCPing...");
    SetControlsRunning(TRUE);

    typedef struct {
        wchar_t host[256];
        int port;
        PingStats* stats;
    } PingParams;

    PingStats* stats = (PingStats*)calloc(1, sizeof(PingStats));
    PingParams* params = (PingParams*)calloc(1, sizeof(PingParams));
    if (!stats || !params) {
        free(stats);
        free(params);
        MessageBoxW(g_hwnd, L"内存分配失败", L"错误", MB_ICONERROR);
        InterlockedExchange(&g_running, 0);
        SetControlsRunning(FALSE);
        return;
    }

    wcscpy_s(params->host, 256, host);
    params->port = port;
    params->stats = stats;

    stats->minTime = 999999.0;
    stats->maxPingLimit = maxCount;

    InterlockedExchange(&g_stop, 0);
    g_thread = CreateThread(NULL, 0, PingThread, params, 0, NULL);
    if (!g_thread) {
        free(stats);
        free(params);
        InterlockedExchange(&g_running, 0);
        SetControlsRunning(FALSE);
        AppendLog(L"启动线程失败。");
        return;
    }
    CloseHandle(g_thread);
}

static void StopPing(void) {
    if (InterlockedCompareExchange(&g_running, 0, 1) != 1) return;
    InterlockedExchange(&g_stop, 1);
    AppendLog(L"正在停止...");
    SetControlsRunning(FALSE);
}

static void PingThreadCleanup(PingStats* finalStats) {
    wchar_t buf[256];

    InterlockedExchange(&g_running, 0);
    InterlockedExchange(&g_stop, 0);
    SetControlsRunning(FALSE);

    AppendLog(L"");
    AppendLog(L"Ping 统计信息:");

    if (finalStats && finalStats->pingCount > 0) {
        swprintf(buf, 256, L"    数据包: 已发送 = %d, 已接收 = %d, 丢失 = %d (%.0f%% 丢失)",
                 finalStats->pingCount,
                 finalStats->successCount,
                 finalStats->failCount,
                 finalStats->pingCount ? (finalStats->failCount * 100.0 / finalStats->pingCount) : 0.0);
        AppendLog(buf);

        AppendLog(L"往返行程的估计时间(以毫秒为单位):");

        swprintf(buf, 256, L"    最短 = %.0fms, 最长 = %.0fms, 平均 = %.0fms",
                 finalStats->minTime == 999999.0 ? 0.0 : finalStats->minTime,
                 finalStats->maxTime,
                 finalStats->successCount > 0 ? (finalStats->totalTime / finalStats->successCount) : 0.0);
        AppendLog(buf);
    } else {
        AppendLog(L"    Ping 已停止。");
    }

    free(finalStats);
}

static DWORD WINAPI PingThread(LPVOID lpParam) {
    typedef struct {
        wchar_t host[256];
        int port;
        PingStats* stats;
    } PingParams;

    PingParams* params = (PingParams*)lpParam;
    wchar_t host[256];
    wcscpy_s(host, 256, params->host);
    int port = params->port;
    PingStats* stats = params->stats;
    free(params);

    wchar_t startMsg[256];
    swprintf(startMsg, 256, L"正在 Ping %s:%d ...", host, port);
    AppendLog(startMsg);

    while (!InterlockedCompareExchange(&g_stop, 0, 0) &&
           (stats->maxPingLimit == 0 || stats->pingCount < stats->maxPingLimit)) {
        double elapsed = 0.0;
        int result = TCPPing(host, port, &elapsed);

        if (InterlockedCompareExchange(&g_stop, 0, 0)) break;

        stats->pingCount++;

        wchar_t line[256];
        if (result == 0) {
            stats->successCount++;
            stats->totalTime += elapsed;
            if (elapsed < stats->minTime) stats->minTime = elapsed;
            if (elapsed > stats->maxTime) stats->maxTime = elapsed;

            swprintf(line, 256, L"来自 %s 的回复: 时间=%.0fms", host, elapsed);
            AppendLog(line);
        } else {
            stats->failCount++;
            AppendLog(L"请求超时。");
        }

        for (int i = 0; i < 10; ++i) {
            if (InterlockedCompareExchange(&g_stop, 0, 0)) break;
            Sleep(100);
        }
    }

    PostMessageW(g_hwnd, WM_USER + 1, 0, (LPARAM)stats);
    return 0;
}

static int TCPPing(const wchar_t* host, int port, double* elapsed) {
    struct addrinfoW hints, *res = NULL, *it = NULL;
    wchar_t portStr[16];
    SOCKET sock = INVALID_SOCKET;
    LARGE_INTEGER freq, t1, t2;

    swprintf(portStr, 16, L"%d", port);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    if (GetAddrInfoW(host, portStr, &hints, &res) != 0) {
        return -1;
    }

    QueryPerformanceFrequency(&freq);

    for (it = res; it != NULL; it = it->ai_next) {
        sock = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (sock == INVALID_SOCKET) continue;

        u_long nb = 1;
        ioctlsocket(sock, FIONBIO, &nb);

        QueryPerformanceCounter(&t1);
        connect(sock, it->ai_addr, (int)it->ai_addrlen);

        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(sock, &wfds);

        struct timeval tv;
        tv.tv_sec = 2;
        tv.tv_usec = 0;

        int r = select(0, NULL, &wfds, NULL, &tv);
        QueryPerformanceCounter(&t2);

        if (r > 0 && FD_ISSET(sock, &wfds)) {
            int so_error = 0;
            int slen = sizeof(so_error);
            getsockopt(sock, SOL_SOCKET, SO_ERROR, (char*)&so_error, &slen);
            if (so_error == 0) {
                *elapsed = (double)(t2.QuadPart - t1.QuadPart) * 1000.0 / (double)freq.QuadPart;
                closesocket(sock);
                FreeAddrInfoW(res);
                return 0;
            }
        }

        closesocket(sock);
        sock = INVALID_SOCKET;
    }

    if (res) FreeAddrInfoW(res);
    return -1;
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        CreateWindowW(L"Button", L"参数设置", WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                      14, 12, 772, 130, hwnd, NULL, GetModuleHandleW(NULL), NULL);

        CreateWindowW(L"Static", L"IP/域名:", WS_CHILD | WS_VISIBLE,
                      34, 42, 80, 22, hwnd, NULL, NULL, NULL);
        hIpEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"Edit", L"360.com",
                      WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                      120, 38, 250, 26, hwnd, (HMENU)IDC_IPEDIT, GetModuleHandleW(NULL), NULL);

        CreateWindowW(L"Static", L"端口:", WS_CHILD | WS_VISIBLE,
                      390, 42, 40, 22, hwnd, NULL, NULL, NULL);
        hPortEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"Edit", L"80",
                      WS_CHILD | WS_VISIBLE | ES_NUMBER | ES_AUTOHSCROLL,
                      440, 38, 90, 26, hwnd, (HMENU)IDC_PORTEDIT, GetModuleHandleW(NULL), NULL);

        CreateWindowW(L"Static", L"次数:", WS_CHILD | WS_VISIBLE,
                      550, 42, 40, 22, hwnd, NULL, NULL, NULL);
        hCountEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"Edit", L"4",
                      WS_CHILD | WS_VISIBLE | ES_NUMBER | ES_AUTOHSCROLL,
                      600, 38, 90, 26, hwnd, (HMENU)IDC_COUNTEDIT, GetModuleHandleW(NULL), NULL);

        hStartButton = CreateWindowW(L"Button", L"开始",
                      WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                      120, 78, 120, 30, hwnd, (HMENU)IDC_STARTBUTTON, GetModuleHandleW(NULL), NULL);

        hStopButton = CreateWindowW(L"Button", L"停止",
                      WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_DISABLED,
                      260, 78, 120, 30, hwnd, (HMENU)IDC_STOPBUTTON, GetModuleHandleW(NULL), NULL);

        CreateWindowW(L"Button", L"日志输出", WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                      14, 154, 772, 440, hwnd, NULL, GetModuleHandleW(NULL), NULL);

        hLogEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"Edit", L"",
                      WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                      28, 178, 744, 400, hwnd, (HMENU)IDC_LOGEDIT, GetModuleHandleW(NULL), NULL);

        return 0;
    }

    case WM_COMMAND: {
        switch (LOWORD(wParam)) {
        case IDC_STARTBUTTON:
            StartPing();
            break;
        case IDC_STOPBUTTON:
            StopPing();
            break;
        case ID_MENU_COPY:
            SendMessageW(hLogEdit, WM_COPY, 0, 0);
            break;
        case ID_MENU_SELECT_ALL:
            SendMessageW(hLogEdit, EM_SETSEL, 0, -1);
            break;
        }
        return 0;
    }

    case WM_CONTEXTMENU: {
        if ((HWND)wParam == hLogEdit) {
            HMENU hMenu = CreatePopupMenu();
            AppendMenuW(hMenu, MF_STRING, ID_MENU_COPY, L"复制");
            AppendMenuW(hMenu, MF_STRING, ID_MENU_SELECT_ALL, L"全选");

            DWORD start = 0, end = 0;
            SendMessageW(hLogEdit, EM_GETSEL, (WPARAM)&start, (LPARAM)&end);
            if (start == end) EnableMenuItem(hMenu, ID_MENU_COPY, MF_BYCOMMAND | MF_GRAYED);

            TrackPopupMenu(hMenu, TPM_RIGHTBUTTON | TPM_TOPALIGN | TPM_LEFTALIGN,
                           GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), 0, hwnd, NULL);
            DestroyMenu(hMenu);
        }
        return 0;
    }

    case WM_USER + 1:
        PingThreadCleanup((PingStats*)lParam);
        return 0;

    case WM_DESTROY:
        StopPing();
        if (hFont) DeleteObject(hFont);
        if (hFontBold) DeleteObject(hFontBold);
        PostQuitMessage(0);
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = (MINMAXINFO*)lParam;
        mmi->ptMinTrackSize.x = 800;
        mmi->ptMinTrackSize.y = 650;
        mmi->ptMaxTrackSize.x = 800;
        mmi->ptMaxTrackSize.y = 650;
        return 0;
    }
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;

    WSAData wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        MessageBoxW(NULL, L"WSAStartup 失败", L"错误", MB_ICONERROR);
        return 1;
    }

    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(icex);
    icex.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icex);

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"TCPingWindowClass";
    wc.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    wc.hIconSm = LoadIconW(NULL, IDI_APPLICATION);

    if (!RegisterClassExW(&wc)) {
        WSACleanup();
        return 1;
    }

    g_hwnd = CreateWindowExW(
        0, L"TCPingWindowClass", L"TCP Ping 工具",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 800, 650,
        NULL, NULL, hInstance, NULL
    );

    if (!g_hwnd) {
        WSACleanup();
        return 1;
    }

    LOGFONTW lf;
    memset(&lf, 0, sizeof(lf));
    lf.lfHeight = -11;
    lf.lfWeight = FW_NORMAL;
    wcscpy_s(lf.lfFaceName, LF_FACESIZE, L"Segoe UI");
    hFont = CreateFontIndirectW(&lf);
    lf.lfWeight = FW_BOLD;
    hFontBold = CreateFontIndirectW(&lf);

    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);

    SendMessageW(hIpEdit, WM_SETFONT, (WPARAM)hFont, TRUE);
    SendMessageW(hPortEdit, WM_SETFONT, (WPARAM)hFont, TRUE);
    SendMessageW(hCountEdit, WM_SETFONT, (WPARAM)hFont, TRUE);
    SendMessageW(hLogEdit, WM_SETFONT, (WPARAM)hFont, TRUE);
    SendMessageW(hStartButton, WM_SETFONT, (WPARAM)hFontBold, TRUE);
    SendMessageW(hStopButton, WM_SETFONT, (WPARAM)hFontBold, TRUE);

    SetWindowTextW(hLogEdit, L"就绪。");

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    WSACleanup();
    return 0;
}
