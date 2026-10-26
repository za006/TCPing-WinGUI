#define _WIN32_WINNT 0x0600
#define _WINSOCK_DEPRECATED_NO_WARNINGS

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>

#define BASE_PORT 38000
#define MAX_PORT_TRY 100
#define MAX_LOG_LINES 256
#define MAX_ROWS 256
#define ID_TRAY_ICON 1001
#define WM_TRAYMSG (WM_APP + 1)

typedef struct {
    char time[32];
    char a[32];
    char b[32];
    char note[128];
} Row;

static HWND g_hwnd = NULL;
static SOCKET g_listen = INVALID_SOCKET;
static HANDLE g_server_thread = NULL;
static HANDLE g_ping_thread = NULL;
static HANDLE g_stop_event = NULL;
static CRITICAL_SECTION g_lock;
static int g_port = BASE_PORT;
static int g_ping_active = 0;

static char g_a_host[128] = "";
static int g_a_port = 80;
static char g_b_host[128] = "";
static int g_b_port = 80;
static int g_duration = 60;

static char g_logs[MAX_LOG_LINES][256];
static int g_log_count = 0;
static char g_debug[MAX_LOG_LINES][256];
static int g_debug_count = 0;
static Row g_rows[MAX_ROWS];
static int g_row_count = 0;

static NOTIFYICONDATAW g_nid;

static void push_line(char arr[MAX_LOG_LINES][256], int *count, const char *fmt, va_list ap) {
    char buf[256];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    EnterCriticalSection(&g_lock);
    if (*count < MAX_LOG_LINES) {
        strcpy(arr[*count], buf);
        (*count)++;
    } else {
        memmove(arr, arr + 1, sizeof(arr[0]) * (MAX_LOG_LINES - 1));
        strcpy(arr[MAX_LOG_LINES - 1], buf);
    }
    LeaveCriticalSection(&g_lock);
}

static void log_msg(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    push_line(g_logs, &g_log_count, fmt, ap);
    va_end(ap);
}

static void debug_msg(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    push_line(g_debug, &g_debug_count, fmt, ap);
    va_end(ap);
}

static void now_text(char *buf, int n) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(buf, n, "%04d-%02d-%02d %02d:%02d:%02d",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
}

static void add_row(const char *a, const char *b, const char *note) {
    char t[32];
    now_text(t, sizeof(t));
    EnterCriticalSection(&g_lock);
    if (g_row_count < MAX_ROWS) {
        strcpy(g_rows[g_row_count].time, t);
        snprintf(g_rows[g_row_count].a, sizeof(g_rows[g_row_count].a), "%s", a);
        snprintf(g_rows[g_row_count].b, sizeof(g_rows[g_row_count].b), "%s", b);
        snprintf(g_rows[g_row_count].note, sizeof(g_rows[g_row_count].note), "%s", note);
        g_row_count++;
    } else {
        memmove(g_rows, g_rows + 1, sizeof(g_rows[0]) * (MAX_ROWS - 1));
        strcpy(g_rows[MAX_ROWS - 1].time, t);
        snprintf(g_rows[MAX_ROWS - 1].a, sizeof(g_rows[MAX_ROWS - 1].a), "%s", a);
        snprintf(g_rows[MAX_ROWS - 1].b, sizeof(g_rows[MAX_ROWS - 1].b), "%s", b);
        snprintf(g_rows[MAX_ROWS - 1].note, sizeof(g_rows[MAX_ROWS - 1].note), "%s", note);
    }
    LeaveCriticalSection(&g_lock);
}

static void trim(char *s) {
    int len = (int)strlen(s);
    while (len > 0 && (s[len - 1] == '\r' || s[len - 1] == '\n' || s[len - 1] == ' ' || s[len - 1] == '\t')) s[--len] = 0;
    while (*s == ' ' || *s == '\t') memmove(s, s + 1, strlen(s));
}

static void url_decode(char *s) {
    char *r = s, *w = s;
    while (*r) {
        if (*r == '+') {
            *w++ = ' ';
            r++;
        } else if (*r == '%' && r[1] && r[2]) {
            char hex[3] = { r[1], r[2], 0 };
            *w++ = (char)strtol(hex, NULL, 16);
            r += 3;
        } else {
            *w++ = *r++;
        }
    }
    *w = 0;
}

