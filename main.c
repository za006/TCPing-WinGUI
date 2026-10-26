#define _WIN32_WINNT 0x0601
#include <winsock2.h>
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <process.h>

#define PORT 38000
#define WM_TRAYICON (WM_USER + 1)
#define ID_SHOW  1001
#define ID_START 1002
#define ID_STOP  1003
#define ID_EXIT  1004

static HWND g_hwnd;
static volatile LONG g_run = 1;
static volatile LONG g_ping = 0;
static volatile LONG g_ready = 0;
static unsigned short g_port = PORT;
static HANDLE g_srv_th = NULL;
static HANDLE g_ping_th = NULL;
static HANDLE g_mtx = NULL;

static char g_debug[16384];
static int g_dbg_pos = 0;
static char g_oper[8192];
static int g_op_pos = 0;
static char g_last[128] = "none";

static char g_a[256] = "1.1.1.1";
static char g_b[256] = "8.8.8.8";
static int g_pa = 80, g_pb = 80, g_dur = 10;

typedef struct {
    char t[16], a[32], b[32], note[64];
    int ams, bms;
} Row;

static Row g_rows[256];
static int g_row_n = 0;

static const char *HTML =
"<!doctype html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>TCPing GUI</title>"
"<style>"
"body{font:12px Segoe UI,Arial;background:#f6f8fb;margin:12px;color:#1f2937}"
".w{max-width:1200px;margin:auto}.c{background:#fff;border:1px solid #dbe3ee;border-radius:8px;padding:10px;margin:0 0 10px}"
".r{display:flex;gap:8px;flex-wrap:wrap;align-items:end}"
"label{display:block;margin-bottom:4px;color:#4b5563}input{height:28px;border:1px solid #cfd8e3;border-radius:6px;padding:0 8px}"
"button{height:30px;border:1px solid #cfd8e3;background:#f8fafc;border-radius:6px;padding:0 12px}"
".s{display:flex;gap:10px;flex-wrap:wrap}.p{background:#eef4ff;border:1px solid #d7e3f3;border-radius:999px;padding:2px 8px}"
"table{width:100%;border-collapse:collapse}th,td{padding:5px 6px;border-bottom:1px solid #e5eaf1;text-align:left}"
"th{background:#f8fafc;position:sticky;top:0}.bars{display:flex;gap:2px;align-items:end;height:18px}"
".a{width:3px;background:#2b7cff}.b{width:3px;background:#ff8a00}"
".box{height:140px;overflow:auto;white-space:pre-wrap;background:#fff;border:1px solid #dbe3ee;border-radius:6px;padding:8px}"
".dbg{background:#fff}.small{width:88px}.mid{width:150px}.wide{width:220px}"
"</style></head><body><div class='w' id='app'></div><script src='/main.js'></script></body></html>";

static const char *JS =
"const $=id=>document.getElementById(id);"
"function bar(v,c){v=v>9998?0:v;let h=Math.max(2,Math.min(18,Math.round(v/10)));return `<div class='bars'><div class='${c}' style='height:${h}px'></div></div>`}"
"function row(r){return `<tr><td>${r.t}</td><td>${r.a}</td><td>${r.b}</td><td>${bar(r.ams,'a')}</td><td>${bar(r.bms,'b')}</td><td>${r.note}</td></tr>`}"
"async function api(u){return await fetch(u,{cache:'no-store'})}"
"async function refresh(){let s=await (await api('/api/status')).json();"
"$('st').textContent='Status: '+(s.running?'Running':'Stopped');"
"$('sa').textContent='A: '+s.a_stats; $('sb').textContent='B: '+s.b_stats; $('sl').textContent='Last log: '+s.last_log;"
"$('op').textContent=s.op_logs||''; $('dbg').textContent=s.debug||'';"
"$('tb').innerHTML=(s.rows||[]).map(row).join('');}"
"async function start(){let q=`/api/start?a=${encodeURIComponent($('a').value)}&pa=${$('pa').value}&b=${encodeURIComponent($('b').value)}&pb=${$('pb').value}&d=${$('d').value}`;await api(q);refresh();}"
"async function stop(){await api('/api/stop');refresh();}"
"document.getElementById('app').innerHTML=`"
"<div class='c'><div class='r'>"
"<div><label>Target A</label><input id='a' class='wide' value='1.1.1.1'></div>"
"<div><label>Port A</label><input id='pa' class='small' type='number' value='80'></div>"
"<div><label>Target B</label><input id='b' class='wide' value='8.8.8.8'></div>"
"<div><label>Port B</label><input id='pb' class='small' type='number' value='80'></div>"
"<div><label>Duration</label><input id='d' class='small' type='number' value='10'></div>"
"<div><button onclick='start()'>Start</button></div><div><button onclick='stop()'>Stop</button></div><div><button onclick='refresh()'>Refresh</button></div>"
"</div></div>"
"<div class='c'><div class='s'><span class='p' id='st'>Status: -</span><span class='p' id='sa'>A: -</span><span class='p' id='sb'>B: -</span><span class='p' id='sl'>Last log: -</span></div></div>"
"<div class='c'><table><thead><tr><th>Time</th><th>A</th><th>B</th><th>A Bar</th><th>B Bar</th><th>Note</th></tr></thead><tbody id='tb'></tbody></table></div>"
"<div class='c'><div>Operational Logs</div><div id='op' class='box'></div></div>"
"<div class='c'><div>Debug Log</div><div id='dbg' class='box dbg'></div></div></div>`;"
"setInterval(refresh,1000);refresh();";

