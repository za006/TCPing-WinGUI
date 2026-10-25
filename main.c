#define UNICODE
#define _UNICODE

#include <windows.h>
#include <commctrl.h>
#include <wchar.h>

#pragma comment(lib, "comctl32.lib")

#define IDC_EDIT1   1001
#define IDC_EDIT2   1002
#define IDC_EDIT3   1003
#define IDC_BTN1    1004
#define IDC_BTN2    1005
#define IDC_LIST    1006
#define IDC_LOG     1007

static HWND g_hEdit1, g_hEdit2, g_hEdit3;
static HWND g_hBtn1, g_hBtn2, g_hList, g_hLog;
static HFONT g_hFontUI = NULL;
static HFONT g_hFontMono = NULL;

static HFONT MakeFont(int height, const wchar_t* face)
{
    return CreateFontW(
        height, 0, 0, 0, FW_NORMAL,
        FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face
    );
}

static void InitListView(HWND hList)
{
    LVCOLUMNW col = {0};
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;

    col.pszText = L"时间";
    col.cx = 120;
    ListView_InsertColumn(hList, 0, &col);

    col.pszText = L"目标A";
    col.cx = 180;
    ListView_InsertColumn(hList, 1, &col);

    col.pszText = L"目标B";
    col.cx = 180;
    ListView_InsertColumn(hList, 2, &col);

    col.pszText = L"备注";
    col.cx = 120;
    ListView_InsertColumn(hList, 3, &col);

    ListView_SetExtendedListViewStyle(
        hList,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER
    );
}

static void ApplyFonts(void)
{
    SendMessageW(g_hEdit1, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
    SendMessageW(g_hEdit2, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
    SendMessageW(g_hEdit3, WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
    SendMessageW(g_hBtn1,  WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
    SendMessageW(g_hBtn2,  WM_SETFONT, (WPARAM)g_hFontUI, TRUE);
    SendMessageW(g_hList,  WM_SETFONT, (WPARAM)g_hFontMono, TRUE);
    SendMessageW(g_hLog,   WM_SETFONT, (WPARAM)g_hFontMono, TRUE);
}

static void LayoutControls(HWND hwnd)
{
    RECT rc;
    GetClientRect(hwnd, &rc);

    int margin = 16;
    int top = 16;
    int rowH = 30;
    int gap = 10;

    int labelW = 70;
    int editW = 220;
    int editH = 26;
    int btnW = 100;
    int btnH = 30;

    MoveWindow(GetDlgItem(hwnd, IDC_STATIC + 1), margin, top + 3, labelW, 20, TRUE);
    MoveWindow(g_hEdit1, margin + labelW, top, editW, editH, TRUE);
    MoveWindow(GetDlgItem(hwnd, IDC_STATIC + 2), margin, top + rowH + 3, labelW, 20, TRUE);
    MoveWindow(g_hEdit2, margin + labelW, top + rowH, editW, editH, TRUE);
    MoveWindow(GetDlgItem(hwnd, IDC_STATIC + 3), margin, top + rowH * 2 + 3, labelW, 20, TRUE);
    MoveWindow(g_hEdit3, margin + labelW, top + rowH * 2, 100, editH, TRUE);

    MoveWindow(g_hBtn1, margin + labelW + 120, top + rowH * 2, btnW, btnH, TRUE);
    MoveWindow(g_hBtn2, margin + labelW + 230, top + rowH * 2, btnW, btnH, TRUE);

    int listTop = top + rowH * 3 + 18;
    int listH = (rc.bottom - listTop - margin) * 2 / 3;
    int logTop = listTop + listH + gap;
    int logH = rc.bottom - logTop - margin;

    if (listH < 120) listH = 120;
    if (logH < 80) logH = 80;

    MoveWindow(g_hList, margin, listTop, rc.right - margin * 2, listH, TRUE);
    MoveWindow(g_hLog, margin, logTop, rc.right - margin * 2, logH, TRUE);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE:
    {
        CreateWindowW(L"STATIC", L"目标A", WS_CHILD | WS_VISIBLE,
            16, 19, 70, 20, hwnd, NULL, NULL, NULL);

        g_hEdit1 = CreateWindowW(L"EDIT", L"192.168.1.1",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            86, 16, 220, 26, hwnd, (HMENU)IDC_EDIT1, NULL, NULL);

        CreateWindowW(L"STATIC", L"目标B", WS_CHILD | WS_VISIBLE,
            16, 49, 70, 20, hwnd, NULL, NULL, NULL);

        g_hEdit2 = CreateWindowW(L"EDIT", L"example.com",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            86, 46, 220, 26, hwnd, (HMENU)IDC_EDIT2, NULL, NULL);

        CreateWindowW(L"STATIC", L"秒数", WS_CHILD | WS_VISIBLE,
            16, 79, 70, 20, hwnd, NULL, NULL, NULL);

        g_hEdit3 = CreateWindowW(L"EDIT", L"51",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            86, 76, 100, 26, hwnd, (HMENU)IDC_EDIT3, NULL, NULL);

        g_hBtn1 = CreateWindowW(L"BUTTON", L"开始",
            WS_CHILD | WS_VISIBLE,
            206, 76, 100, 30, hwnd, (HMENU)IDC_BTN1, NULL, NULL);

        g_hBtn2 = CreateWindowW(L"BUTTON", L"停止",
            WS_CHILD | WS_VISIBLE,
            316, 76, 100, 30, hwnd, (HMENU)IDC_BTN2, NULL, NULL);

        g_hList = CreateWindowW(WC_LISTVIEWW, L"",
            WS_CHILD | WS_VISIBLE | WS_BORDER | LVS_REPORT | LVS_SINGLESEL,
            16, 120, 800, 280, hwnd, (HMENU)IDC_LIST, NULL, NULL);

        g_hLog = CreateWindowW(L"EDIT", L"准备就绪。\r\n",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_MULTILINE |
            ES_AUTOVSCROLL | WS_VSCROLL | ES_READONLY | ES_NOHIDESEL,
            16, 410, 800, 120, hwnd, (HMENU)IDC_LOG, NULL, NULL);

        g_hFontUI = MakeFont(20, L"Segoe UI");
        g_hFontMono = MakeFont(18, L"Cascadia Mono");
        ApplyFonts();
        InitListView(g_hList);
        LayoutControls(hwnd);
        return 0;
    }

    case WM_SIZE:
        LayoutControls(hwnd);
        return 0;

    case WM_COMMAND:
        return 0;

    case WM_DESTROY:
        if (g_hFontUI) DeleteObject(g_hFontUI);
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
    wc.lpszClassName = L"SimpleGuiFrame";
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);

    if (!RegisterClassW(&wc))
        return 0;

    HWND hwnd = CreateWindowW(
        wc.lpszClassName,
        L"GUI 框架测试",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 900, 650,
        NULL, NULL, hInstance, NULL);

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
