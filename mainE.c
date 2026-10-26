#define _CRT_SECURE_NO_WARNINGS

#include <windows.h>
#include <commctrl.h>
#include <stdio.h>

#pragma comment(lib, "comctl32.lib")

#define APP_TITLE           "TCPing GUI"

#define WM_APP_LOG          (WM_APP + 1)
#define WM_APP_ROW          (WM_APP + 2)
#define WM_APP_DONE         (WM_APP + 3)
#define WM_APP_STATUS       (WM_APP + 4)

#define IDC_EDIT_A          1001
#define IDC_EDIT_PORTA      1002
#define IDC_EDIT_B          1003
#define IDC_EDIT_PORTB      1004
#define IDC_EDIT_DURATION   1005
#define IDC_BTN_START       1006
#define IDC_BTN_STOP        1007
#define IDC_BTN_WEB         1008
#define IDC_LIST_RESULTS    1009
#define IDC_STATUSBAR       1010

typedef struct {
    char hostA[256];
    int portA;
    char hostB[256];
    int portB;
    int durationSec;
    volatile LONG stopFlag;
    HANDLE hThread;
} APPSTATE;

typedef struct {
    char timeText[32];
    char aText[32];
    char bText[32];
    char diffText[32];
    int aMs;
    int bMs;
    int diffMs;
} RESULTROW;

HINSTANCE g_hInst = NULL;
HWND g_hWnd = NULL;
HWND g_hList = NULL;
HWND g_hStatus = NULL;
HWND g_hEditA = NULL, g_hEditPortA = NULL, g_hEditB = NULL, g_hEditPortB = NULL, g_hEditDuration = NULL;
HWND g_hBtnStart = NULL, g_hBtnStop = NULL, g_hBtnWeb = NULL;

extern void StartWork(void);
extern void StopWork(void);

static void SetStatusText(const char* text) {
    if (g_hStatus) {
        SendMessageA(g_hStatus, SB_SETTEXTA, 0, (LPARAM)(text ? text : ""));
    }
}

static void InitListView(HWND hList) {
    ListView_SetExtendedListViewStyle(hList,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);

    LVCOLUMNA col;
    ZeroMemory(&col, sizeof(col));
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;

    col.pszText = "Time";
    col.cx = 110;
    col.iSubItem = 0;
    ListView_InsertColumn(hList, 0, &col);

    col.pszText = "A ms";
    col.cx = 90;
    col.iSubItem = 1;
    ListView_InsertColumn(hList, 1, &col);

    col.pszText = "B ms";
    col.cx = 90;
    col.iSubItem = 2;
    ListView_InsertColumn(hList, 2, &col);

    col.pszText = "Diff";
    col.cx = 90;
    col.iSubItem = 3;
    ListView_InsertColumn(hList, 3, &col);
}