static void lock(void){ WaitForSingleObject(g_mtx, INFINITE); }
static void unlock(void){ ReleaseMutex(g_mtx); }

static void addbuf(char *buf, int *pos, int cap, const char *fmt, ...) {
    char t[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t, sizeof(t), fmt, ap);
    va_end(ap);
    int n = (int)strlen(t);
    if (*pos + n + 1 >= cap) {
        *pos = 0;
        buf[0] = 0;
    }
    memcpy(buf + *pos, t, n);
    *pos += n;
    buf[*pos] = 0;
}

static void dbg(const char *fmt, ...) {
    char t[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t, sizeof(t), fmt, ap);
    va_end(ap);
    lock();
    addbuf(g_debug, &g_dbg_pos, sizeof(g_debug), "[DBG] %s\n", t);
    lstrcpynA(g_last, t, sizeof(g_last));
    unlock();
}

static void op(const char *fmt, ...) {
    char t[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t, sizeof(t), fmt, ap);
    va_end(ap);
    lock();
    addbuf(g_oper, &g_op_pos, sizeof(g_oper), "%s\n", t);
    lstrcpynA(g_last, t, sizeof(g_last));
    unlock();
}

static int port_ok(unsigned short p) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return 0;

    int o = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&o, sizeof(o));

    struct sockaddr_in a;
    ZeroMemory(&a, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(p);

    int ok = (bind(s, (struct sockaddr*)&a, sizeof(a)) == 0);
    closesocket(s);
    return ok;
}

static unsigned short pick_port(void) {
    for (unsigned short p = 38000; p < 38100; p++) {
        if (port_ok(p)) return p;
    }
    return 0;
}

static int send_resp(SOCKET c, const char *type, const char *body) {
    char h[256];
    int bl = (int)strlen(body);
    int hl = wsprintfA(h,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %d\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: close\r\n\r\n",
        type, bl);
    if (send(c, h, hl, 0) == SOCKET_ERROR) return 0;
    if (send(c, body, bl, 0) == SOCKET_ERROR) return 0;
    return 1;
}

static void esc(char *o, size_t n, const char *s) {
    size_t p = 0;
    for (; *s && p + 2 < n; s++) {
        if (*s == '"' || *s == '\\') {
            o[p++] = '\\';
            o[p++] = *s;
        } else if (*s == '\n') {
            o[p++] = '\\';
            o[p++] = 'n';
        } else if (*s != '\r') {
            o[p++] = *s;
        }
    }
    o[p] = 0;
}

static void status_json(char *out, int outsz) {
    char dbg2[32768], op2[16384], last2[256];
    lock();
    esc(dbg2, sizeof(dbg2), g_debug);
    esc(op2, sizeof(op2), g_oper);
    esc(last2, sizeof(last2), g_last);

    int p = 0;
    p += wsprintfA(out + p,
        "{\"running\":%s,"
        "\"a_stats\":\"Target=%s Port=%d\","
        "\"b_stats\":\"Target=%s Port=%d\","
        "\"last_log\":\"%s\","
        "\"debug\":\"%s\","
        "\"op_logs\":\"%s\","
        "\"rows\":[",
        g_run ? "true" : "false", g_a, g_pa, g_b, g_pb, last2, dbg2, op2);

    for (int i = 0; i < g_row_n; i++) {
        Row *r = &g_rows[i];
        p += wsprintfA(out + p,
            "%s{\"t\":\"%s\",\"a\":\"%s\",\"b\":\"%s\",\"ams\":%d,\"bms\":%d,\"note\":\"%s\"}",
            i ? "," : "", r->t, r->a, r->b, r->ams, r->bms, r->note);
    }

    p += wsprintfA(out + p, "]}");
    unlock();
}

static int qv(const char *q, const char *k, char *o, int n) {
    const char *p = strstr(q, k);
    if (!p) return 0;
    p += strlen(k);
    if (*p != '=') return 0;
    p++;
    int i = 0;
    while (*p && *p != '&' && i < n - 1) {
        o[i++] = (*p == '+') ? ' ' : *p;
        p++;
    }
    o[i] = 0;
    return 1;
}

