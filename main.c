#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <commctrl.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "ws2_32.lib")

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

static HINSTANCE g_hInst;
static HWND g_hWnd;
static HWND g_hList;
static HWND g_hStatus;
static HWND g_hEditA, g_hEditPortA, g_hEditB, g_hEditPortB, g_hEditDuration;
static HWND g_hBtnStart, g_hBtnStop, g_hBtnWeb;
static APPSTATE g_state = {0};

static void SetStatusText(const char* text);
static void AddLogLine(const char* text);
static void AddResultRow(const RESULTROW* r);
static DWORD WINAPI WorkerThreadProc(LPVOID lpParam);
static int TcpPingOnce(const char* host, int port, int timeoutMs);
static void UpdateButtons(BOOL running);
static void StartWork(void);
static void StopWork(void);
static void InitListView(HWND hList);
static void ResizeControls(HWND hwnd);

static const char* MsToText(int ms, char* buf, size_t cb) {
    if (ms < 0) {
        snprintf(buf, cb, "timeout");
    } else {
        snprintf(buf, cb, "%d", ms);
    }
    return buf;
}

static void GetNowText(char* buf, size_t cb) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(buf, cb, "%02d:%02d:%02d",
             st.wHour, st.wMinute, st.wSecond);
}

static void SafeSetText(HWND h, const char* text) {
    SetWindowTextA(h, text ? text : "");
}

static void SetStatusText(const char* text) {
    if (g_hStatus) {
        SendMessageA(g_hStatus, SB_SETTEXTA, 0, (LPARAM)(text ? text : ""));
    }
}

static void AddLogLine(const char* text) {
    if (!g_hList) return;
    int idx = ListView_GetItemCount(g_hList);
    char buf[64];
    GetNowText(buf, sizeof(buf));

    LVITEMA item;
    ZeroMemory(&item, sizeof(item));
    item.mask = LVIF_TEXT;
    item.iItem = idx;
    item.iSubItem = 0;
    item.pszText = (LPSTR)buf;
    ListView_InsertItemA(g_hList, &item);

    ListView_SetItemTextA(g_hList, idx, 1, (LPSTR)text);
}

static void AddResultRow(const RESULTROW* r) {
    if (!g_hList || !r) return;
    int idx = ListView_GetItemCount(g_hList);

    LVITEMA item;
    ZeroMemory(&item, sizeof(item));
    item.mask = LVIF_TEXT;
    item.iItem = idx;
    item.iSubItem = 0;
    item.pszText = (LPSTR)r->timeText;
    ListView_InsertItemA(g_hList, &item);

    ListView_SetItemTextA(g_hList, idx, 1, (LPSTR)r->aText);
    ListView_SetItemTextA(g_hList, idx, 2, (LPSTR)r->bText);
    ListView_SetItemTextA(g_hList, idx, 3, (LPSTR)r->diffText);
    ListView_EnsureVisible(g_hList, idx, FALSE);
}

static int TcpPingOnce(const char* host, int port, int timeoutMs) {
    struct addrinfo hints, *res = NULL, *p = NULL;
    char portStr[16];
    SOCKET s = INVALID_SOCKET;
    int ret = -1;
    LARGE_INTEGER f, t1, t2;

    ZeroMemory(&hints, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    snprintf(portStr, sizeof(portStr), "%d", port);
    if (getaddrinfo(host, portStr, &hints, &res) != 0) return -1;

    for (p = res; p; p = p->ai_next) {
        s = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (s == INVALID_SOCKET) continue;

        u_long nb = 1;
        ioctlsocket(s, FIONBIO, &nb);

        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&t1);

        int c = connect(s, p->ai_addr, (int)p->ai_addrlen);
        if (c == 0) {
            QueryPerformanceCounter(&t2);
            closesocket(s);
            freeaddrinfo(res);
            return (int)((t2.QuadPart - t1.QuadPart) * 1000 / freq.QuadPart);
        }

        int wsaErr = WSAGetLastError();
        if (wsaErr != WSAEWOULDBLOCK && wsaErr != WSAEINPROGRESS) {
            closesocket(s);
            s = INVALID_SOCKET;
            continue;
        }

        fd_set wfds, efds;
        FD_ZERO(&wfds);
        FD_ZERO(&efds);
        FD_SET(s, &wfds);
        FD_SET(s, &efds);

        struct timeval tv;
        tv.tv_sec = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;

        int sel = select(0, NULL, &wfds, &efds, &tv);
        if (sel > 0 && FD_ISSET(s, &wfds)) {
            int so_error = 0;
            int len = sizeof(so_error);
            getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&so_error, &len);
            if (so_error == 0) {
                QueryPerformanceCounter(&t2);
                closesocket(s);
                freeaddrinfo(res);
                return (int)((t2.QuadPart - t1.QuadPart) * 1000 / freq.QuadPart);
            }
        }

        closesocket(s);
        s = INVALID_SOCKET;
    }

    if (res) freeaddrinfo(res);
    return ret;
}

