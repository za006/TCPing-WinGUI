#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "ws2_32.lib")

#define IDC_HOST1     1001
#define IDC_PORT1     1002
#define IDC_HOST2     1003
#define IDC_PORT2     1004
#define IDC_DUR       1005
#define IDC_START     1006
#define IDC_STOP      1007
#define IDC_LIST      1008
#define IDC_SUMMARY   1009

#define APP_ICON_ID   101

typedef struct {
    wchar_t host1[256];
    wchar_t host2[256];
    int port1;
    int port2;
    int durationSec;
} PingParams;

typedef struct {
    DWORD latency;
    BOOL ok;
} PingResult;

static HWND g_hHost1, g_hPort1, g_hHost2, g_hPort2, g_hDur;
static HWND g_hStart, g_hStop, g_hList, g_hSummary;
static HANDLE g_hThread = NULL;
static volatile BOOL g_running = FALSE;
static HWND g_hwndMain = NULL;
static HFONT g_hFontUI = NULL;
static HFONT g_hFontMono = NULL;

static void SetAppIcon(HWND hwnd);
static HFONT MakeFont(int height, const wchar_t* face);
static void AppendSummaryLine(const wchar_t* line);
static void AddListRow(const wchar_t* timeText, const wchar_t* aText, const wchar_t* bText, const wchar_t* aBar, const wchar_t* bBar, const wchar_t* note);
static void BuildBar(wchar_t* out, int maxCount, DWORD latency);
static const wchar_t* LatTag(DWORD latency);
static BOOL TCPPing(const wchar_t* host, int port, DWORD* latencyMs);
static DWORD WINAPI PingThread(LPVOID lpParam);
static void StartPing(void);
static void StopPing(void);
static void InitListView(HWND hwndList);

static HFONT MakeFont(int height, const wchar_t* face)
{
    return CreateFontW(
        height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH, face
    );
}

static void SetAppIcon(HWND hwnd)
{
    HICON hIcon = (HICON)LoadImageW(
        GetModuleHandleW(NULL),
        MAKEINTRESOURCEW(APP_ICON_ID),
        IMAGE_ICON,
        0, 0,
        LR_DEFAULTSIZE);

    if (hIcon) {
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)hIcon);
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hIcon);
    }
}

static void AppendSummaryLine(const wchar_t* line)
{
    int len = GetWindowTextLengthW(g_hSummary);
    SendMessageW(g_hSummary, EM_SETSEL, len, len);
    SendMessageW(g_hSummary, EM_REPLACESEL, FALSE, (LPARAM)line);
    SendMessageW(g_hSummary, EM_REPLACESEL, FALSE, (LPARAM)L"\r\n");
    SendMessageW(g_hSummary, EM_SCROLLCARET, 0, 0);
}

static void AddListRow(const wchar_t* timeText, const wchar_t* aText, const wchar_t* bText, const wchar_t* aBar, const wchar_t* bBar, const wchar_t* note)
{
    LVITEMW item;
    int idx;

    ZeroMemory(&item, sizeof(item));
    item.mask = LVIF_TEXT;
    item.iItem = ListView_GetItemCount(g_hList);
    item.iSubItem = 0;
    item.pszText = (LPWSTR)timeText;
    idx = ListView_InsertItemW(g_hList, &item);

    ListView_SetItemText(g_hList, idx, 1, (LPWSTR)aText);
    ListView_SetItemText(g_hList, idx, 2, (LPWSTR)bText);
    ListView_SetItemText(g_hList, idx, 3, (LPWSTR)aBar);
    ListView_SetItemText(g_hList, idx, 4, (LPWSTR)bBar);
    ListView_SetItemText(g_hList, idx, 5, (LPWSTR)note);

    ListView_EnsureVisible(g_hList, idx, FALSE);
}

static void BuildBar(wchar_t* out, int maxCount, DWORD latency)
{
    int n = (int)(latency / 5);
    if (n < 0) n = 0;
    if (n > maxCount) n = maxCount;

    for (int i = 0; i < n; i++) out[i] = L'█';
    out[n] = 0;
}

static const wchar_t* LatTag(DWORD latency)
{
    if (latency <= 1500) return L"绿";
    return L"黄";
}