static void ResizeControls(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);

    int pad = 10;
    int top = 10;
    int rowH = 24;
    int statusH = 22;
    int listTop = 80;

    MoveWindow(g_hEditA,        70,  top, 140, rowH, TRUE);
    MoveWindow(g_hEditPortA,    270, top, 60,  rowH, TRUE);
    MoveWindow(g_hEditB,        410, top, 140, rowH, TRUE);
    MoveWindow(g_hEditPortB,    610, top, 60,  rowH, TRUE);
    MoveWindow(g_hEditDuration, 750, top, 50,  rowH, TRUE);

    MoveWindow(g_hBtnStart, 10, 40, 90, 28, TRUE);
    MoveWindow(g_hBtnStop,  110, 40, 90, 28, TRUE);
    MoveWindow(g_hBtnWeb,   210, 40, 110, 28, TRUE);

    MoveWindow(g_hList,
        pad,
        listTop,
        rc.right - pad * 2,
        rc.bottom - listTop - statusH - pad,
        TRUE);

    SendMessage(g_hStatus, WM_SIZE, 0, 0);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        INITCOMMONCONTROLSEX icex;
        icex.dwSize = sizeof(icex);
        icex.dwICC = ICC_WIN95_CLASSES | ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES;
        InitCommonControlsEx(&icex);

        CreateWindowA("static", "Target A:", WS_CHILD | WS_VISIBLE,
            10, 12, 60, 20, hwnd, NULL, g_hInst, NULL);

        g_hEditA = CreateWindowA("edit", "192.168.1.1",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            70, 10, 140, 22, hwnd, (HMENU)IDC_EDIT_A, g_hInst, NULL);

        CreateWindowA("static", "Port A:", WS_CHILD | WS_VISIBLE,
            220, 12, 50, 20, hwnd, NULL, g_hInst, NULL);

        g_hEditPortA = CreateWindowA("edit", "80",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            270, 10, 60, 22, hwnd, (HMENU)IDC_EDIT_PORTA, g_hInst, NULL);

        CreateWindowA("static", "Target B:", WS_CHILD | WS_VISIBLE,
            350, 12, 60, 20, hwnd, NULL, g_hInst, NULL);

        g_hEditB = CreateWindowA("edit", "192.168.1.7",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            410, 10, 140, 22, hwnd, (HMENU)IDC_EDIT_B, g_hInst, NULL);

        CreateWindowA("static", "Port B:", WS_CHILD | WS_VISIBLE,
            560, 12, 50, 20, hwnd, NULL, g_hInst, NULL);

        g_hEditPortB = CreateWindowA("edit", "53",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            610, 10, 60, 22, hwnd, (HMENU)IDC_EDIT_PORTB, g_hInst, NULL);

        CreateWindowA("static", "Duration:", WS_CHILD | WS_VISIBLE,
            690, 12, 60, 20, hwnd, NULL, g_hInst, NULL);

        g_hEditDuration = CreateWindowA("edit", "50",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            750, 10, 50, 22, hwnd, (HMENU)IDC_EDIT_DURATION, g_hInst, NULL);

        g_hBtnStart = CreateWindowA("button", "Start",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            10, 40, 90, 28, hwnd, (HMENU)IDC_BTN_START, g_hInst, NULL);

        g_hBtnStop = CreateWindowA("button", "Stop",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            110, 40, 90, 28, hwnd, (HMENU)IDC_BTN_STOP, g_hInst, NULL);

        g_hBtnWeb = CreateWindowA("button", "开启Web端",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            210, 40, 110, 28, hwnd, (HMENU)IDC_BTN_WEB, g_hInst, NULL);

        g_hList = CreateWindowExA(WS_EX_CLIENTEDGE, WC_LISTVIEWA, "",
            WS_CHILD | WS_VISIBLE | LVS_REPORT,
            10, 80, 760, 300, hwnd, (HMENU)IDC_LIST_RESULTS, g_hInst, NULL);

        g_hStatus = CreateWindowExA(0, STATUSCLASSNAMEA, NULL,
            WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
            0, 0, 0, 0, hwnd, (HMENU)IDC_STATUSBAR, g_hInst, NULL);

        InitListView(g_hList);
        SetStatusText("Ready");
        return 0;
    }

    case WM_SIZE:
        ResizeControls(hwnd);
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_BTN_START:
            StartWork();
            return 0;
        case IDC_BTN_STOP:
            StopWork();
            return 0;
        case IDC_BTN_WEB:
            MessageBoxA(hwnd, "Web端按钮已保留，但当前不自动打开。", "Info", MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        break;

    case WM_APP_LOG:
        return 0;

    case WM_APP_ROW:
        return 0;

    case WM_APP_STATUS: {
        char* s = (char*)lParam;
        if (s) {
            SetStatusText(s);
            free(s);
        }
        return 0;
    }

    case WM_APP_DONE:
        SetStatusText("Done");
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmdLine, int nShowCmd) {
    (void)hPrev;
    (void)lpCmdLine;

    g_hInst = hInst;

    WNDCLASSA wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "TCPingGUIClass";

    if (!RegisterClassA(&wc)) {
        MessageBoxA(NULL, "RegisterClass failed", APP_TITLE, MB_OK | MB_ICONERROR);
        return 0;
    }

    g_hWnd = CreateWindowA(
        wc.lpszClassName,
        APP_TITLE,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 900, 560,
        NULL, NULL, hInst, NULL);

    if (!g_hWnd) {
        MessageBoxA(NULL, "CreateWindow failed", APP_TITLE, MB_OK | MB_ICONERROR);
        return 0;
    }

    ShowWindow(g_hWnd, nShowCmd);
    UpdateWindow(g_hWnd);

    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    return (int)msg.wParam;
}
