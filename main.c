#define _WIN32_WINNT 0x0601
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#include <shellapi.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <time.h>

#define PORT 38000
#define MAX_ROWS 256
#define MAX_LOGS 256
#define MAX_DEBUG 256
#define BUF_SIZE 8192
#define WM_TRAYICON (WM_USER + 1)

typedef struct {
    char time[32];
    char a[32];
    char b[32];
    char note[128];
} Row;

static HWND g_hwnd = NULL;
static HMENU g_menu = NULL;
static NOTIFYICONDATAA g_nid = {0};

static volatile LONG g_running = 0;
static HANDLE g_worker = NULL;
static HANDLE g_stopEvent = NULL;

static CRITICAL_SECTION g_lock;

static Row g_rows[MAX_ROWS];
static int g_rowCount = 0;

static char g_logs[MAX_LOGS][256];
static int g_logCount = 0;

static char g_debug[MAX_DEBUG][256];
static int g_debugCount = 0;

static char g_aHost[128] = "127.0.0.1";
static char g_bHost[128] = "8.8.8.8";
static int g_aPort = 80;
static int g_bPort = 80;
static int g_duration = 60;

static const char *g_indexHtml =
"<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=1280,minimum-scale=0.2'><title>TCPing GUI</title><script src='/main.js'></script></head><body></body></html>";

static void add_logf(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    EnterCriticalSection(&g_lock);
    if (g_logCount >= MAX_LOGS) {
        memmove(g_logs, g_logs + 1, sizeof(g_logs[0]) * (MAX_LOGS - 1));
        g_logCount = MAX_LOGS - 1;
    }
    lstrcpynA(g_logs[g_logCount++], buf, 255);
    LeaveCriticalSection(&g_lock);
}

static void add_debugf(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    EnterCriticalSection(&g_lock);
    if (g_debugCount >= MAX_DEBUG) {
        memmove(g_debug, g_debug + 1, sizeof(g_debug[0]) * (MAX_DEBUG - 1));
        g_debugCount = MAX_DEBUG - 1;
    }
    lstrcpynA(g_debug[g_debugCount++], buf, 255);
    LeaveCriticalSection(&g_lock);
}

static void add_row(const char *a, const char *b, const char *note) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    EnterCriticalSection(&g_lock);
    if (g_rowCount >= MAX_ROWS) {
        memmove(g_rows, g_rows + 1, sizeof(g_rows[0]) * (MAX_ROWS - 1));
        g_rowCount = MAX_ROWS - 1;
    }
    snprintf(g_rows[g_rowCount].time, sizeof(g_rows[g_rowCount].time),
             "%02d:%02d:%02d.%03d", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    lstrcpynA(g_rows[g_rowCount].a, a, 31);
    lstrcpynA(g_rows[g_rowCount].b, b, 31);
    lstrcpynA(g_rows[g_rowCount].note, note, 127);
    g_rowCount++;
    LeaveCriticalSection(&g_lock);
}

static int tcp_ping(const char *host, int port, char *out, int outsz) {
    struct addrinfo hints, *res = NULL;
    SOCKET s = INVALID_SOCKET;
    char portbuf[16];
    LARGE_INTEGER freq, t1, t2;

    ZeroMemory(&hints, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;

    snprintf(portbuf, sizeof(portbuf), "%d", port);
    if (getaddrinfo(host, portbuf, &hints, &res) != 0) {
        lstrcpynA(out, "DNS", outsz - 1);
        return -1;
    }

    s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == INVALID_SOCKET) {
        freeaddrinfo(res);
        lstrcpynA(out, "SOCK", outsz - 1);
        return -1;
    }

    DWORD timeout = 1500;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout, sizeof(timeout));

    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t1);

    int rc = connect(s, res->ai_addr, (int)res->ai_addrlen);

    QueryPerformanceCounter(&t2);
    closesocket(s);
    freeaddrinfo(res);

    if (rc == 0) {
        double ms = (double)(t2.QuadPart - t1.QuadPart) * 1000.0 / (double)freq.QuadPart;
        snprintf(out, outsz, "%.1fms", ms);
        return (int)(ms + 0.5);
    } else {
        lstrcpynA(out, "FAIL", outsz - 1);
        return -1;
    }
}

