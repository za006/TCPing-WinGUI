// main.c
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <shellapi.h>
#include <stdio.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "shell32.lib")

#ifndef WM_APP_LOG
#define WM_APP_LOG   (WM_APP + 1)
#endif
#ifndef WM_APP_ROW
#define WM_APP_ROW   (WM_APP + 2)
#endif
#ifndef WM_APP_STATUS
#define WM_APP_STATUS (WM_APP + 3)
#endif
#ifndef WM_APP_DONE
#define WM_APP_DONE  (WM_APP + 4)
#endif

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
    int     pct;
    wchar_t state[64];
} TCPING_ROW;

HINSTANCE g_hInst = NULL;
HWND g_hwndMain = NULL;

static HANDLE g_hThread = NULL;
static volatile LONG g_running = 0;
static volatile LONG g_stop = 0;

static const wchar_t* g_targets[] = {
    L"127.0.0.1:80",
    L"127.0.0.1:8080",
    L"localhost:3000"
};
static const int g_targetCount = sizeof(g_targets) / sizeof(g_targets[0]);

static void PostLog(const wchar_t* text)
{
    if (g_hwndMain && text) PostMessageW(g_hwndMain, WM_APP_LOG, 0, (LPARAM)text);
}

static void PostStatus(const wchar_t* text)
{
    if (g_hwndMain && text) PostMessageW(g_hwndMain, WM_APP_STATUS, 0, (LPARAM)text);
}

static DWORD TcpConnectOnce(const wchar_t* host, unsigned short port)
{
    struct addrinfoW hints, *res = NULL, *p = NULL;
    SOCKET s = INVALID_SOCKET;
    DWORD best = (DWORD)-1;
    LARGE_INTEGER f, t1, t2;
    int ok = 0;

    ZeroMemory(&hints, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    wchar_t portbuf[16];
    swprintf(portbuf, 16, L"%u", port);

    if (QueryPerformanceFrequency(&f) == 0) return (DWORD)-1;

    if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) return (DWORD)-1;

    if (GetAddrInfoW(host, portbuf, &hints, &res) != 0) return (DWORD)-1;

    for (p = res; p != NULL; p = p->ai_next) {
        s = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (s == INVALID_SOCKET) continue;

        u_long nonblocking = 1;
        ioctlsocket(s, FIONBIO, &nonblocking);

        QueryPerformanceCounter(&t1);
        int r = connect(s, p->ai_addr, (int)p->ai_addrlen);
        if (r == 0) {
            QueryPerformanceCounter(&t2);
            ok = 1;
        } else {
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK || err == WSAEINPROGRESS || err == WSAEINVAL) {
                fd_set wfds;
                struct timeval tv;
                FD_ZERO(&wfds);
                FD_SET(s, &wfds);
                tv.tv_sec = 1;
                tv.tv_usec = 0;

                r = select(0, NULL, &wfds, NULL, &tv);
                if (r > 0) {
                    int soerr = 0;
                    int slen = sizeof(soerr);
                    getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&soerr, &slen);
                    if (soerr == 0) {
                        QueryPerformanceCounter(&t2);
                        ok = 1;
                    }
                }
            }
        }

        closesocket(s);
        s = INVALID_SOCKET;

        if (ok) {
            double ms = (double)(t2.QuadPart - t1.QuadPart) * 1000.0 / (double)f.QuadPart;
            best = (DWORD)(ms + 0.5);
            break;
        }
    }

    if (res) FreeAddrInfoW(res);
    return best;
}

static void SendRow(HWND hwnd, TCPING_ROW* row)
{
    if (!hwnd || !row) return;
    PostMessageW(hwnd, WM_APP_ROW, 0, (LPARAM)row);
}

