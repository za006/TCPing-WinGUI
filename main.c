#define _CRT_SECURE_NO_WARNINGS

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

extern HINSTANCE g_hInst;
extern HWND g_hWnd;
extern HWND g_hList;
extern HWND g_hStatus;
extern HWND g_hEditA;
extern HWND g_hEditPortA;
extern HWND g_hEditB;
extern HWND g_hEditPortB;
extern HWND g_hEditDuration;
extern HWND g_hBtnStart;
extern HWND g_hBtnStop;
extern HWND g_hBtnWeb;

#define WM_APP_LOG          (WM_APP + 1)
#define WM_APP_ROW          (WM_APP + 2)
#define WM_APP_DONE         (WM_APP + 3)
#define WM_APP_STATUS       (WM_APP + 4)

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

static APPSTATE g_state = {0};

static void GetNowText(char* buf, size_t cb) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(buf, cb, "%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);
}

static void MsToText(int ms, char* buf, size_t cb) {
    if (ms < 0) {
        snprintf(buf, cb, "timeout");
    } else {
        snprintf(buf, cb, "%d", ms);
    }
}

static void PostLog(const char* s) {
    if (!s) return;
    char* dup = _strdup(s);
    if (dup) PostMessageA(g_hWnd, WM_APP_LOG, 0, (LPARAM)dup);
}

static void PostStatus(const char* s) {
    if (!s) return;
    char* dup = _strdup(s);
    if (dup) PostMessageA(g_hWnd, WM_APP_STATUS, 0, (LPARAM)dup);
}

static int TcpPingOnce(const char* host, int port, int timeoutMs) {
    struct addrinfo hints, *res = NULL, *p = NULL;
    char portStr[16];

    ZeroMemory(&hints, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    snprintf(portStr, sizeof(portStr), "%d", port);

    if (getaddrinfo(host, portStr, &hints, &res) != 0) {
        return -1;
    }

    for (p = res; p; p = p->ai_next) {
        SOCKET s = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (s == INVALID_SOCKET) continue;

        u_long nb = 1;
        ioctlsocket(s, FIONBIO, &nb);

        LARGE_INTEGER freq, t1, t2;
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
    }

    if (res) freeaddrinfo(res);
    return -1;
}

static DWORD WINAPI WorkerThreadProc(LPVOID lpParam) {
    APPSTATE* st = (APPSTATE*)lpParam;
    WSADATA wsa;

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        PostLog("WSAStartup failed");
        PostMessageA(g_hWnd, WM_APP_DONE, 0, 0);
        return 0;
    }

    {
        char buf[256];
        snprintf(buf, sizeof(buf), "Start: %s:%d vs %s:%d, duration=%ds",
                 st->hostA, st->portA, st->hostB, st->portB, st->durationSec);
        PostLog(buf);
    }

    int elapsed = 0;
    while (elapsed < st->durationSec) {
        if (InterlockedCompareExchange(&st->stopFlag, 0, 0) != 0) break;

        int aMs = TcpPingOnce(st->hostA, st->portA, 2000);
        int bMs = TcpPingOnce(st->hostB, st->portB, 2000);

        RESULTROW* r = (RESULTROW*)malloc(sizeof(RESULTROW));
        if (r) {
            ZeroMemory(r, sizeof(RESULTROW));
            GetNowText(r->timeText, sizeof(r->timeText));
            MsToText(aMs, r->aText, sizeof(r->aText));
            MsToText(bMs, r->bText, sizeof(r->bText));

            if (aMs >= 0 && bMs >= 0) r->diffMs = abs(aMs - bMs);
            else r->diffMs = -1;

            MsToText(r->diffMs, r->diffText, sizeof(r->diffText));
            r->aMs = aMs;
            r->bMs = bMs;

            PostMessageA(g_hWnd, WM_APP_ROW, 0, (LPARAM)r);
        }

        {
            char buf[128];
            snprintf(buf, sizeof(buf), "Running... %d/%d sec", elapsed + 1, st->durationSec);
            PostStatus(buf);
        }

        Sleep(1000);
        elapsed++;
    }

    WSACleanup();
    PostMessageA(g_hWnd, WM_APP_DONE, 0, 0);
    return 0;
}

void StartWork(void) {
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

    EnableWindow(g_hBtnStart, FALSE);
    EnableWindow(g_hBtnStop, TRUE);
    EnableWindow(g_hEditA, FALSE);
    EnableWindow(g_hEditPortA, FALSE);
    EnableWindow(g_hEditB, FALSE);
    EnableWindow(g_hEditPortB, FALSE);
    EnableWindow(g_hEditDuration, FALSE);

    PostStatus("Starting...");

    g_state.hThread = CreateThread(NULL, 0, WorkerThreadProc, &g_state, 0, NULL);
    if (!g_state.hThread) {
        PostStatus("Failed to start worker");
        EnableWindow(g_hBtnStart, TRUE);
        EnableWindow(g_hBtnStop, FALSE);
        EnableWindow(g_hEditA, TRUE);
        EnableWindow(g_hEditPortA, TRUE);
        EnableWindow(g_hEditB, TRUE);
        EnableWindow(g_hEditPortB, TRUE);
        EnableWindow(g_hEditDuration, TRUE);
        return;
    }

    CloseHandle(g_state.hThread);
    g_state.hThread = NULL;
}

void StopWork(void) {
    InterlockedExchange(&g_state.stopFlag, 1);
    PostStatus("Stopping...");
}