static int port_available(int port) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return 0;

    struct sockaddr_in addr;
    ZeroMemory(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = htons((u_short)port);

    int ok = bind(s, (struct sockaddr *)&addr, sizeof(addr)) == 0;
    closesocket(s);
    return ok;
}

static unsigned __stdcall worker_thread(void *arg) {
    (void)arg;
    add_debugf("worker start");
    add_logf("started");

    time_t start = time(NULL);
    while (WaitForSingleObject(g_stopEvent, 0) == WAIT_TIMEOUT) {
        if (g_duration > 0 && (int)(time(NULL) - start) >= g_duration) {
            add_debugf("duration reached");
            break;
        }

        char a[32], b[32];
        int ra = tcp_ping(g_aHost, g_aPort, a, sizeof(a));
        int rb = tcp_ping(g_bHost, g_bPort, b, sizeof(b));

        char note[128] = {0};
        if (ra >= 0 && rb >= 0) {
            if (ra < rb) strcpy(note, "A faster");
            else if (rb < ra) strcpy(note, "B faster");
            else strcpy(note, "same");
        } else if (ra >= 0) {
            strcpy(note, "B fail");
        } else if (rb >= 0) {
            strcpy(note, "A fail");
        } else {
            strcpy(note, "both fail");
        }

        add_row(a, b, note);
        add_debugf("tick a=%s b=%s", a, b);
        Sleep(1000);
    }

    InterlockedExchange(&g_running, 0);
    add_logf("stopped");
    add_debugf("worker exit");
    return 0;
}

static void start_worker(void) {
    if (InterlockedCompareExchange(&g_running, 1, 0) != 0) return;

    ResetEvent(g_stopEvent);
    g_worker = (HANDLE)_beginthreadex(NULL, 0, worker_thread, NULL, 0, NULL);
    add_debugf("start worker");
}

static void stop_worker(void) {
    if (InterlockedCompareExchange(&g_running, 0, 1) == 1) {
        SetEvent(g_stopEvent);
        if (g_worker) {
            WaitForSingleObject(g_worker, INFINITE);
            CloseHandle(g_worker);
            g_worker = NULL;
        }
        add_debugf("stop worker");
    }
}

static void open_browser(void) {
    ShellExecuteA(NULL, "open", "http://127.0.0.1:38000/", NULL, NULL, SW_SHOWNORMAL);
}

static void tray_add(HWND hwnd) {
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    lstrcpynA(g_nid.szTip, "TCPing GUI", sizeof(g_nid.szTip));
    Shell_NotifyIconA(NIM_ADD, &g_nid);
}

static void tray_del(void) {
    Shell_NotifyIconA(NIM_DELETE, &g_nid);
}