static BOOL TCPPing(const wchar_t* host, int port, DWORD* latencyMs)
{
    struct addrinfoW hints;
    struct addrinfoW* res = NULL;
    struct addrinfoW* p = NULL;
    wchar_t portStr[16];
    SOCKET s = INVALID_SOCKET;
    LARGE_INTEGER t1, t2, freq;

    ZeroMemory(&hints, sizeof(hints));
    wsprintfW(portStr, L"%d", port);

    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    if (GetAddrInfoW(host, portStr, &hints, &res) != 0)
        return FALSE;

    if (!QueryPerformanceFrequency(&freq)) {
        FreeAddrInfoW(res);
        return FALSE;
    }

    for (p = res; p != NULL; p = p->ai_next) {
        s = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (s == INVALID_SOCKET)
            continue;

        QueryPerformanceCounter(&t1);
        if (connect(s, p->ai_addr, (int)p->ai_addrlen) == 0) {
            QueryPerformanceCounter(&t2);
            *latencyMs = (DWORD)((t2.QuadPart - t1.QuadPart) * 1000 / freq.QuadPart);
            closesocket(s);
            FreeAddrInfoW(res);
            return TRUE;
        }

        closesocket(s);
        s = INVALID_SOCKET;
    }

    FreeAddrInfoW(res);
    return FALSE;
}

