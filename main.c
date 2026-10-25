// main.c
// gcc -O2 -Wall -municode -mwindows main.c -o TCPing_GUI.exe -lws2_32 -lshell32

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "shell32.lib")

#define APP_PORT 8080
#define WM_TRAY (WM_APP + 1)
#define ID_TRAY_ICON 100
#define ID_TRAY_SHOW 101
#define ID_TRAY_START 102
#define ID_TRAY_STOP 103
#define ID_TRAY_EXIT 104

typedef struct {
    wchar_t aHost[256];
    wchar_t bHost[256];
    int aPort;
    int bPort;
    int durationSec;
} AppConfig;

typedef struct {
    int running;
    int stopRequested;
    int finished;
    int sentA, okA;
    int sentB, okB;
    DWORD sumA, sumB;
    DWORD minA, minB, maxA, maxB;
    wchar_t lastLog[512];
    wchar_t rows[65536];
    CRITICAL_SECTION lock;
} AppState;

static HINSTANCE g_hInst;
static HWND g_hwnd;
static HANDLE g_serverThread;
static HANDLE g_workerThread;
static volatile int g_shutdown = 0;
static AppState g_state;
static AppConfig g_cfg;

static const char* g_shellHtml =
"<!doctype html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>TCPing Web</title></head><body>"
"<div id='app'>Loading...</div>"
"<script src='/main.js'></script>"
"</body></html>";

static void SafeCopyW(wchar_t* dst, size_t cap, const wchar_t* src) {
    if (!dst || cap == 0) return;
    if (!src) src = L"";
    wcsncpy(dst, src, cap - 1);
    dst[cap - 1] = 0;
}

static void StateInit(void) {
    ZeroMemory(&g_state, sizeof(g_state));
    InitializeCriticalSection(&g_state.lock);
}

static void StateFree(void) {
    DeleteCriticalSection(&g_state.lock);
}

static void ResetRunState(void) {
    EnterCriticalSection(&g_state.lock);
    g_state.running = 0;
    g_state.stopRequested = 0;
    g_state.finished = 0;
    g_state.sentA = g_state.okA = 0;
    g_state.sentB = g_state.okB = 0;
    g_state.sumA = g_state.sumB = 0;
    g_state.minA = g_state.minB = 0xFFFFFFFF;
    g_state.maxA = g_state.maxB = 0;
    g_state.lastLog[0] = 0;
    g_state.rows[0] = 0;
    LeaveCriticalSection(&g_state.lock);
}

static void LogLine(const wchar_t* s) {
    EnterCriticalSection(&g_state.lock);
    SafeCopyW(g_state.lastLog, 512, s);
    LeaveCriticalSection(&g_state.lock);
}

static BOOL TcpPingOnce(const wchar_t* host, int port, DWORD* latencyMs) {
    struct addrinfoW hints, *res = NULL, *p = NULL;
    wchar_t portStr[16];
    SOCKET s = INVALID_SOCKET;
    LARGE_INTEGER t1, t2, freq;
    BOOL ok = FALSE;

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

    for (p = res; p; p = p->ai_next) {
        s = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (s == INVALID_SOCKET) continue;

        QueryPerformanceCounter(&t1);
        if (connect(s, p->ai_addr, (int)p->ai_addrlen) == 0) {
            QueryPerformanceCounter(&t2);
            *latencyMs = (DWORD)((t2.QuadPart - t1.QuadPart) * 1000 / freq.QuadPart);
            ok = TRUE;
            closesocket(s);
            break;
        }
        closesocket(s);
        s = INVALID_SOCKET;
    }

    FreeAddrInfoW(res);
    return ok;
}