static void show_menu(HWND hwnd) {
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(g_menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
}

static void send_text(SOCKET s, const char *buf) {
    send(s, buf, (int)strlen(buf), 0);
}

static void json_escape_append(char *dst, size_t *pos, size_t cap, const char *src) {
    while (*src && *pos + 2 < cap) {
        unsigned char c = (unsigned char)*src++;
        if (c == '\\' || c == '"') {
            dst[(*pos)++] = '\\';
            dst[(*pos)++] = c;
        } else if (c == '\n') {
            dst[(*pos)++] = '\\';
            dst[(*pos)++] = 'n';
        } else if (c == '\r') {
        } else {
            dst[(*pos)++] = c;
        }
    }
}

static void respond_text(SOCKET s, const char *ctype, const char *body) {
    char hdr[256];
    snprintf(hdr, sizeof(hdr),
             "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
             ctype, strlen(body));
    send_text(s, hdr);
    send_text(s, body);
}

static void respond_json(SOCKET s, const char *body) {
    respond_text(s, "application/json; charset=utf-8", body);
}

static int socket_recv_all(SOCKET s, char *buf, int cap) {
    int used = 0;
    int n;
    while (used < cap - 1) {
        n = recv(s, buf + used, cap - 1 - used, 0);
        if (n <= 0) break;
        used += n;
        buf[used] = 0;
        if (strstr(buf, "\r\n\r\n")) break;
    }
    return used;
}

static void handle_client(SOCKET c) {
    char req[BUF_SIZE];
    socket_recv_all(c, req, sizeof(req));
    add_debugf("request in");

    if (strncmp(req, "GET / HTTP/1.1", 14) == 0) {
        respond_text(c, "text/html; charset=utf-8", g_indexHtml);
        return;
    }

    if (strncmp(req, "GET /main.js HTTP/1.1", 21) == 0) {
        HANDLE f = CreateFileA("main.js", GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f == INVALID_HANDLE_VALUE) {
            respond_text(c, "application/javascript; charset=utf-8", "document.body.innerHTML='missing main.js';");
            return;
        }
        DWORD size = GetFileSize(f, NULL);
        char *js = (char *)GlobalAlloc(GMEM_FIXED, size + 1);
        DWORD n = 0;
        ReadFile(f, js, size, &n, NULL);
        js[n] = 0;
        CloseHandle(f);
        respond_text(c, "application/javascript; charset=utf-8", js);
        GlobalFree(js);
        return;
    }

    if (strncmp(req, "GET /api/status HTTP/1.1", 24) == 0) {
        char body[65536];
        size_t p = 0;

        EnterCriticalSection(&g_lock);

        p += snprintf(body + p, sizeof(body) - p, "{");
        p += snprintf(body + p, sizeof(body) - p, "\"running\":%s,", g_running ? "true" : "false");
        p += snprintf(body + p, sizeof(body) - p, "\"a_host\":\"%s\",\"a_port\":%d,", g_aHost, g_aPort);
        p += snprintf(body + p, sizeof(body) - p, "\"b_host\":\"%s\",\"b_port\":%d,", g_bHost, g_bPort);
        p += snprintf(body + p, sizeof(body) - p, "\"duration\":%d,", g_duration);

        p += snprintf(body + p, sizeof(body) - p, "\"rows\":[");
        for (int i = 0; i < g_rowCount; i++) {
            if (i) p += snprintf(body + p, sizeof(body) - p, ",");
            p += snprintf(body + p, sizeof(body) - p,
                          "{\"time\":\"%s\",\"a\":\"%s\",\"b\":\"%s\",\"note\":\"%s\"}",
                          g_rows[i].time, g_rows[i].a, g_rows[i].b, g_rows[i].note);
        }
        p += snprintf(body + p, sizeof(body) - p, "],");

        p += snprintf(body + p, sizeof(body) - p, "\"logs\":[");
        for (int i = 0; i < g_logCount; i++) {
            if (i) p += snprintf(body + p, sizeof(body) - p, ",");
            p += snprintf(body + p, sizeof(body) - p, "\"");
            json_escape_append(body, &p, sizeof(body), g_logs[i]);
            p += snprintf(body + p, sizeof(body) - p, "\"");
        }
        p += snprintf(body + p, sizeof(body) - p, "],");

        p += snprintf(body + p, sizeof(body) - p, "\"debug\":[");
        for (int i = 0; i < g_debugCount; i++) {
            if (i) p += snprintf(body + p, sizeof(body) - p, ",");
            p += snprintf(body + p, sizeof(body) - p, "\"");
            json_escape_append(body, &p, sizeof(body), g_debug[i]);
            p += snprintf(body + p, sizeof(body) - p, "\"");
        }
        p += snprintf(body + p, sizeof(body) - p, "],");

        p += snprintf(body + p, sizeof(body) - p, "\"a_stats\":\"running=%s\",\"b_stats\":\"running=%s\"",
                      g_running ? "yes" : "no", g_running ? "yes" : "no");
        p += snprintf(body + p, sizeof(body) - p, "}");

        LeaveCriticalSection(&g_lock);

        respond_json(c, body);
        return;
    }

    if (strncmp(req, "POST /api/start HTTP/1.1", 24) == 0) {
        char *body = strstr(req, "\r\n\r\n");
        if (body) body += 4; else body = "";
        char a[128] = {0}, b[128] = {0};
        int ap = 80, bp = 80, dur = 60;

        sscanf(body,
               "{\"a_host\":\"%127[^\"]\",\"a_port\":\"%d\",\"b_host\":\"%127[^\"]\",\"b_port\":\"%d\",\"duration\":\"%d\"}",
               a, &ap, b, &bp, &dur);

        if (a[0]) lstrcpynA(g_aHost, a, sizeof(g_aHost));
        if (b[0]) lstrcpynA(g_bHost, b, sizeof(g_bHost));
        g_aPort = ap > 0 ? ap : 80;
        g_bPort = bp > 0 ? bp : 80;
        g_duration = dur > 0 ? dur : 60;

        add_debugf("api start %s:%d %s:%d dur=%d", g_aHost, g_aPort, g_bHost, g_bPort, g_duration);
        start_worker();
        respond_json(c, "{\"ok\":true}");
        return;
    }

    if (strncmp(req, "POST /api/stop HTTP/1.1", 23) == 0) {
        add_debugf("api stop");
        stop_worker();
        respond_json(c, "{\"ok\":true}");
        return;
    }

    respond_text(c, "text/plain; charset=utf-8", "not found");
}

static unsigned __stdcall client_thread(void *arg) {
    SOCKET c = (SOCKET)(uintptr_t)arg;
    handle_client(c);
    closesocket(c);
    return 0;
}

static unsigned __stdcall server_thread(void *arg) {
    (void)arg;
    WSADATA wsa;
    SOCKET s = INVALID_SOCKET;
    struct sockaddr_in addr;
    int reuse = 1;

    WSAStartup(MAKEWORD(2, 2), &wsa);
    add_debugf("wsastartup");

    if (!port_available(PORT)) {
        add_logf("port 38000 busy");
        add_debugf("port check failed");
        return 0;
    }

    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        add_logf("socket fail");
        return 0;
    }

    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));

    ZeroMemory(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = htons(PORT);

    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        add_logf("bind fail");
        closesocket(s);
        return 0;
    }

    if (listen(s, 16) != 0) {
        add_logf("listen fail");
        closesocket(s);
        return 0;
    }

    add_debugf("listen 127.0.0.1:%d", PORT);
    add_logf("server ready");

    open_browser();

    while (1) {
        SOCKET c = accept(s, NULL, NULL);
        if (c == INVALID_SOCKET) continue;
        _beginthreadex(NULL, 0, client_thread, (void *)(uintptr_t)c, 0, NULL);
    }

    closesocket(s);
    return 0;
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        tray_add(hwnd);
        break;
    case WM_TRAYICON:
        if (lParam == WM_RBUTTONUP) show_menu(hwnd);
        if (lParam == WM_LBUTTONDBLCLK) ShowWindow(hwnd, SW_SHOW);
        break;
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case 1: ShowWindow(hwnd, SW_SHOW); break;
        case 2: start_worker(); break;
        case 3: stop_worker(); break;
        case 4:
            stop_worker();
            tray_del();
            DestroyWindow(hwnd);
            break;
        }
        break;
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    case WM_DESTROY:
        stop_worker();
        tray_del();
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, PWSTR lpCmd, int nCmdShow) {
    (void)hPrev; (void)lpCmd; (void)nCmdShow;

    InitializeCriticalSection(&g_lock);
    g_stopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = hInst;
    wc.lpszClassName = "TCPingGUIWnd";
    RegisterClassA(&wc);

    g_hwnd = CreateWindowA("TCPingGUIWnd", "TCPing GUI", WS_OVERLAPPEDWINDOW,
                           CW_USEDEFAULT, CW_USEDEFAULT, 640, 480,
                           NULL, NULL, hInst, NULL);

    g_menu = CreatePopupMenu();
    AppendMenuA(g_menu, MF_STRING, 1, "Show Window");
    AppendMenuA(g_menu, MF_STRING, 2, "Start");
    AppendMenuA(g_menu, MF_STRING, 3, "Stop");
    AppendMenuA(g_menu, MF_SEPARATOR, 0, NULL);
    AppendMenuA(g_menu, MF_STRING, 4, "Exit");

    _beginthreadex(NULL, 0, server_thread, NULL, 0, NULL);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    DeleteCriticalSection(&g_lock);
    return 0;
}