static int tcp_ms(const char *host, int port, int timeout_ms) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return -1;

    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);

    struct sockaddr_in a;
    ZeroMemory(&a, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)port);
    a.sin_addr.s_addr = inet_addr(host);

    if (a.sin_addr.s_addr == INADDR_NONE) {
        struct hostent *he = gethostbyname(host);
        if (!he) {
            closesocket(s);
            return -1;
        }
        memcpy(&a.sin_addr, he->h_addr, he->h_length);
    }

    DWORD t0 = GetTickCount();
    if (connect(s, (struct sockaddr*)&a, sizeof(a)) == 0) {
        closesocket(s);
        return (int)(GetTickCount() - t0);
    }

    fd_set wf;
    FD_ZERO(&wf);
    FD_SET(s, &wf);
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    if (select(0, NULL, &wf, NULL, &tv) > 0) {
        int e = 0, l = sizeof(e);
        getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&e, &l);
        if (e == 0) {
            closesocket(s);
            return (int)(GetTickCount() - t0);
        }
    }

    closesocket(s);
    return -1;
}

static unsigned __stdcall ping_th(void *p) {
    (void)p;
    dbg("ping worker started");
    DWORD st = GetTickCount();

    while (g_ping) {
        SYSTEMTIME tm;
        GetLocalTime(&tm);

        Row r;
        ZeroMemory(&r, sizeof(r));
        wsprintfA(r.t, "%02d:%02d:%02d", tm.wHour, tm.wMinute, tm.wSecond);

        r.ams = tcp_ms(g_a, g_pa, 1000);
        r.bms = tcp_ms(g_b, g_pb, 1000);

        if (r.ams >= 0) wsprintfA(r.a, "%d ms", r.ams);
        else lstrcpyA(r.a, "timeout");

        if (r.bms >= 0) wsprintfA(r.b, "%d ms", r.bms);
        else lstrcpyA(r.b, "timeout");

        lstrcpyA(r.note, "tick");

        lock();
        if (g_row_n < 256) g_rows[g_row_n++] = r;
        else {
            memmove(g_rows, g_rows + 1, sizeof(Row) * 255);
            g_rows[255] = r;
        }
        unlock();

        op("A=%s B=%s", r.a, r.b);

        if (g_dur > 0 && (int)((GetTickCount() - st) / 1000) >= g_dur) break;
        Sleep(1000);
    }

    g_ping = 0;
    dbg("ping worker stopped");
    return 0;
}

static void start_ping(void) {
    if (g_ping) return;
    g_ping = 1;
    g_ping_th = (HANDLE)_beginthreadex(NULL, 0, ping_th, NULL, 0, NULL);
}

static void stop_ping(void) {
    g_ping = 0;
    if (g_ping_th) {
        WaitForSingleObject(g_ping_th, 1500);
        CloseHandle(g_ping_th);
        g_ping_th = NULL;
    }
}

static unsigned __stdcall server_th(void *p) {
    (void)p;

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        dbg("socket failed");
        return 0;
    }

    int o = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&o, sizeof(o));

    struct sockaddr_in a;
    ZeroMemory(&a, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = inet_addr("127.0.0.1");
    a.sin_port = htons(g_port);

    if (bind(s, (struct sockaddr*)&a, sizeof(a)) != 0) {
        dbg("bind failed on %u", g_port);
        closesocket(s);
        return 0;
    }

    if (listen(s, 8) != 0) {
        dbg("listen failed");
        closesocket(s);
        return 0;
    }

    g_ready = 1;
    dbg("server started on 127.0.0.1:%u", g_port);

    while (g_run) {
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(s, &rf);

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 200000;

        if (select(0, &rf, NULL, NULL, &tv) <= 0) continue;

        SOCKET c = accept(s, NULL, NULL);
        if (c == INVALID_SOCKET) continue;

        char req[4096];
        int n = recv(c, req, sizeof(req) - 1, 0);
        if (n <= 0) {
            closesocket(c);
            continue;
        }
        req[n] = 0;

        char m[16] = {0}, path[1024] = {0};
        sscanf(req, "%15s %1023s", m, path);
        dbg("received %s", path);

        if (!strcmp(path, "/")) {
            send_resp(c, "text/html; charset=utf-8", HTML);
        } else if (!strcmp(path, "/main.js")) {
            send_resp(c, "application/javascript; charset=utf-8", JS);
        } else if (!strncmp(path, "/api/start", 10)) {
            char t[256], v[32];
            if (qv(path, "a", t, sizeof(t))) lstrcpynA(g_a, t, sizeof(g_a));
            if (qv(path, "b", t, sizeof(t))) lstrcpynA(g_b, t, sizeof(g_b));
            if (qv(path, "pa", v, sizeof(v))) g_pa = atoi(v);
            if (qv(path, "pb", v, sizeof(v))) g_pb = atoi(v);
            if (qv(path, "d", v, sizeof(v))) g_dur = atoi(v);
            start_ping();
            send_resp(c, "application/json", "{\"ok\":true}");
        } else if (!strncmp(path, "/api/stop", 9)) {
            stop_ping();
            send_resp(c, "application/json", "{\"ok\":true}");
        } else if (!strncmp(path, "/api/status", 11)) {
            char j[49152];
            status_json(j, sizeof(j));
            send_resp(c, "application/json", j);
        } else {
            send_resp(c, "text/plain; charset=utf-8", "404");
        }

        closesocket(c);
    }

    closesocket(s);
    dbg("server stopped");
    return 0;
}