static void AppendRow(const wchar_t* timeText, const wchar_t* aText, const wchar_t* bText,
                      int aPct, int bPct, const wchar_t* note) {
    wchar_t row[2048];
    wchar_t aBar[256], bBar[256];

    if (aPct < 0) aPct = 0;
    if (aPct > 100) aPct = 100;
    if (bPct < 0) bPct = 0;
    if (bPct > 100) bPct = 100;

    wsprintfW(aBar, L"<div class='barbox'><div class='bar a' style='width:%d%%'></div></div>", aPct);
    wsprintfW(bBar, L"<div class='barbox'><div class='bar b' style='width:%d%%'></div></div>", bPct);

    wsprintfW(row,
        L"<tr><td>%s</td><td>%s</td><td>%s</td><td>%s</td><td>%s</td><td>%s</td></tr>",
        timeText, aText, bText, aBar, bBar, note);

    EnterCriticalSection(&g_state.lock);
    lstrcatW(g_state.rows, row);
    LeaveCriticalSection(&g_state.lock);
}

static DWORD WINAPI WorkerThread(LPVOID lp) {
    (void)lp;
    AppConfig cfg;
    int i, loops;
    SYSTEMTIME st;

    EnterCriticalSection(&g_state.lock);
    cfg = g_cfg;
    g_state.running = 1;
    LeaveCriticalSection(&g_state.lock);

    loops = cfg.durationSec > 0 ? cfg.durationSec : 60;

    for (i = 0; i < loops && !g_shutdown; i++) {
        wchar_t timeText[32], aText[64], bText[64], note[32];
        DWORD aLat = 0, bLat = 0;
        BOOL okA, okB;

        if (g_state.stopRequested) break;

        GetLocalTime(&st);
        wsprintfW(timeText, L"%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);

        okA = TcpPingOnce(cfg.aHost, cfg.aPort, &aLat);
        okB = TcpPingOnce(cfg.bHost, cfg.bPort, &bLat);

        EnterCriticalSection(&g_state.lock);
        g_state.sentA++;
        g_state.sentB++;
        if (okA) {
            g_state.okA++;
            g_state.sumA += aLat;
            if (aLat < g_state.minA) g_state.minA = aLat;
            if (aLat > g_state.maxA) g_state.maxA = aLat;
        }
        if (okB) {
            g_state.okB++;
            g_state.sumB += bLat;
            if (bLat < g_state.minB) g_state.minB = bLat;
            if (bLat > g_state.maxB) g_state.maxB = bLat;
        }
        LeaveCriticalSection(&g_state.lock);

        if (okA) wsprintfW(aText, L"%lu ms", aLat); else lstrcpyW(aText, L"TIMEOUT");
        if (okB) wsprintfW(bText, L"%lu ms", bLat); else lstrcpyW(bText, L"TIMEOUT");
        if (okA && okB) lstrcpyW(note, L"OK");
        else if (!okA && !okB) lstrcpyW(note, L"双失败");
        else lstrcpyW(note, L"单失败");

        AppendRow(
            timeText, aText, bText,
            okA ? (int)(100 - (aLat > 1000 ? 100 : aLat / 10)) : 0,
            okB ? (int)(100 - (bLat > 1000 ? 100 : bLat / 10)) : 0,
            note
        );

        EnterCriticalSection(&g_state.lock);
        wsprintfW(g_state.lastLog, L"第 %d 次：A=%s, B=%s", i + 1, aText, bText);
        LeaveCriticalSection(&g_state.lock);

        Sleep(1000);
    }

    EnterCriticalSection(&g_state.lock);
    g_state.running = 0;
    g_state.finished = 1;
    LeaveCriticalSection(&g_state.lock);

    return 0;
}

static void StartRun(const AppConfig* cfg) {
    if (g_state.running) return;
    EnterCriticalSection(&g_state.lock);
    g_cfg = *cfg;
    LeaveCriticalSection(&g_state.lock);
    ResetRunState();
    g_workerThread = CreateThread(NULL, 0, WorkerThread, NULL, 0, NULL);
}

static void StopRun(void) {
    EnterCriticalSection(&g_state.lock);
    g_state.stopRequested = 1;
    LeaveCriticalSection(&g_state.lock);
    LogLine(L"已请求停止");
}

static void BuildStatusJson(char* out, size_t cap) {
    wchar_t wbuf[65536];
    wchar_t aStat[256], bStat[256], rows[65536], logText[1024], runningText[64];
    double avgA, avgB, lossA, lossB;

    EnterCriticalSection(&g_state.lock);
    if (g_state.running) lstrcpyW(runningText, L"运行中");
    else if (g_state.finished) lstrcpyW(runningText, L"已结束");
    else lstrcpyW(runningText, L"等待");

    avgA = g_state.okA ? (double)g_state.sumA / g_state.okA : 0.0;
    avgB = g_state.okB ? (double)g_state.sumB / g_state.okB : 0.0;
    lossA = g_state.sentA ? (100.0 * (g_state.sentA - g_state.okA) / g_state.sentA) : 0.0;
    lossB = g_state.sentB ? (100.0 * (g_state.sentB - g_state.okB) / g_state.sentB) : 0.0;

    wsprintfW(aStat, L"发送=%d 接收=%d 丢失=%d(%.0f%%) 最短=%lu 最长=%lu 平均=%.0fms",
        g_state.sentA, g_state.okA, g_state.sentA - g_state.okA, lossA,
        g_state.minA == 0xFFFFFFFF ? 0 : g_state.minA, g_state.maxA, avgA);

    wsprintfW(bStat, L"发送=%d 接收=%d 丢失=%d(%.0f%%) 最短=%lu 最长=%lu 平均=%.0fms",
        g_state.sentB, g_state.okB, g_state.sentB - g_state.okB, lossB,
        g_state.minB == 0xFFFFFFFF ? 0 : g_state.minB, g_state.maxB, avgB);

    SafeCopyW(rows, 65536, g_state.rows);
    SafeCopyW(logText, 1024, g_state.lastLog);
    LeaveCriticalSection(&g_state.lock);

    wsprintfW(wbuf,
        L"{\"running\":%s,\"runningText\":\"%s\",\"aStat\":\"%s\",\"bStat\":\"%s\",\"rowsHtml\":\"%s\",\"logText\":\"%s\"}",
        g_state.running ? L"true" : L"false",
        runningText, aStat, bStat, rows, logText);

    WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, out, (int)cap, NULL, NULL);
}