static void json_escape(const char *s, char *out, int outsz) {
    int i = 0;
    for (; *s && i < outsz - 2; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '\\' || c == '"') {
            out[i++] = '\\';
            out[i++] = c;
        } else if (c == '\n') {
            out[i++] = '\\';
            out[i++] = 'n';
        } else if (c == '\r') {
        } else {
            out[i++] = c;
        }
    }
    out[i] = 0;
}

static int send_all(SOCKET s, const char *buf, int len) {
    int sent = 0;
    while (sent < len) {
        int n = send(s, buf + sent, len - sent, 0);
        if (n <= 0) return 0;
        sent += n;
    }
    return 1;
}

static void respond(SOCKET s, const char *ctype, const char *body) {
    char hdr[256];
    snprintf(hdr, sizeof(hdr),
             "HTTP/1.1 200 OK\r\n"
             "Content-Type: %s\r\n"
             "Access-Control-Allow-Origin: *\r\n"
             "Connection: close\r\n"
             "Content-Length: %d\r\n\r\n",
             ctype, (int)strlen(body));
    send_all(s, hdr, (int)strlen(hdr));
    send_all(s, body, (int)strlen(body));
}

static void respond_404(SOCKET s) {
    const char *body = "not found";
    char hdr[256];
    snprintf(hdr, sizeof(hdr),
             "HTTP/1.1 404 Not Found\r\n"
             "Connection: close\r\n"
             "Content-Length: %d\r\n\r\n%s",
             (int)strlen(body), body);
    send_all(s, hdr, (int)strlen(hdr));
}

static void parse_kv(char *body) {
    char *p;

    if ((p = strstr(body, "a_host="))) { p += 7; sscanf(p, "%127[^&}", g_a_host); url_decode(g_a_host); trim(g_a_host); }
    if ((p = strstr(body, "a_port="))) g_a_port = atoi(p + 7);
    if ((p = strstr(body, "b_host="))) { p += 7; sscanf(p, "%127[^&}", g_b_host); url_decode(g_b_host); trim(g_b_host); }
    if ((p = strstr(body, "b_port="))) g_b_port = atoi(p + 7);
    if ((p = strstr(body, "duration="))) g_duration = atoi(p + 9);

    if (g_a_port <= 0) g_a_port = 80;
    if (g_b_port <= 0) g_b_port = 80;
    if (g_duration < 0) g_duration = 60;
}

static void parse_jsonish(char *body) {
    char *p;
    char tmp[256];

    if ((p = strstr(body, "\"a_host\""))) {
        p = strchr(p + 8, ':');
        if (p) {
            p++;
            while (*p == ' ' || *p == '"') p++;
            sscanf(p, "%127[^\"]", tmp);
            strcpy(g_a_host, tmp);
            trim(g_a_host);
        }
    }
    if ((p = strstr(body, "\"a_port\""))) {
        p = strchr(p + 8, ':');
        if (p) g_a_port = atoi(p + 1);
    }
    if ((p = strstr(body, "\"b_host\""))) {
        p = strchr(p + 8, ':');
        if (p) {
            p++;
            while (*p == ' ' || *p == '"') p++;
            sscanf(p, "%127[^\"]", tmp);
            strcpy(g_b_host, tmp);
            trim(g_b_host);
        }
    }
    if ((p = strstr(body, "\"b_port\""))) {
        p = strchr(p + 8, ':');
        if (p) g_b_port = atoi(p + 1);
    }
    if ((p = strstr(body, "\"duration\""))) {
        p = strchr(p + 10, ':');
        if (p) g_duration = atoi(p + 1);
    }

    if (g_a_port <= 0) g_a_port = 80;
    if (g_b_port <= 0) g_b_port = 80;
    if (g_duration < 0) g_duration = 60;
}

static void parse_post(char *body) {
    if (strchr(body, '=')) parse_kv(body);
    else parse_jsonish(body);
}