static DWORD WINAPI WorkerThreadProc(LPVOID lpParam) {
    APPSTATE* st = (APPSTATE*)lpParam;
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        PostMessageA(g_hWnd, WM_APP_LOG, 0, (LPARAM)"WSAStartup failed");
        PostMessageA(g_hWnd, WM_APP_DONE, 0, 0);
        return 0;
    }

    char msg[256];
    snprintf(msg, sizeof(msg), "Start: %s:%d vs %s:%d, duration=%ds",
             st->hostA, st->portA, st->hostB, st->portB, st->durationSec);
    PostMessageA(g_hWnd, WM_APP_LOG, 0, (LPARAM)_strdup(msg));

    int elapsed = 0;
    while (elapsed < st->durationSec) {
        if (InterlockedCompareExchange(&st->stopFlag, 0, 0) != 0) break;

        int aMs = TcpPingOnce(st->hostA, st->portA, 2000);
        int bMs = TcpPingOnce(st->hostB, st->portB, 2000);
        char timeText[32], aText[32], bText[32], diffText[32];

        GetNowText(timeText, sizeof(timeText));
        MsToText(aMs, aText, sizeof(aText));
        MsToText(bMs, bText, sizeof(bText));

        int diff = -1;
        if (aMs >= 0 && bMs >= 0) diff = abs(aMs - bMs);
        MsToText(diff, diffText, sizeof(diffText));

        RESULTROW* r = (RESULTROW*)malloc(sizeof(RESULTROW));
        if (r) {
            ZeroMemory(r, sizeof(*r));
            strncpy(r->timeText, timeText, sizeof(r->timeText) - 1);
            strncpy(r->aText, aText, sizeof(r->aText) - 1);
            strncpy(r->bText, bText, sizeof(r->bText) - 1);
            strncpy(r->diffText, diffText, sizeof(r->diffText) - 1);
            r->aMs = aMs;
            r->bMs = bMs;
            r->diffMs = diff;
            PostMessageA(g_hWnd, WM_APP_ROW, 0, (LPARAM)r);
        }

        char status[256];
        snprintf(status, sizeof(status), "Running... %d/%d sec", elapsed + 1, st->durationSec);
        PostMessageA(g_hWnd, WM_APP_STATUS, 0, (LPARAM)_strdup(status));

        Sleep(1000);
        elapsed++;
    }

    PostMessageA(g_hWnd, WM_APP_DONE, 0, 0);
    WSACleanup();
    return 0;
}

static void UpdateButtons(BOOL running) {
    EnableWindow(g_hBtnStart, !running);
    EnableWindow(g_hBtnStop, running);
    EnableWindow(g_hEditA, !running);
    EnableWindow(g_hEditPortA, !running);
    EnableWindow(g_hEditB, !running);
    EnableWindow(g_hEditPortB, !running);
    EnableWindow(g_hEditDuration, !running);
}