static void UrlDecode(char* s) {
    char* p = s;
    char hex[3] = {0};
    while (*s) {
        if (*s == '%' && s[1] && s[2]) {
            hex[0] = s[1]; hex[1] = s[2];
            *p++ = (char)strtoul(hex, NULL, 16);
            s += 3;
        } else if (*s == '+') {
            *p++ = ' ';
            s++;
        } else {
            *p++ = *s++;
        }
    }
    *p = 0;
}

static int JsonGetString(const char* body, const char* key, char* out, size_t cap) {
    const char* p = strstr(body, key);
    const char* q;
    size_t n;
    if (!p) return 0;
    p = strchr(p, ':');
    if (!p) return 0;
    p++;
    while (*p == ' ' || *p == '"') p++;
    q = p;
    while (*q && *q != '"' && *q != ',' && *q != '}') q++;
    n = (size_t)(q - p);
    if (n >= cap) n = cap - 1;
    memcpy(out, p, n);
    out[n] = 0;
    return 1;
}

static int JsonGetInt(const char* body, const char* key, int defv) {
    const char* p = strstr(body, key);
    if (!p) return defv;
    p = strchr(p, ':');
    if (!p) return defv;
    return atoi(p + 1);
}

static void Utf8ToWideSafe(const char* src, wchar_t* dst, size_t cap) {
    MultiByteToWideChar(CP_UTF8, 0, src, -1, dst, (int)cap);
    dst[cap - 1] = 0;
}