static DWORD WINAPI TcpingThread(LPVOID lp)
{
    HWND hwnd = (HWND)lp;
    WSADATA wsa;
    const int durationSec = 50;
    const int intervalMs = 1000;
    TCPING_ROW rows[sizeof(g_targets) / sizeof(g_targets[0])];
    int i;

    InterlockedExchange(&g_running, 1);
    InterlockedExchange(&g_stop, 0);

    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) {
        PostStatus(L"WSAStartup failed");
        InterlockedExchange(&g_running, 0);
        PostMessageW(hwnd, WM_APP_DONE, 0, 0);
        return 0;
    }

    ZeroMemory(rows, sizeof(rows));
    for (i = 0; i < g_targetCount; ++i) {
        rows[i].id = i;
        lstrcpynW(rows[i].target, g_targets[i], 255);
        rows[i].min_ms = 0x7fffffff;
        rows[i].state[0] = 0;
    }

    PostStatus(L"运行中...");
    PostLog(L"开始 TCPing");

    for (int sec = 0; sec < durationSec && !g_stop; ++sec) {
        for (i = 0; i < g_targetCount && !g_stop; ++i) {
            wchar_t host[256];
            unsigned short port = 0;
            DWORD ms = (DWORD)-1;
            wchar_t* colon;

            lstrcpynW(host, rows[i].target, 255);
            colon = wcsrchr(host, L':');
            if (colon) {
                *colon = 0;
                port = (unsigned short)_wtoi(colon + 1);
            } else {
                port = 80;
            }

            rows[i].sent++;

            ms = TcpConnectOnce(host, port);
            if (ms != (DWORD)-1) {
                rows[i].received++;
                rows[i].last_ms = (int)ms;
                if (rows[i].last_ms < rows[i].min_ms) rows[i].min_ms = rows[i].last_ms;
                if (rows[i].last_ms > rows[i].max_ms) rows[i].max_ms = rows[i].last_ms;
                rows[i].avg_ms = (rows[i].avg_ms * (rows[i].received - 1) + rows[i].last_ms) / rows[i].received;
                rows[i].lost = (rows[i].sent - rows[i].received) * 100 / rows[i].sent;
                swprintf(rows[i].state, 64, L"OK");
                rows[i].pct = rows[i].received * 100 / rows[i].sent;
            } else {
                rows[i].lost = (rows[i].sent - rows[i].received) * 100 / rows[i].sent;
                rows[i].pct = rows[i].received * 100 / rows[i].sent;
                lstrcpynW(rows[i].state, L"TIMEOUT", 63);
            }

            SendRow(hwnd, &rows[i]);
        }

        Sleep(intervalMs);
    }

    for (i = 0; i < g_targetCount; ++i) {
        SendRow(hwnd, &rows[i]);
    }

    PostLog(L"测试结束");
    PostStatus(L"完成");
    WSACleanup();

    InterlockedExchange(&g_running, 0);
    PostMessageW(hwnd, WM_APP_DONE, 0, 0);
    return 0;
}

void StartTcping(HWND hwnd)
{
    if (InterlockedCompareExchange(&g_running, 1, 0) != 0) return;

    g_hwndMain = hwnd;
    if (g_hThread) {
        CloseHandle(g_hThread);
        g_hThread = NULL;
    }
    g_hThread = CreateThread(NULL, 0, TcpingThread, hwnd, 0, NULL);
    if (!g_hThread) {
        InterlockedExchange(&g_running, 0);
        PostStatus(L"线程创建失败");
    } else {
        CloseHandle(g_hThread);
        g_hThread = NULL;
    }
}

void StopTcping(void)
{
    InterlockedExchange(&g_stop, 1);
    if (g_hThread) {
        WaitForSingleObject(g_hThread, 200);
        CloseHandle(g_hThread);
        g_hThread = NULL;
    }
    InterlockedExchange(&g_running, 0);
}

void OpenWebInterface(void)
{
    ShellExecuteW(NULL, L"open", L"http://127.0.0.1:8080/", NULL, NULL, SW_SHOWNORMAL);
}