static DWORD WINAPI PingThread(LPVOID lpParam)
{
    PingParams* p = (PingParams*)lpParam;
    DWORD startTick = GetTickCount();

    int sentA = 0, okA = 0;
    int sentB = 0, okB = 0;
    DWORD sumA = 0, sumB = 0;
    DWORD minA = 0xFFFFFFFF, minB = 0xFFFFFFFF;
    DWORD maxA = 0, maxB = 0;

    while (g_running && (GetTickCount() - startTick) < (DWORD)(p->durationSec * 1000)) {
        SYSTEMTIME st;
        wchar_t timeText[32];
        wchar_t aText[64], bText[64];
        wchar_t aBar[128], bBar[128];
        wchar_t note[64];

        PingResult ra = { 0 }, rb = { 0 };

        GetLocalTime(&st);
        wsprintfW(timeText, L"%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);

        ra.ok = TCPPing(p->host1, p->port1, &ra.latency);
        rb.ok = TCPPing(p->host2, p->port2, &rb.latency);

        sentA++;
        sentB++;

        if (ra.ok) {
            okA++;
            sumA += ra.latency;
            if (ra.latency < minA) minA = ra.latency;
            if (ra.latency > maxA) maxA = ra.latency;
        }

        if (rb.ok) {
            okB++;
            sumB += rb.latency;
            if (rb.latency < minB) minB = rb.latency;
            if (rb.latency > maxB) maxB = rb.latency;
        }

        BuildBar(aBar, 40, ra.ok ? ra.latency : 0);
        BuildBar(bBar, 40, rb.ok ? rb.latency : 0);

        if (ra.ok)
            wsprintfW(aText, L"%lu ms [%s]", ra.latency, LatTag(ra.latency));
        else
            lstrcpyW(aText, L"TIMEOUT [红]");

        if (rb.ok)
            wsprintfW(bText, L"%lu ms [%s]", rb.latency, LatTag(rb.latency));
        else
            lstrcpyW(bText, L"TIMEOUT [红]");

        if (ra.ok && rb.ok)
            lstrcpyW(note, L"OK");
        else if (!ra.ok && !rb.ok)
            lstrcpyW(note, L"双失败");
        else
            lstrcpyW(note, L"单失败");

        AddListRow(timeText, aText, bText, aBar, bBar, note);
        Sleep(1000);
    }

    {
        wchar_t stat[1024];
        double lossA = sentA ? (100.0 * (sentA - okA) / sentA) : 0.0;
        double lossB = sentB ? (100.0 * (sentB - okB) / sentB) : 0.0;
        double avgA = okA ? (double)sumA / okA : 0.0;
        double avgB = okB ? (double)sumB / okB : 0.0;

        wsprintfW(stat,
            L"===== 统计 =====\r\n"
            L"A目标: 发送=%d, 接收=%d, 丢失=%d(%.0f%%), 最短=%lu, 最长=%lu, 平均=%.0fms\r\n"
            L"B目标: 发送=%d, 接收=%d, 丢失=%d(%.0f%%), 最短=%lu, 最长=%lu, 平均=%.0fms\r\n",
            sentA, okA, sentA - okA, lossA,
            minA == 0xFFFFFFFF ? 0 : minA, maxA, avgA,
            sentB, okB, sentB - okB, lossB,
            minB == 0xFFFFFFFF ? 0 : minB, maxB, avgB);

        AppendSummaryLine(stat);
    }

    free(p);
    g_running = FALSE;
    EnableWindow(g_hStart, TRUE);
    EnableWindow(g_hStop, FALSE);
    return 0;
}

static void StartPing(void)
{
    wchar_t buf[256];
    PingParams* params = NULL;

    if (g_running)
        return;

    params = (PingParams*)calloc(1, sizeof(PingParams));
    if (!params)
        return;

    GetWindowTextW(g_hHost1, params->host1, 256);
    GetWindowTextW(g_hPort1, buf, 256);
    params->port1 = _wtoi(buf);

    GetWindowTextW(g_hHost2, params->host2, 256);
    GetWindowTextW(g_hPort2, buf, 256);
    params->port2 = _wtoi(buf);

    GetWindowTextW(g_hDur, buf, 256);
    params->durationSec = _wtoi(buf);
    if (params->durationSec <= 0)
        params->durationSec = 51;

    if (params->host1[0] == 0 || params->host2[0] == 0 || params->port1 <= 0 || params->port2 <= 0) {
        MessageBoxW(g_hwndMain, L"请输入两个目标和有效端口", L"提示", MB_OK | MB_ICONINFORMATION);
        free(params);
        return;
    }

    SendMessageW(g_hList, LVM_DELETEALLITEMS, 0, 0);
    SetWindowTextW(g_hSummary, L"开始测试...\r\n");

    g_running = TRUE;
    EnableWindow(g_hStart, FALSE);
    EnableWindow(g_hStop, TRUE);

    g_hThread = CreateThread(NULL, 0, PingThread, params, 0, NULL);
    if (!g_hThread) {
        g_running = FALSE;
        EnableWindow(g_hStart, TRUE);
        EnableWindow(g_hStop, FALSE);
        free(params);
    }
}

static void StopPing(void)
{
    g_running = FALSE;
    AppendSummaryLine(L"请求停止，等待统计输出...");
}

static void InitListView(HWND hwndList)
{
    LVCOLUMNW col;
    DWORD style;

    style = ListView_GetExtendedListViewStyle(hwndList);
    style |= LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER;
    ListView_SetExtendedListViewStyle(hwndList, style);

    ZeroMemory(&col, sizeof(col));
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;

    col.pszText = L"时间";
    col.cx = 95;
    col.iSubItem = 0;
    ListView_InsertColumn(hwndList, 0, &col);

    col.pszText = L"A目标";
    col.cx = 130;
    col.iSubItem = 1;
    ListView_InsertColumn(hwndList, 1, &col);

    col.pszText = L"B目标";
    col.cx = 130;
    col.iSubItem = 2;
    ListView_InsertColumn(hwndList, 2, &col);

    col.pszText = L"A条形";
    col.cx = 250;
    col.iSubItem = 3;
    ListView_InsertColumn(hwndList, 3, &col);

    col.pszText = L"B条形";
    col.cx = 250;
    col.iSubItem = 4;
    ListView_InsertColumn(hwndList, 4, &col);

    col.pszText = L"备注";
    col.cx = 90;
    col.iSubItem = 5;
    ListView_InsertColumn(hwndList, 5, &col);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE:
    {
        g_hwndMain = hwnd;
        SetAppIcon(hwnd);

        CreateWindowW(L"STATIC", L"目标A", WS_CHILD | WS_VISIBLE,
            20, 20, 60, 20, hwnd, NULL, NULL, NULL);
        g_hHost1 = CreateWindowW(L"EDIT", L"192.168.1.1",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            80, 18, 220, 24, hwnd, (HMENU)IDC_HOST1, NULL, NULL);
        g_hPort1 = CreateWindowW(L"EDIT", L"80",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            310, 18, 60, 24, hwnd, (HMENU)IDC_PORT1, NULL, NULL);

        CreateWindowW(L"STATIC", L"目标B", WS_CHILD | WS_VISIBLE,
            20, 55, 60, 20, hwnd, NULL, NULL, NULL);
        g_hHost2 = CreateWindowW(L"EDIT", L"z.cn",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            80, 53, 220, 24, hwnd, (HMENU)IDC_HOST2, NULL, NULL);
        g_hPort2 = CreateWindowW(L"EDIT", L"443",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            310, 53, 60, 24, hwnd, (HMENU)IDC_PORT2, NULL, NULL);

        CreateWindowW(L"STATIC", L"持续秒数", WS_CHILD | WS_VISIBLE,
            20, 90, 60, 20, hwnd, NULL, NULL, NULL);
        g_hDur = CreateWindowW(L"EDIT", L"51",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            80, 88, 80, 24, hwnd, (HMENU)IDC_DUR, NULL, NULL);

        g_hStart = CreateWindowW(L"BUTTON", L"开始",
            WS_CHILD | WS_VISIBLE,
            180, 88, 80, 26, hwnd, (HMENU)IDC_START, NULL, NULL);
        g_hStop = CreateWindowW(L"BUTTON", L"停止",
            WS_CHILD | WS_VISIBLE | WS_DISABLED,
            270, 88, 80, 26, hwnd, (HMENU)IDC_STOP, NULL, NULL);

        g_hList = CreateWindowW(WC_LISTVIEWW, L"",
            WS_CHILD | WS_VISIBLE | WS_BORDER | LVS_REPORT | LVS_SINGLESEL,
            20, 130, 840, 320, hwnd, (HMENU)IDC_LIST, NULL, NULL);

        g_hSummary = CreateWindowW(L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_MULTILINE |
            ES_AUTOVSCROLL | WS_VSCROLL | ES_READONLY | ES_WANTRETURN | ES_NOHIDESEL,
            20, 460, 840, 120, hwnd, (HMENU)IDC_SUMMARY, NULL, NULL);

        g_hFontUI = MakeFont(18, L"Segoe UI");
        g_hFontMono = MakeFont(18, L"Cascadia Mono");

        SendMessageW(g_hHost1, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        SendMessageW(g_hPort1, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        SendMessageW(g_hHost2, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        SendMessageW(g_hPort2, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        SendMessageW(g_hDur, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        SendMessageW(g_hStart, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        SendMessageW(g_hStop, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
        SendMessageW(g_hList, WM_SETFONT, (WPARAM)g_hFontMono, TRUE);
        SendMessageW(g_hSummary, WM_SETFONT, (WPARAM)g_hFontMono, TRUE);

        InitListView(g_hList);
        SetWindowTextW(g_hSummary, L"准备就绪。\r\n");
        break;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_START:
            StartPing();
            break;
        case IDC_STOP:
            StopPing();
            break;
        }
        break;

    case WM_DESTROY:
        g_running = FALSE;
        if (g_hThread) {
            WaitForSingleObject(g_hThread, 3000);
            CloseHandle(g_hThread);
            g_hThread = NULL;
        }
        if (g_hFontUI) {
            DeleteObject(g_hFontUI);
            g_hFontUI = NULL;
        }
        if (g_hFontMono) {
            DeleteObject(g_hFontMono);
            g_hFontMono = NULL;
        }
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return 0;
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrev, PWSTR lpCmdLine, int nCmdShow)
{
    WNDCLASSW wc;
    HWND hwnd;
    MSG msg;
    WSADATA wsa;
    INITCOMMONCONTROLSEX icc;

    ZeroMemory(&icc, sizeof(icc));
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icc);

    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"TCPingCompareWnd";
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hIcon = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(APP_ICON_ID), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);

    if (!RegisterClassW(&wc))
        return 0;

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        return 0;

    hwnd = CreateWindowW(
        L"TCPingCompareWnd",
        L"TCPing 双目标对比",
        WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 900, 650,
        NULL, NULL, hInstance, NULL);

    if (!hwnd) {
        WSACleanup();
        return 0;
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    WSACleanup();
    return (int)msg.wParam;
}