static int connect_tcp(const char *host, int port, int timeout_ms, int *ms_out) {
    struct addrinfo hints, *res = NULL, *p = NULL;
    char portbuf[16];
    SOCKET s = INVALID_SOCKET;
    int ok = 0;
    DWORD start = GetTickCount();

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(portbuf, sizeof(portbuf), "%d", port);

    if (getaddrinfo(host, portbuf, &hints, &res) != 0) return 0;

    for (p = res; p; p = p->ai_next) {
        s = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (s == INVALID_SOCKET) continue;

        u_long nb = 1;
        ioctlsocket(s, FIONBIO, &nb);
        connect(s, p->ai_addr, (int)p->ai_addrlen);

        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(s, &wfds);

        struct timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;

        if (select(0, NULL, &wfds, NULL, &tv) > 0) {
            int err = 0, len = sizeof(err);
            getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&err, &len);
            if (err == 0) {
                ok = 1;
                break;
            }
        }

        closesocket(s);
        s = INVALID_SOCKET;
    }

    if (res) freeaddrinfo(res);

    if (ok) {
        if (ms_out) *ms_out = (int)(GetTickCount() - start);
        closesocket(s);
        return 1;
    }
    return 0;
}

static DWORD WINAPI ping_thread(void *arg) {
    (void)arg;
    g_ping_active = 1;
    debug_msg("ping thread started");
    log_msg("ping started");

    DWORD start = GetTickCount();
    while (WaitForSingleObject(g_stop_event, 0) == WAIT_TIMEOUT) {
        if (g_duration > 0 && (GetTickCount() - start) >= (DWORD)g_duration * 1000) break;

        int ma = -1, mb = -1;
        char aa[32], bb[32], note[128];

        if (g_a_host[0]) {
            if (connect_tcp(g_a_host, g_a_port, 3000, &ma)) snprintf(aa, sizeof(aa), "%d", ma);
            else strcpy(aa, "fail");
        } else strcpy(aa, "-");

        if (g_b_host[0]) {
            if (connect_tcp(g_b_host, g_b_port, 3000, &mb)) snprintf(bb, sizeof(bb), "%d", mb);
            else strcpy(bb, "fail");
        } else strcpy(bb, "-");

        snprintf(note, sizeof(note), "A=%s B=%s", aa, bb);
        add_row(aa, bb, note);
        log_msg("%s", note);

        Sleep(1000);
    }

    log_msg("ping stopped");
    debug_msg("ping thread exit");
    g_ping_active = 0;
    g_ping_thread = NULL;
    return 0;
}

static void stop_ping(void) {
    if (g_stop_event) SetEvent(g_stop_event);
    if (g_ping_thread) {
        WaitForSingleObject(g_ping_thread, 3000);
        CloseHandle(g_ping_thread);
        g_ping_thread = NULL;
    }
    g_ping_active = 0;
    if (g_stop_event) ResetEvent(g_stop_event);
}

static int start_ping(void) {
    if (g_ping_thread) return 1;
    if (g_stop_event) ResetEvent(g_stop_event);
    g_ping_thread = CreateThread(NULL, 0, ping_thread, NULL, 0, NULL);
    return g_ping_thread != NULL;
}

