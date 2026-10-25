#define UNICODE
#define _UNICODE

#include <windows.h>
#include <commctrl.h>

#pragma comment(lib, "comctl32.lib")

#define ID_EDIT_A   1001
#define ID_EDIT_B   1002
#define ID_EDIT_T   1003
#define ID_BTN_OK   1004
#define ID_BTN_STOP 1005
#define ID_LIST     1006
#define ID_LOG      1007
#define ID_LABEL_A  1101
#define ID_LABEL_B  1102
#define ID_LABEL_T  1103

static HWND g_hwndA, g_hwndB, g_hwndT;
static HWND g_hBtnOK, g_hBtnStop;
static HWND g_hList, g_hLog;
static HFONT g_hFont = NULL;
static HFONT g_hFontMono = NULL;

static HFONT MakeFont(int h, const wchar_t* face)
{
    return CreateFontW(
        h, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face
    );
}

static void ApplyFonts(void)
{
    SendMessageW(g_hwndA, WM_SETFONT, (WPARAM)g_hFont, TRUE);
    SendMessageW(g_hwndB, WM_SETFONT, (WPARAM)g_hFont, TRUE);
    SendMessageW(g_hwndT, WM_SETFONT, (WPARAM)g_hFont, TRUE);
    SendMessageW(g_hBtnOK, WM_SETFONT, (WPARAM)g_hFont, TRUE);
    SendMessageW(g_hBtnStop, WM_SETFONT, (WPARAM)g_hFont, TRUE);
    SendMessageW(g_hList, WM_SETFONT, (WPARAM)g_hFontMono, TRUE);
    SendMessageW(g_hLog, WM_SETFONT, (WPARAM)g_hFontMono, TRUE);
}

static void InitList(HWND hList)
{
    LVCOLUMNW col = {0};
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;

    col.pszText = L"时间"; col.cx = 110; ListView_InsertColumn(hList, 0, &col);
    col.pszText = L"A";    col.cx = 180; ListView_InsertColumn(hList, 1, &col);
    col.pszText = L"B";    col.cx = 180; ListView_InsertColumn(hList, 2, &col);
    col.pszText = L"备注";  col.cx = 120; ListView_InsertColumn(hList, 3, &col);

    ListView_SetExtendedListViewStyle(
        hList,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER
    );
}