static void StartWork(void) {
    char buf[256];

    GetWindowTextA(g_hEditA, g_state.hostA, sizeof(g_state.hostA));
    GetWindowTextA(g_hEditB, g_state.hostB, sizeof(g_state.hostB));
    GetWindowTextA(g_hEditPortA, buf, sizeof(buf));
    g_state.portA = atoi(buf);
    GetWindowTextA(g_hEditPortB, buf, sizeof(buf));
    g_state.portB = atoi(buf);
    GetWindowTextA(g_hEditDuration, buf, sizeof(buf));
    g_state.durationSec = atoi(buf);

    if (g_state.durationSec <= 0) g_state.durationSec = 50;
    if (g_state.portA <= 0) g_state.portA = 80;
    if (g_state.portB <= 0) g_state.portB = 53;

    InterlockedExchange(&g_state.stopFlag, 0);

    SetStatusText("Starting...");
    UpdateButtons(TRUE);

    g_state.hThread = CreateThread(NULL, 0, WorkerThreadProc, &g_state, 0, NULL);
    if (!g_state.hThread) {
        SetStatusText("Failed to start worker");
        UpdateButtons(FALSE);
    } else {
        CloseHandle(g_state.hThread);
        g_state.hThread = NULL;
    }
}

static void StopWork(void) {
    InterlockedExchange(&g_state.stopFlag, 1);
    SetStatusText("Stopping...");
}

static void InitListView(HWND hList) {
    ListView_SetExtendedListViewStyle(hList,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);

    LVCOLUMNA col;
    ZeroMemory(&col, sizeof(col));
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;

    col.pszText = "Time";
    col.cx = 90;
    ListView_InsertColumnA(hList, 0, &col);

    col.pszText = "A ms";
    col.cx = 90;
    ListView_InsertColumnA(hList, 1, &col);

    col.pszText = "B ms";
    col.cx = 90;
    ListView_InsertColumnA(hList, 2, &col);

    col.pszText = "Diff";
    col.cx = 90;
    ListView_InsertColumnA(hList, 3, &col);
}

static void ResizeControls(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);

    int pad = 10;
    int top = 10;
    int rowH = 24;
    int labelW = 60;
    int editW = 90;
    int btnW = 90;

    MoveWindow(GetDlgItem(hwnd, IDC_EDIT_A), pad + labelW, top, 140, rowH, TRUE);
    MoveWindow(GetDlgItem(hwnd, IDC_EDIT_PORTA), pad + labelW + 145, top, editW, rowH, TRUE);
    MoveWindow(GetDlgItem(hwnd, IDC_EDIT_B), pad + labelW + 255, top, 140, rowH, TRUE);
    MoveWindow(GetDlgItem(hwnd, IDC_EDIT_PORTB), pad + labelW + 400, top, editW, rowH, TRUE);
    MoveWindow(GetDlgItem(hwnd, IDC_EDIT_DURATION), pad + labelW + 530, top, 70, rowH, TRUE);

    MoveWindow(g_hBtnStart, pad, top + 30, btnW, 28, TRUE);
    MoveWindow(g_hBtnStop, pad + 100, top + 30, btnW, 28, TRUE);
    MoveWindow(g_hBtnWeb, pad + 200, top + 30, 110, 28, TRUE);

    int listTop = 70;
    int statusH = 22;
    MoveWindow(g_hList, pad, listTop, rc.right - pad * 2, rc.bottom - listTop - statusH - pad, TRUE);

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
        UpdateButtons(FALSE);
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
            MessageBoxA(hwnd, "Web端按钮已保留，但当前未自动打开。", "Info", MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        break;

    case WM_APP_LOG: {
        char* s = (char*)lParam;
        if (s) {
            AddLogLine(s);
            free(s);
        }
        return 0;
    }
    case WM_APP_ROW: {
        RESULTROW* r = (RESULTROW*)lParam;
        if (r) {
            AddResultRow(r);
            free(r);
        }
        return 0;
    }
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
        UpdateButtons(FALSE);
        return 0;

    case WM_DESTROY:
        InterlockedExchange(&g_state.stopFlag, 1);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmdLine, int nShowCmd) {
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
        wc.lpszClassName, APP_TITLE,
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