static void html_page(char *out, int outsz) {
    snprintf(out, outsz,
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>TCPing</title><script src='/main.js'></script></head><body></body></html>");
}

static void json_status(char *out, int outsz) {
    char logs[8192] = "[";
    char dbg[8192] = "[";
    char rows[16384] = "[";

    EnterCriticalSection(&g_lock);
    for (int i = 0; i < g_log_count; i++) {
        char e[512];
        json_escape(g_logs[i], e, sizeof(e));
        strcat(logs, "\""); strcat(logs, e); strcat(logs, "\"");
        if (i != g_log_count - 1) strcat(logs, ",");
    }
    for (int i = 0; i < g_debug_count; i++) {
        char e[512];
        json_escape(g_debug[i], e, sizeof(e));
        strcat(dbg, "\""); strcat(dbg, e); strcat(dbg, "\"");
        if (i != g_debug_count - 1) strcat(dbg, ",");
    }
    for (int i = 0; i < g_row_count; i++) {
        char et[64], ea[64], eb[64], en[256];
        json_escape(g_rows[i].time, et, sizeof(et));
        json_escape(g_rows[i].a, ea, sizeof(ea));
        json_escape(g_rows[i].b, eb, sizeof(eb));
        json_escape(g_rows[i].note, en, sizeof(en));
        char item[512];
        snprintf(item, sizeof(item),
                 "{\"time\":\"%s\",\"a\":\"%s\",\"b\":\"%s\",\"note\":\"%s\"}",
                 et, ea, eb, en);
        strcat(rows, item);
        if (i != g_row_count - 1) strcat(rows, ",");
    }
    LeaveCriticalSection(&g_lock);

    strcat(logs, "]");
    strcat(dbg, "]");
    strcat(rows, "]");

    snprintf(out, outsz,
        "{"
        "\"running\":%s,"
        "\"a_stats\":\"%s:%d\","
        "\"b_stats\":\"%s:%d\","
        "\"last_log\":\"%s\","
        "\"logs\":%s,"
        "\"debug\":%s,"
        "\"rows\":%s"
        "}",
        g_ping_active ? "true" : "false",
        g_a_host[0] ? g_a_host : "-", g_a_port,
        g_b_host[0] ? g_b_host : "-", g_b_port,
        g_log_count ? g_logs[g_log_count - 1] : "-",
        logs, dbg, rows);
}

static void open_browser(int port) {
    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/", port);
    ShellExecuteA(NULL, "open", url, NULL, NULL, SW_SHOWNORMAL);
}

static int try_listen(int port, SOCKET *out) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return 0;

    int opt = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (char *)&opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = htons((u_short)port);

    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR) {
        closesocket(s);
        return 0;
    }
    if (listen(s, 16) == SOCKET_ERROR) {
        closesocket(s);
        return 0;
    }
    *out = s;
    return 1;
}