static void HandleHttpClient(SOCKET c) {
    char req[8192];
    int n = recv(c, req, sizeof(req) - 1, 0);
    if (n <= 0) return;
    req[n] = 0;

    char method[16] = {0}, path[256] = {0};
    sscanf(req, "%15s %255s", method, path);

    if (strcmp(path, "/") == 0) {
        const char* hdr = "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\n\r\n";
        send(c, hdr, (int)strlen(hdr), 0);
        send(c, g_shellHtml, (int)strlen(g_shellHtml), 0);
        return;
    }

    if (strcmp(path, "/main.js") == 0) {
        FILE* f;
        char buf[8192];
        size_t r;
        f = fopen("main.js", "rb");
        if (!f) {
            const char* hdr = "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain; charset=utf-8\r\nConnection: close\r\n\r\nmain.js not found";
            send(c, hdr, (int)strlen(hdr), 0);
            return;
        }
        {
            const char* hdr = "HTTP/1.1 200 OK\r\nContent-Type: application/javascript; charset=utf-8\r\nConnection: close\r\n\r\n";
            send(c, hdr, (int)strlen(hdr), 0);
        }
        while ((r = fread(buf, 1, sizeof(buf), f)) > 0) {
            send(c, buf, (int)r, 0);
        }
        fclose(f);
        return;
    }

    if (strcmp(path, "/api/status") == 0) {
        char json[65536];
        BuildStatusJson(json, sizeof(json));
        const char* hdr = "HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\nConnection: close\r\n\r\n";
        send(c, hdr, (int)strlen(hdr), 0);
        send(c, json, (int)strlen(json), 0);
        return;
    }

    if (strcmp(path, "/api/start") == 0) {
        const char* body = strstr(req, "\r\n\r\n");
        AppConfig cfg;
        char tmp[512];

        ZeroMemory(&cfg, sizeof(cfg));
        cfg.aPort = 80;
        cfg.bPort = 443;
        cfg.durationSec = 60;

        if (body) body += 4; else body = "";

        if (JsonGetString(body, "\"aHost\"", tmp, sizeof(tmp))) {
            UrlDecode(tmp);
            Utf8ToWideSafe(tmp, cfg.aHost, 256);
        }
        if (JsonGetString(body, "\"bHost\"", tmp, sizeof(tmp))) {
            UrlDecode(tmp);
            Utf8ToWideSafe(tmp, cfg.bHost, 256);
        }
        cfg.aPort = JsonGetInt(body, "\"aPort\"", 80);
        cfg.bPort = JsonGetInt(body, "\"bPort\"", 443);
        cfg.durationSec = JsonGetInt(body, "\"durationSec\"", 60);
        if (cfg.durationSec <= 0) cfg.durationSec = 60;

        if (cfg.aHost[0] && cfg.bHost[0] && cfg.aPort > 0 && cfg.bPort > 0) {
            StartRun(&cfg);
        }

        const char* hdr = "HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\nConnection: close\r\n\r\n{\"ok\":true}";
        send(c, hdr, (int)strlen(hdr), 0);
        return;
    }

    if (strcmp(path, "/api/stop") == 0) {
        StopRun();
        const char* hdr = "HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\nConnection: close\r\n\r\n{\"ok\":true}";
        send(c, hdr, (int)strlen(hdr), 0);
        return;
    }

    {
        const char* hdr = "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain; charset=utf-8\r\nConnection: close\r\n\r\nnot found";
        send(c, hdr, (int)strlen(hdr), 0);
    }
}

