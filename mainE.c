// mainE.c
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>

#pragma comment(lib, "comctl32.lib")

// ====== 与 main.c 共享的消息定义 ======
#ifndef WM_APP_LOG
#define WM_APP_LOG (WM_APP + 1)
#endif

#ifndef WM_APP_ROW
#define WM_APP_ROW (WM_APP + 2)
#endif

#ifndef WM_APP_STATUS
#define WM_APP_STATUS (WM_APP + 3)
#endif

#ifndef WM_APP_DONE
#define WM_APP_DONE (WM_APP + 4)
#endif

// ====== 由 main.c 提供的外部函数/变量 ======
extern void StartTcping(HWND hwnd);
extern void StopTcping(void);
extern void OpenWebInterface(void);

extern HINSTANCE g_hInst;
extern HWND g_hwndMain;

// ====== UI 控件句柄 ======
static HWND g_hList = NULL;
static HWND g_hLog  = NULL;
static HWND g_hBtnStart = NULL;
static HWND g_hBtnStop  = NULL;
static HWND g_hBtnWeb    = NULL;
static HWND g_hStatus    = NULL;

// ====== 列表行数据（需与 main.c 发送的数据结构对应） ======
typedef struct TCPING_ROW {
    int     id;
    wchar_t target[256];
    int     sent;
    int     received;
    int     lost;
    int     last_ms;
    int     min_ms;
    int     avg_ms;
    int     max_ms;
    int     pct;           // 0~100，用于条形进度
    wchar_t state[64];
} TCPING_ROW;

// ====== 工具函数 ======
static void SetStatusText(const wchar_t* text)
{
    if (g_hStatus)
        SendMessageW(g_hStatus, SB_SETTEXTW, 0, (LPARAM)text);
}