static DWORD WINAPI server_thread(void *arg) {
    SOCKET ls = (SOCKET)(uintptr_t)arg;
    debug_msg("server thread started");
    for (;;) {
        fd_set rfds;
        struct timeval tv;
        FD_ZERO(&rfds);
        FD_SET(ls, &rfds);
        tv.tv_sec = 0;
        tv.tv_usec = 200000;

        if (WaitForSingleObject(g_stop_event, 0) == WAIT_OBJECT_0) break;
        if (select(0, &rfds, NULL, NULL, &tv) <= 0) continue;

        SOCKET cs = accept(ls, NULL, NULL);
        if (cs == INVALID_SOCKET) continue;

        char req[4096] = {0};
        int pos = 0;
        while (pos < (int)sizeof(req) - 1) {
            int n = recv(cs, req + pos, 1, 0);
            if (n <= 0) break;
            pos += n;
            req[pos] = 0;
            if (strstr(req, "\r\n\r\n")) break;
        }

        char method[16] = {0}, path[128] = {0};
        sscanf(req, "%15s %127s", method, path);
        debug_msg("%s %s", method, path);

        if (!strcmp(method, "GET") && !strcmp(path, "/")) {
            char page[512];
            html_page(page, sizeof(page));
            respond(cs, "text/html; charset=utf-8", page);
        } else if (!strcmp(method, "GET") && !strcmp(path, "/main.js")) {
            HANDLE h = CreateFileA("main.js", GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            if (h != INVALID_HANDLE_VALUE) {
                DWORD sz = GetFileSize(h, NULL);
                char *buf = (char *)malloc(sz + 1);
                DWORD rd = 0;
                if (buf && ReadFile(h, buf, sz, &rd, NULL)) {
                    buf[rd] = 0;
                    respond(cs, "application/javascript; charset=utf-8", buf);
                } else {
                    respond_404(cs);
                }
                if (buf) free(buf);
                CloseHandle(h);
            } else {
                respond_404(cs);
            }
        } else if (!strcmp(method, "GET") && !strcmp(path, "/api/status")) {
            char js[32768];
            json_status(js, sizeof(js));
            respond(cs, "application/json; charset=utf-8", js);
        } else if (!strcmp(method, "POST") && !strcmp(path, "/api/start")) {
            char *p = strstr(req, "\r\n\r\n");
            if (p) {
                char body[2048];
                strncpy(body, p + 4, sizeof(body) - 1);
                body[sizeof(body) - 1] = 0;
                parse_post(body);
            }
            log_msg("start %s:%d %s:%d", g_a_host, g_a_port, g_b_host, g_b_port);
            if (!g_ping_thread) start_ping();
            respond(cs, "application/json; charset=utf-8", "{\"ok\":true}");
        } else if (!strcmp(method, "POST") && !strcmp(path, "/api/stop")) {
            stop_ping();
            log_msg("stop");
            respond(cs, "application/json; charset=utf-8", "{\"ok\":true}");
        } else {
            respond_404(cs);
        }

        shutdown(cs, SD_BOTH);
        closesocket(cs);
    }
    debug_msg("server thread exit");
    return 0;
}

static void tray_add(HWND hwnd) {
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = ID_TRAY_ICON;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYMSG;
    g_nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    lstrcpyW(g_nid.szTip, L"TCPing");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

static void tray_del(void) {
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
}

static void tray_menu(HWND hwnd) {
    POINT pt;
    GetCursorPos(&pt);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 1, L"Show Window");
    AppendMenuW(menu, MF_STRING, 2, L"Start");
    AppendMenuW(menu, MF_STRING, 3, L"Stop");
    AppendMenuW(menu, MF_STRING, 4, L"Exit");
    SetForegroundWindow(hwnd);
    int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
    DestroyMenu(menu);
    if (cmd == 1) ShowWindow(hwnd, SW_SHOW);
    else if (cmd == 2) start_ping();
    else if (cmd == 3) stop_ping();
    else if (cmd == 4) PostMessageW(hwnd, WM_CLOSE, 0, 0);
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TRAYMSG:
        if (lp == WM_RBUTTONUP || lp == WM_LBUTTONDBLCLK) tray_menu(hwnd);
        return 0;
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    case WM_DESTROY:
        tray_del();
        stop_ping();
        if (g_stop_event) SetEvent(g_stop_event);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static int start_server_and_browser(void) {
    for (int p = BASE_PORT; p < BASE_PORT + MAX_PORT_TRY; p++) {
        SOCKET ls = INVALID_SOCKET;
        if (try_listen(p, &ls)) {
            g_port = p;
            g_listen = ls;
            g_server_thread = CreateThread(NULL, 0, server_thread, (void *)(uintptr_t)ls, 0, NULL);
            if (!g_server_thread) {
                closesocket(ls);
                g_listen = INVALID_SOCKET;
                return 0;
            }
            log_msg("server on 127.0.0.1:%d", g_port);
            debug_msg("listen ok %d", g_port);
            open_browser(g_port);
            return 1;
        }
    }
    return 0;
}

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE hPrev, LPWSTR lpCmdLine, int nCmdShow) {
    (void)hPrev;
    (void)lpCmdLine;

    InitializeCriticalSection(&g_lock);

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 0;

    g_stop_event = CreateEvent(NULL, TRUE, FALSE, NULL);

    WNDCLASSW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"TCPingWindow";
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    RegisterClassW(&wc);

    g_hwnd = CreateWindowW(L"TCPingWindow", L"TCPing", WS_OVERLAPPEDWINDOW,
                           CW_USEDEFAULT, CW_USEDEFAULT, 900, 600,
                           NULL, NULL, hInstance, NULL);
    if (!g_hwnd) return 0;

    tray_add(g_hwnd);
    ShowWindow(g_hwnd, nCmdShow);
    UpdateWindow(g_hwnd);

    log_msg("app started");
    debug_msg("wWinMain ok");

    if (!start_server_and_browser()) {
        MessageBoxW(NULL, L"Cannot bind any port from 38000 to 38099.", L"TCPing", MB_ICONERROR);
        return 0;
    }

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_server_thread) {
        SetEvent(g_stop_event);
        WaitForSingleObject(g_server_thread, 3000);
        CloseHandle(g_server_thread);
    }
    if (g_listen != INVALID_SOCKET) closesocket(g_listen);
    if (g_stop_event) CloseHandle(g_stop_event);
    DeleteCriticalSection(&g_lock);
    WSACleanup();
    return 0;
}