static void open_web(void) {
    wchar_t url[64];
    swprintf(url, 64, L"http://127.0.0.1:%u/", g_port);
    ShellExecuteW(NULL, L"open", url, NULL, NULL, SW_SHOWNORMAL);
}

static void tray_add(HWND h) {
    NOTIFYICONDATAA n;
    ZeroMemory(&n, sizeof(n));
    n.cbSize = sizeof(n);
    n.hWnd = h;
    n.uID = 1;
    n.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    n.uCallbackMessage = WM_TRAYICON;
    n.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    lstrcpyA(n.szTip, "TCPing GUI");
    Shell_NotifyIconA(NIM_ADD, &n);
}

static void tray_del(HWND h) {
    NOTIFYICONDATAA n;
    ZeroMemory(&n, sizeof(n));
    n.cbSize = sizeof(n);
    n.hWnd = h;
    n.uID = 1;
    Shell_NotifyIconA(NIM_DELETE, &n);
}

static void tray_menu(HWND h) {
    POINT p;
    GetCursorPos(&p);
    HMENU m = CreatePopupMenu();
    AppendMenuA(m, MF_STRING, ID_SHOW, "Show Window");
    AppendMenuA(m, MF_STRING, ID_START, "Start");
    AppendMenuA(m, MF_STRING, ID_STOP, "Stop");
    AppendMenuA(m, MF_SEPARATOR, 0, NULL);
    AppendMenuA(m, MF_STRING, ID_EXIT, "Exit");
    SetForegroundWindow(h);
    TrackPopupMenu(m, TPM_RIGHTBUTTON, p.x, p.y, 0, h, NULL);
    DestroyMenu(m);
}

static void start_server(void) {
    if (!port_ok(g_port)) {
        g_port = pick_port();
        if (!g_port) {
            MessageBoxA(NULL, "No available port.", "TCPing", MB_OK | MB_ICONERROR);
            return;
        }
    }

    g_run = 1;
    g_ready = 0;
    g_srv_th = (HANDLE)_beginthreadex(NULL, 0, server_th, NULL, 0, NULL);

    for (int i = 0; i < 50 && !g_ready; i++) Sleep(100);

    if (!g_ready) {
        MessageBoxA(NULL, "Server start failed.", "TCPing", MB_OK | MB_ICONERROR);
        g_run = 0;
        return;
    }

    open_web();
}

static void stop_server(void) {
    g_run = 0;
    stop_ping();
    if (g_srv_th) {
        WaitForSingleObject(g_srv_th, 1500);
        CloseHandle(g_srv_th);
        g_srv_th = NULL;
    }
    g_ready = 0;
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE:
        tray_add(h);
        start_server();
        return 0;
    case WM_TRAYICON:
        if (l == WM_RBUTTONUP) tray_menu(h);
        else if (l == WM_LBUTTONDBLCLK) ShowWindow(h, SW_SHOW);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case ID_SHOW: ShowWindow(h, SW_SHOW); SetForegroundWindow(h); break;
        case ID_START: start_ping(); break;
        case ID_STOP: stop_ping(); break;
        case ID_EXIT: DestroyWindow(h); break;
        }
        return 0;
    case WM_CLOSE:
        ShowWindow(h, SW_HIDE);
        return 0;
    case WM_DESTROY:
        tray_del(h);
        stop_server();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(h, m, w, l);
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE hp, PWSTR lp, int ns) {
    (void)hp;
    (void)lp;
    (void)ns;

    WSADATA w;
    if (WSAStartup(MAKEWORD(2, 2), &w)) return 1;

    g_mtx = CreateMutexA(NULL, FALSE, NULL);

    WNDCLASSA wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hi;
    wc.lpszClassName = "TCPingGUI";
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    RegisterClassA(&wc);

    g_hwnd = CreateWindowExA(
        0, "TCPingGUI", "TCPing GUI",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 800, 600,
        NULL, NULL, hi, NULL);

    if (!g_hwnd) return 1;

    ShowWindow(g_hwnd, SW_HIDE);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_mtx) CloseHandle(g_mtx);
    WSACleanup();
    return 0;
}