static void AppendLogText(const wchar_t* text)
{
    if (!g_hLog || !text) return;

    int len = GetWindowTextLengthW(g_hLog);
    SendMessageW(g_hLog, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    SendMessageW(g_hLog, EM_REPLACESEL, FALSE, (LPARAM)text);
    SendMessageW(g_hLog, EM_REPLACESEL, FALSE, (LPARAM)L"\r\n");
    SendMessageW(g_hLog, EM_SCROLLCARET, 0, 0);
}

static void InitListViewColumns(HWND hList)
{
    LVCOLUMNW col;
    ZeroMemory(&col, sizeof(col));
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM | LVCF_FMT;

    col.fmt = LVCFMT_LEFT;
    col.pszText = L"目标";
    col.cx = 170;
    ListView_InsertColumn(hList, 0, &col);

    col.pszText = L"发送";
    col.cx = 60;
    ListView_InsertColumn(hList, 1, &col);

    col.pszText = L"接收";
    col.cx = 60;
    ListView_InsertColumn(hList, 2, &col);

    col.pszText = L"丢包";
    col.cx = 60;
    ListView_InsertColumn(hList, 3, &col);

    col.pszText = L"最后";
    col.cx = 70;
    ListView_InsertColumn(hList, 4, &col);

    col.pszText = L"最小";
    col.cx = 70;
    ListView_InsertColumn(hList, 5, &col);

    col.pszText = L"平均";
    col.cx = 70;
    ListView_InsertColumn(hList, 6, &col);

    col.pszText = L"最大";
    col.cx = 70;
    ListView_InsertColumn(hList, 7, &col);

    col.pszText = L"状态";
    col.cx = 120;
    ListView_InsertColumn(hList, 8, &col);
}

static void UpdateListRow(const TCPING_ROW* row)
{
    if (!g_hList || !row) return;

    LVITEMW item;
    wchar_t buf[64];

    ZeroMemory(&item, sizeof(item));
    item.mask = LVIF_TEXT | LVIF_PARAM;
    item.iItem = row->id;
    item.iSubItem = 0;
    item.pszText = (LPWSTR)row->target;
    item.lParam = (LPARAM)row->id;

    if (ListView_GetItemCount(g_hList) <= row->id) {
        ListView_InsertItem(g_hList, &item);
    } else {
        ListView_SetItemText(g_hList, row->id, 0, (LPWSTR)row->target);
    }

    swprintf(buf, 64, L"%d", row->sent);
    ListView_SetItemText(g_hList, row->id, 1, buf);

    swprintf(buf, 64, L"%d", row->received);
    ListView_SetItemText(g_hList, row->id, 2, buf);

    swprintf(buf, 64, L"%d%%", row->lost);
    ListView_SetItemText(g_hList, row->id, 3, buf);

    swprintf(buf, 64, L"%d ms", row->last_ms);
    ListView_SetItemText(g_hList, row->id, 4, buf);

    swprintf(buf, 64, L"%d ms", row->min_ms);
    ListView_SetItemText(g_hList, row->id, 5, buf);

    swprintf(buf, 64, L"%d ms", row->avg_ms);
    ListView_SetItemText(g_hList, row->id, 6, buf);

    swprintf(buf, 64, L"%d ms", row->max_ms);
    ListView_SetItemText(g_hList, row->id, 7, buf);

    ListView_SetItemText(g_hList, row->id, 8, (LPWSTR)row->state);
}

static void LayoutControls(HWND hwnd)
{
    RECT rc;
    GetClientRect(hwnd, &rc);

    const int pad = 10;
    const int top = 10;
    const int btnH = 28;
    const int btnW = 110;
    const int statusH = 22;
    const int logH = 140;

    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;

    MoveWindow(g_hBtnStart, pad, top, btnW, btnH, TRUE);
    MoveWindow(g_hBtnStop,  pad + btnW + 8, top, btnW, btnH, TRUE);
    MoveWindow(g_hBtnWeb,   pad + (btnW + 8) * 2, top, btnW + 20, btnH, TRUE);

    int listTop = top + btnH + 10;
    int listH = h - listTop - logH - statusH - 24;
    if (listH < 100) listH = 100;

    MoveWindow(g_hList, pad, listTop, w - pad * 2, listH, TRUE);

    int logTop = listTop + listH + 8;
    MoveWindow(g_hLog, pad, logTop, w - pad * 2, logH, TRUE);

    if (g_hStatus)
        SendMessageW(g_hStatus, WM_SIZE, 0, 0);
}

static HWND CreateMainControls(HWND hwnd)
{
    INITCOMMONCONTROLSEX icex = { sizeof(icex), ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_WIN95_CLASSES };
    InitCommonControlsEx(&icex);

    g_hBtnStart = CreateWindowW(L"BUTTON", L"开始测试",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 0, 0, hwnd, (HMENU)1001, g_hInst, NULL);

    g_hBtnStop = CreateWindowW(L"BUTTON", L"停止测试",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 0, 0, hwnd, (HMENU)1002, g_hInst, NULL);

    g_hBtnWeb = CreateWindowW(L"BUTTON", L"打开网页",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        0, 0, 0, 0, hwnd, (HMENU)1003, g_hInst, NULL);

    g_hList = CreateWindowW(WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | WS_BORDER | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        0, 0, 0, 0, hwnd, (HMENU)2001, g_hInst, NULL);

    ListView_SetExtendedListViewStyle(g_hList,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);

    InitListViewColumns(g_hList);

    g_hLog = CreateWindowW(L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
        0, 0, 0, 0, hwnd, (HMENU)2002, g_hInst, NULL);

    g_hStatus = CreateWindowExW(0, STATUSCLASSNAMEW, NULL,
        WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
        0, 0, 0, 0, hwnd, (HMENU)2003, g_hInst, NULL);

    SetStatusText(L"就绪");

    return hwnd;
}

// ====== 窗口过程 ======
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE:
        CreateMainControls(hwnd);
        LayoutControls(hwnd);
        return 0;

    case WM_SIZE:
        LayoutControls(hwnd);
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case 1001:
            SetStatusText(L"测试中...");
            StartTcping(hwnd);
            break;
        case 1002:
            StopTcping();
            SetStatusText(L"已停止");
            break;
        case 1003:
            OpenWebInterface();   // 仅按钮点击时打开
            break;
        }
        return 0;

    case WM_APP_LOG:
    {
        const wchar_t* line = (const wchar_t*)lParam;
        AppendLogText(line ? line : L"");
        return 0;
    }

    case WM_APP_ROW:
    {
        const TCPING_ROW* row = (const TCPING_ROW*)lParam;
        UpdateListRow(row);
        return 0;
    }

    case WM_APP_STATUS:
    {
        const wchar_t* text = (const wchar_t*)lParam;
        SetStatusText(text ? text : L"");
        return 0;
    }

    case WM_APP_DONE:
        SetStatusText(L"完成");
        return 0;

    case WM_DESTROY:
        StopTcping();
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ====== 程序入口 ======
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR lpCmdLine, int nShowCmd)
{
    (void)hPrev;
    (void)lpCmdLine;

    g_hInst = hInst;

    WNDCLASSW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"TCPingMainWindow";

    RegisterClassW(&wc);

    HWND hwnd = CreateWindowW(
        wc.lpszClassName,
        L"TCPing GUI",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1100, 720,
        NULL, NULL, hInst, NULL);

    if (!hwnd) return 0;

    g_hwndMain = hwnd;
    ShowWindow(hwnd, nShowCmd);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return (int)msg.wParam;
}