static void Layout(HWND hwnd)
{
    RECT rc;
    GetClientRect(hwnd, &rc);

    const int m = 14;
    const int labelW = 64;
    const int editH = 26;
    const int rowH = 34;
    const int gap = 8;
    const int btnW = 84;
    const int btnH = 28;

    int x1 = m + labelW;
    int x2 = x1 + 220 + 10;
    int xBtn = x2;
    int wEdit = 220;

    int y1 = m;
    int y2 = y1 + rowH;
    int y3 = y2 + rowH;

    MoveWindow(GetDlgItem(hwnd, ID_LABEL_A), m, y1 + 4, labelW, 20, TRUE);
    MoveWindow(g_hwndA, x1, y1, wEdit, editH, TRUE);

    MoveWindow(GetDlgItem(hwnd, ID_LABEL_B), m, y2 + 4, labelW, 20, TRUE);
    MoveWindow(g_hwndB, x1, y2, wEdit, editH, TRUE);

    MoveWindow(GetDlgItem(hwnd, ID_LABEL_T), m, y3 + 4, labelW, 20, TRUE);
    MoveWindow(g_hwndT, x1, y3, 90, editH, TRUE);

    MoveWindow(g_hBtnOK,  xBtn,     y3 - 1, btnW, btnH, TRUE);
    MoveWindow(g_hBtnStop, xBtn+92,  y3 - 1, btnW, btnH, TRUE);

    int listTop = y3 + rowH + 6;
    int listH = (rc.bottom - listTop - m) * 2 / 3;
    int logTop = listTop + listH + gap;
    int logH = rc.bottom - logTop - m;

    if (listH < 120) listH = 120;
    if (logH < 70) logH = 70;

    MoveWindow(g_hList, m, listTop, rc.right - m * 2, listH, TRUE);
    MoveWindow(g_hLog,  m, logTop,  rc.right - m * 2, logH, TRUE);

    ListView_SetColumnWidth(g_hList, 0, LVSCW_AUTOSIZE_USEHEADER);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE:
    {
        CreateWindowW(L"STATIC", L"目标A",
            WS_CHILD | WS_VISIBLE,
            0, 0, 0, 0, hwnd, (HMENU)ID_LABEL_A, NULL, NULL);

        g_hwndA = CreateWindowW(L"EDIT", L"192.168.1.1",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            0, 0, 0, 0, hwnd, (HMENU)ID_EDIT_A, NULL, NULL);

        CreateWindowW(L"STATIC", L"目标B",
            WS_CHILD | WS_VISIBLE,
            0, 0, 0, 0, hwnd, (HMENU)ID_LABEL_B, NULL, NULL);

        g_hwndB = CreateWindowW(L"EDIT", L"z.cn",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            0, 0, 0, 0, hwnd, (HMENU)ID_EDIT_B, NULL, NULL);

        CreateWindowW(L"STATIC", L"持续秒",
            WS_CHILD | WS_VISIBLE,
            0, 0, 0, 0, hwnd, (HMENU)ID_LABEL_T, NULL, NULL);

        g_hwndT = CreateWindowW(L"EDIT", L"51",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            0, 0, 0, 0, hwnd, (HMENU)ID_EDIT_T, NULL, NULL);

        g_hBtnOK = CreateWindowW(L"BUTTON", L"开始",
            WS_CHILD | WS_VISIBLE,
            0, 0, 0, 0, hwnd, (HMENU)ID_BTN_OK, NULL, NULL);

        g_hBtnStop = CreateWindowW(L"BUTTON", L"停止",
            WS_CHILD | WS_VISIBLE,
            0, 0, 0, 0, hwnd, (HMENU)ID_BTN_STOP, NULL, NULL);

        g_hList = CreateWindowW(WC_LISTVIEWW, L"",
            WS_CHILD | WS_VISIBLE | WS_BORDER | LVS_REPORT | LVS_SINGLESEL,
            0, 0, 0, 0, hwnd, (HMENU)ID_LIST, NULL, NULL);

        g_hLog = CreateWindowW(L"EDIT", L"准备就绪。\r\n",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_MULTILINE |
            ES_AUTOVSCROLL | WS_VSCROLL | ES_READONLY | ES_NOHIDESEL,
            0, 0, 0, 0, hwnd, (HMENU)ID_LOG, NULL, NULL);

        g_hFont = MakeFont(20, L"Segoe UI");
        g_hFontMono = MakeFont(18, L"Cascadia Mono");
        ApplyFonts();
        InitList(g_hList);
        Layout(hwnd);
        return 0;
    }

    case WM_SIZE:
        Layout(hwnd);
        return 0;

    case WM_GETMINMAXINFO:
    {
        MINMAXINFO* mmi = (MINMAXINFO*)lParam;
        mmi->ptMinTrackSize.x = 620;
        mmi->ptMinTrackSize.y = 430;
        return 0;
    }

    case WM_COMMAND:
        return 0;

    case WM_DESTROY:
        if (g_hFont) DeleteObject(g_hFont);
        if (g_hFontMono) DeleteObject(g_hFontMono);
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrev, PWSTR lpCmdLine, int nCmdShow)
{
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LISTVIEW_CLASSES };
    InitCommonControlsEx(&icc);

    WNDCLASSW wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"LightGuiFrame";
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);

    if (!RegisterClassW(&wc))
        return 0;

    HWND hwnd = CreateWindowW(
        wc.lpszClassName,
        L"TCPing 双目标对比",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 860, 620,
        NULL, NULL, hInstance, NULL
    );

    if (!hwnd)
        return 0;

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}