static DWORD WINAPI ServerThread(LPVOID lp) {
    SOCKET ls;
    struct sockaddr_in addr;
    int yes = 1;
    (void)lp;

    ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ls == INVALID_SOCKET) return 0;

    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));

    ZeroMemory(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(APP_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(ls, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        closesocket(ls);
        return 0;
    }

    if (listen(ls, 8) != 0) {
        closesocket(ls);
        return 0;
    }

    while (!g_shutdown) {
        fd_set fds;
        struct timeval tv;
        SOCKET c;

        FD_ZERO(&fds);
        FD_SET(ls, &fds);
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        if (select(0, &fds, NULL, NULL, &tv) > 0) {
            c = accept(ls, NULL, NULL);
            if (c != INVALID_SOCKET) {
                HandleHttpClient(c);
                closesocket(c);
            }
        }
    }

    closesocket(ls);
    return 0;
}

static void TrayAdd(HWND hwnd) {
    NOTIFYICONDATAW nid;
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = ID_TRAY_ICON;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    lstrcpyW(nid.szTip, L"TCPing Web");
    Shell_NotifyIconW(NIM_ADD, &nid);
}

static void TrayRemove(HWND hwnd) {
    NOTIFYICONDATAW nid;
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = ID_TRAY_ICON;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

static void TrayPopup(HWND hwnd) {
    HMENU m = CreatePopupMenu();
    POINT pt;
    GetCursorPos(&pt);
    AppendMenuW(m, MF_STRING, ID_TRAY_SHOW, L"显示窗口");
    AppendMenuW(m, MF_STRING, ID_TRAY_START, L"开始");
    AppendMenuW(m, MF_STRING, ID_TRAY_STOP, L"停止");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, ID_TRAY_EXIT, L"退出");
    SetForegroundWindow(hwnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
    DestroyMenu(m);
}

static void OpenBrowser(void) {
    ShellExecuteW(NULL, L"open", L"http://127.0.0.1:8080/", NULL, NULL, SW_SHOWNORMAL);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        TrayAdd(hwnd);
        OpenBrowser();
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case ID_TRAY_SHOW:
            ShowWindow(hwnd, SW_SHOW);
            ShowWindow(hwnd, SW_RESTORE);
            SetForegroundWindow(hwnd);
            break;
        case ID_TRAY_START:
            if (!g_state.running) {
                AppConfig cfg;
                ZeroMemory(&cfg, sizeof(cfg));
                lstrcpyW(cfg.aHost, L"360.com");
                lstrcpyW(cfg.bHost, L"1.1.1.1");
                cfg.aPort = 80;
                cfg.bPort = 443;
                cfg.durationSec = 60;
                StartRun(&cfg);
            }
            break;
        case ID_TRAY_STOP:
            StopRun();
            break;
        case ID_TRAY_EXIT:
            DestroyWindow(hwnd);
            break;
        }
        return 0;
    case WM_TRAY:
        if (lParam == WM_RBUTTONUP) {
            TrayPopup(hwnd);
        } else if (lParam == WM_LBUTTONDBLCLK) {
            ShowWindow(hwnd, SW_SHOW);
            ShowWindow(hwnd, SW_RESTORE);
            SetForegroundWindow(hwnd);
        }
        return 0;
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    case WM_DESTROY:
        g_shutdown = 1;
        StopRun();
        TrayRemove(hwnd);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrev, PWSTR lpCmdLine, int nCmdShow) {
    WNDCLASSW wc;
    HWND hwnd;
    MSG msg;
    WSADATA wsa;

    (void)hPrev;
    (void)lpCmdLine;
    (void)nCmdShow;

    g_hInst = hInstance;
    StateInit();

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        return 0;

    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"TCPingWebWnd";
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);

    RegisterClassW(&wc);

    hwnd = CreateWindowW(
        L"TCPingWebWnd",
        L"TCPing 本地Web版",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 400, 300,
        NULL, NULL, hInstance, NULL);

    g_hwnd = hwnd;
    ShowWindow(hwnd, SW_HIDE);
    UpdateWindow(hwnd);

    g_serverThread = CreateThread(NULL, 0, ServerThread, NULL, 0, NULL);

    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    g_shutdown = 1;
    if (g_serverThread) {
        WaitForSingleObject(g_serverThread, 1500);
        CloseHandle(g_serverThread);
    }

    WSACleanup();
    StateFree();
    return 0;
}
