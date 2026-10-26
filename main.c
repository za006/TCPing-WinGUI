#define _WIN32_WINNT 0x0600

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <shellapi.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <process.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "shell32.lib")

#define APP_NAME        L"TCPing GUI"
#define CLASS_NAME      L"TCPingGuiHiddenWnd"
#define WM_TRAY         (WM_APP + 1)
#define WM_APP_EXIT     (WM_APP + 2)

#define MAX_LOG         4096
#define MAX_ROWS        512
#define MAX_NAME        256
#define MAX_NOTE        128
#define SERVER_PORT_MIN 38000
#define SERVER_PORT_MAX 38099

typedef struct {
    char time[32];
    double a_ms;
    double b_ms;
    char note[MAX_NOTE];
    int valid;
} Row;

typedef struct {
    volatile LONG running;
    volatile LONG stop_request;
    HANDLE server_thread;
    HANDLE ping_thread;
    HANDLE watchdog_thread;
    SOCKET listen_sock;
    int server_port;
    char target_a[MAX_NAME];
    char target_b[MAX_NAME];
    char port_a[16];
    char port_b[16];
    char duration[16];
    char op_log[MAX_LOG];
    char dbg_log[MAX_LOG];
    CRITICAL_SECTION lock;
    Row rows[MAX_ROWS];
    int row_count;
    SYSTEMTIME started_at;
    volatile LONG ui_visible;
} AppState;

static AppState g;

static HWND g_hwnd = NULL;
static HINSTANCE g_hInst = NULL;
static NOTIFYICONDATAW g_nid;

static const char *g_main_js =
"let timer=null;\n"
"function qs(id){return document.getElementById(id)}\n"
"function logBox(id,msg){const e=qs(id); e.value += msg + '\\n'; e.scrollTop=e.scrollHeight;}\n"
"function setStatus(s){qs('status').textContent=s;}\n"
"function render(data){\n"
"  qs('runState').textContent=data.running ? 'Running' : 'Stopped';\n"
"  qs('statA').textContent='A: '+data.stat_a;\n"
"  qs('statB').textContent='B: '+data.stat_b;\n"
"  qs('lastLog').textContent=data.last_log || '';\n"
"  qs('oplog').value=data.op_log || '';\n"
"  qs('dbglog').value=data.dbg_log || '';\n"
"  const tb=qs('tbody'); tb.innerHTML='';\n"
"  (data.rows||[]).forEach(r=>{\n"
"    const tr=document.createElement('tr');\n"
"    tr.innerHTML=`<td>${r.time}</td><td>${r.a_ms}</td><td>${r.b_ms}</td>\n"
"      <td><div class='bar a' style='height:${r.a_h}px'></div></td>\n"
"      <td><div class='bar b' style='height:${r.b_h}px'></div></td>\n"
"      <td>${r.note||''}</td>`;\n"
"    tb.appendChild(tr);\n"
"  });\n"
"}\n"
"async function refresh(){\n"
"  try{ const r=await fetch('/api/status'); render(await r.json()); }\n"
"  catch(e){ setStatus('Offline'); }\n"
"}\n"
"async function startPing(){\n"
"  const body={\n"
"    target_a:qs('targetA').value.trim(), port_a:qs('portA').value.trim(),\n"
"    target_b:qs('targetB').value.trim(), port_b:qs('portB').value.trim(),\n"
"    duration:qs('duration').value.trim()\n"
"  };\n"
"  await fetch('/api/start',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});\n"
"  refresh();\n"
"}\n"
"async function stopPing(){ await fetch('/api/stop',{method:'POST'}); refresh(); }\n"
"window.onload=()=>{ refresh(); timer=setInterval(refresh,1000); };\n";

static void dbg_logf(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    EnterCriticalSection(&g.lock);
    size_t len = strlen(g.dbg_log);
    size_t bl = strlen(buf);
    if (len + bl + 2 >= MAX_LOG) {
        memmove(g.dbg_log, g.dbg_log + MAX_LOG / 2, MAX_LOG / 2);
        g.dbg_log[MAX_LOG / 2] = 0;
        len = strlen(g.dbg_log);
    }
    strcat(g.dbg_log, buf);
    strcat(g.dbg_log, "\r\n");
    LeaveCriticalSection(&g.lock);
}

static void op_logf(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    EnterCriticalSection(&g.lock);
    size_t len = strlen(g.op_log);
    size_t bl = strlen(buf);
    if (len + bl + 2 >= MAX_LOG) {
        memmove(g.op_log, g.op_log + MAX_LOG / 2, MAX_LOG / 2);
        g.op_log[MAX_LOG / 2] = 0;
        len = strlen(g.op_log);
    }
    strcat(g.op_log, buf);
    strcat(g.op_log, "\r\n");
    LeaveCriticalSection(&g.lock);
}

static void set_last_row(double a_ms, double b_ms, const char *note) {
    SYSTEMTIME st;
    GetLocalTime(&st);

    EnterCriticalSection(&g.lock);
    if (g.row_count >= MAX_ROWS) {
        memmove(&g.rows[0], &g.rows[1], sizeof(Row) * (MAX_ROWS - 1));
        g.row_count = MAX_ROWS - 1;
    }
    Row *r = &g.rows[g.row_count++];
    snprintf(r->time, sizeof(r->time), "%02d:%02d:%02d.%03d",
             st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    r->a_ms = a_ms;
    r->b_ms = b_ms;
    strncpy(r->note, note ? note : "", sizeof(r->note) - 1);
    r->note[sizeof(r->note) - 1] = 0;
    r->valid = 1;
    LeaveCriticalSection(&g.lock);
}

static const char *find_header_value(const char *headers, const char *key) {
    static char val[128];
    val[0] = 0;
    const char *p = strstr(headers, key);
    if (!p) return NULL;
    p += strlen(key);
    while (*p == ' ' || *p == ':') p++;
    const char *e = strstr(p, "\r\n");
    if (!e) return NULL;
    size_t n = (size_t)(e - p);
    if (n >= sizeof(val)) n = sizeof(val) - 1;
    memcpy(val, p, n);
    val[n] = 0;
    return val;
}

static void url_decode(char *s) {
    char *o = s;
    while (*s) {
        if (*s == '%' && s[1] && s[2]) {
            char hex[3] = {s[1], s[2], 0};
            *o++ = (char)strtol(hex, NULL, 16);
            s += 3;
        } else if (*s == '+') {
            *o++ = ' ';
            s++;
        } else {
            *o++ = *s++;
        }
    }
    *o = 0;
}

static void parse_kv(char *body, const char *key, char *out, size_t outsz) {
    char *p = strstr(body, key);
    if (!p) return;
    p += strlen(key);
    if (*p == '=') p++;
    char *e = strpbrk(p, "&}");
    size_t n = e ? (size_t)(e - p) : strlen(p);
    if (n >= outsz) n = outsz - 1;
    memcpy(out, p, n);
    out[n] = 0;
    url_decode(out);
}

static void parse_jsonish(char *body, const char *key, char *out, size_t outsz) {
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    char *p = strstr(body, needle);
    if (!p) return;
    p += strlen(needle);
    while (*p && (*p == ' ' || *p == ':' || *p == '"' )) p++;
    char *e = p;
    while (*e && *e != '"' && *e != ',' && *e != '}') e++;
    size_t n = (size_t)(e - p);
    if (n >= outsz) n = outsz - 1;
    memcpy(out, p, n);
    out[n] = 0;
}

static void extract_field(char *body, const char *key, char *out, size_t outsz) {
    out[0] = 0;
    if (strchr(body, '=')) parse_kv(body, key, out, outsz);
    if (!out[0]) parse_jsonish(body, key, out, outsz);
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

static void send_http(SOCKET cs, const char *ctype, const char *body) {
    char hdr[256];
    int bl = (int)strlen(body);
    snprintf(hdr, sizeof(hdr),
             "HTTP/1.1 200 OK\r\n"
             "Content-Type: %s\r\n"
             "Content-Length: %d\r\n"
             "Connection: close\r\n\r\n",
             ctype, bl);
    send_all(cs, hdr, (int)strlen(hdr));
    send_all(cs, body, bl);
}

static void send_json(SOCKET cs, const char *body) {
    send_http(cs, "application/json; charset=utf-8", body);
}

static void send_html(SOCKET cs, const char *body) {
    send_http(cs, "text/html; charset=utf-8", body);
}

static void send_js(SOCKET cs, const char *body) {
    send_http(cs, "application/javascript; charset=utf-8", body);
}

static int tcp_ping_one(const char *host, const char *port, double *ms_out) {
    struct addrinfo hints, *res = NULL, *it = NULL;
    SOCKET s = INVALID_SOCKET;
    char msbuf[64];
    LARGE_INTEGER f, t1, t2;
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    hints.ai_protocol = IPPROTO_TCP;

    if (getaddrinfo(host, port, &hints, &res) != 0) return -1;

    for (it = res; it; it = it->ai_next) {
        s = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (s == INVALID_SOCKET) continue;

        u_long mode = 1;
        ioctlsocket(s, FIONBIO, &mode);
        connect(s, it->ai_addr, (int)it->ai_addrlen);

        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(s, &wfds);

        struct timeval tv;
        tv.tv_sec = 2;
        tv.tv_usec = 0;

        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t1);
        int r = select(0, NULL, &wfds, NULL, &tv);
        QueryPerformanceCounter(&t2);

        if (r > 0 && FD_ISSET(s, &wfds)) {
            int soerr = 0, slen = sizeof(soerr);
            getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&soerr, &slen);
            closesocket(s);
            if (soerr == 0) {
                double ms = (double)(t2.QuadPart - t1.QuadPart) * 1000.0 / (double)f.QuadPart;
                *ms_out = ms;
                return 0;
            }
        }
        closesocket(s);
        s = INVALID_SOCKET;
    }
    if (res) freeaddrinfo(res);
    return -1;
}

static void compute_note(double a, double b, char *note, size_t n) {
    if (a < 0 && b < 0) snprintf(note, n, "A/B timeout");
    else if (a < 0) snprintf(note, n, "A timeout");
    else if (b < 0) snprintf(note, n, "B timeout");
    else if (a <= b) snprintf(note, n, "A faster");
    else snprintf(note, n, "B faster");
}

static void update_title_running(int running) {
    wchar_t title[128];
    swprintf(title, 128, L"%s - %s", APP_NAME, running ? L"Running" : L"Stopped");
    SetWindowTextW(g_hwnd, title);
}

static void tray_add(HWND hwnd) {
    memset(&g_nid, 0, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wcscpy(g_nid.szTip, L"TCPing GUI");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

static void tray_del(void) {
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
}

static void tray_menu(HWND hwnd) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, 1001, L"Show Window");
    AppendMenuW(m, MF_STRING, 1002, L"Start");
    AppendMenuW(m, MF_STRING, 1003, L"Stop");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, 1004, L"Exit");
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
    DestroyMenu(m);
    if (cmd == 1001) ShowWindow(hwnd, SW_SHOW), SetForegroundWindow(hwnd);
    else if (cmd == 1002) PostMessageW(hwnd, WM_COMMAND, 2001, 0);
    else if (cmd == 1003) PostMessageW(hwnd, WM_COMMAND, 2002, 0);
    else if (cmd == 1004) PostMessageW(hwnd, WM_COMMAND, 2003, 0);
}

static void browser_open(int port) {
    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/", port);
    ShellExecuteA(NULL, "open", url, NULL, NULL, SW_SHOWNORMAL);
}

static int server_send_root(SOCKET cs) {
    const char *body =
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>TCPing GUI</title>"
        "<style>"
        "body{font-family:Segoe UI,Arial,sans-serif;background:#f7f9fc;color:#1b1f24;margin:12px}"
        ".row{display:flex;gap:8px;flex-wrap:wrap;align-items:center;margin-bottom:8px}"
        "input,button,textarea{font:14px Segoe UI;padding:6px 8px;border:1px solid #cfd7e3;border-radius:6px;background:#fff}"
        "button{background:#eaf2ff;cursor:pointer}"
        "table{width:100%;border-collapse:collapse;font-size:13px;background:#fff}"
        "th,td{border:1px solid #dde4ee;padding:5px;text-align:center;vertical-align:middle}"
        "th{background:#eef4fb}"
        ".barbox{width:28px;height:42px;margin:auto;display:flex;align-items:flex-end;justify-content:center;background:#f7f9fc;border-radius:3px}"
        ".bar{width:12px;border-radius:3px 3px 0 0}"
        ".a{background:#3b82f6}.b{background:#f59e0b}"
        ".panel{display:grid;grid-template-columns:1fr 1fr;gap:8px;margin-top:8px}"
        "textarea{width:100%;height:120px;resize:vertical}"
        ".status{margin-top:8px;padding:8px;background:#eef4fb;border:1px solid #d6deea;border-radius:6px;font-size:13px}"
        "</style></head><body>"
        "<div class='row'>"
        "<input id='targetA' placeholder='Target A' value='1.1.1.1'>"
        "<input id='portA' placeholder='Port A' value='443' style='width:80px'>"
        "<input id='targetB' placeholder='Target B' value='8.8.8.8'>"
        "<input id='portB' placeholder='Port B' value='53' style='width:80px'>"
        "<input id='duration' placeholder='Duration (sec)' value='60' style='width:110px'>"
        "<button onclick='startPing()'>Start</button>"
        "<button onclick='stopPing()'>Stop</button>"
        "</div>"
        "<table><thead><tr><th>Time</th><th>A</th><th>B</th><th>A Bar</th><th>B Bar</th><th>Note</th></tr></thead>"
        "<tbody id='tbody'></tbody></table>"
        "<div class='panel'>"
        "<div><div>Operational Log</div><textarea id='oplog' readonly></textarea></div>"
        "<div><div>Debug Log</div><textarea id='dbglog' readonly></textarea></div>"
        "</div>"
        "<div class='status'>"
        "Status: <span id='runState'>Stopped</span> | "
        "<span id='statA'>A:</span> | <span id='statB'>B:</span> | "
        "Last: <span id='lastLog'></span>"
        "</div>"
        "<script src='/main.js'></script>"
        "</body></html>";
    send_html(cs, body);
    return 1;
}

static int handle_request(SOCKET cs, const char *req) {
    char method[8] = {0}, path[128] = {0};
    sscanf(req, "%7s %127s", method, path);

    const char *headers_end = strstr(req, "\r\n\r\n");
    const char *body = headers_end ? headers_end + 4 : "";
    char bodybuf[1024] = {0};
    strncpy(bodybuf, body, sizeof(bodybuf) - 1);

    if (!strcmp(method, "GET") && !strcmp(path, "/")) return server_send_root(cs);
    if (!strcmp(method, "GET") && !strcmp(path, "/main.js")) { send_js(cs, g_main_js); return 1; }

    if (!strcmp(method, "GET") && !strcmp(path, "/api/status")) {
        char json[8192];
        EnterCriticalSection(&g.lock);
        int running = g.running ? 1 : 0;
        char rows[6144] = {0};
        char one[256];
        for (int i = 0; i < g.row_count; i++) {
            Row *r = &g.rows[i];
            double a = r->a_ms < 0 ? 0 : r->a_ms;
            double b = r->b_ms < 0 ? 0 : r->b_ms;
            int ah = (int)(a > 200 ? 42 : (a * 42.0 / 200.0));
            int bh = (int)(b > 200 ? 42 : (b * 42.0 / 200.0));
            if (ah < 2 && r->a_ms >= 0) ah = 2;
            if (bh < 2 && r->b_ms >= 0) bh = 2;
            snprintf(one, sizeof(one),
                "{\"time\":\"%s\",\"a_ms\":\"%.2f\",\"b_ms\":\"%.2f\",\"a_h\":%d,\"b_h\":%d,\"note\":\"%s\"}%s",
                r->time, r->a_ms, r->b_ms, ah, bh, r->note,
                (i + 1 < g.row_count) ? "," : "");
            strcat(rows, one);
        }
        const char *last = g.row_count ? g.rows[g.row_count - 1].note : "";
        snprintf(json, sizeof(json),
                 "{\"running\":%s,\"stat_a\":\"%s:%s\",\"stat_b\":\"%s:%s\",\"last_log\":\"%s\","
                 "\"op_log\":\"%s\",\"dbg_log\":\"%s\",\"rows\":[%s]}",
                 running ? "true" : "false",
                 g.target_a, g.port_a, g.target_b, g.port_b,
                 last, g.op_log, g.dbg_log, rows);
        LeaveCriticalSection(&g.lock);
        send_json(cs, json);
        return 1;
    }

    if (!strcmp(method, "POST") && !strcmp(path, "/api/start")) {
        extract_field(bodybuf, "target_a", g.target_a, sizeof(g.target_a));
        extract_field(bodybuf, "port_a", g.port_a, sizeof(g.port_a));
        extract_field(bodybuf, "target_b", g.target_b, sizeof(g.target_b));
        extract_field(bodybuf, "port_b", g.port_b, sizeof(g.port_b));
        extract_field(bodybuf, "duration", g.duration, sizeof(g.duration));
        if (!g.target_a[0]) strcpy(g.target_a, "1.1.1.1");
        if (!g.port_a[0]) strcpy(g.port_a, "443");
        if (!g.target_b[0]) strcpy(g.target_b, "8.8.8.8");
        if (!g.port_b[0]) strcpy(g.port_b, "53");
        if (!g.duration[0]) strcpy(g.duration, "60");

        InterlockedExchange(&g.stop_request, 0);
        if (!InterlockedCompareExchange(&g.running, 1, 0)) {
            EnterCriticalSection(&g.lock);
            g.row_count = 0;
            g.op_log[0] = 0;
            g.dbg_log[0] = 0;
            LeaveCriticalSection(&g.lock);
            GetLocalTime(&g.started_at);
            g.ping_thread = (HANDLE)_beginthreadex(NULL, 0, (unsigned (__stdcall *)(void *))[](void *p)->unsigned {
                (void)p;
                int duration = atoi(g.duration);
                if (duration <= 0) duration = 60;
                op_logf("Start: %s:%s vs %s:%s for %d sec", g.target_a, g.port_a, g.target_b, g.port_b, duration);
                dbg_logf("ping thread started");
                for (int i = 0; i < duration; i++) {
                    if (InterlockedCompareExchange(&g.stop_request, 0, 0)) break;
                    double a = -1, b = -1;
                    int ra = tcp_ping_one(g.target_a, g.port_a, &a);
                    int rb = tcp_ping_one(g.target_b, g.port_b, &b);
                    char note[64];
                    compute_note(ra == 0 ? a : -1, rb == 0 ? b : -1, note, sizeof(note));
                    set_last_row(ra == 0 ? a : -1, rb == 0 ? b : -1, note);
                    dbg_logf("tick %d: A=%s B=%s", i + 1, ra == 0 ? "ok" : "fail", rb == 0 ? "ok" : "fail");
                    Sleep(1000);
                }
                op_logf("Ping finished");
                InterlockedExchange(&g.running, 0);
                g.ping_thread = NULL;
                PostMessageW(g_hwnd, WM_APP_EXIT, 0, 0);
                return 0;
            }, NULL, 0, NULL);
            update_title_running(1);
            send_json(cs, "{\"ok\":true}");
            return 1;
        }
        send_json(cs, "{\"ok\":false,\"msg\":\"already running\"}");
        return 1;
    }

    if (!strcmp(method, "POST") && !strcmp(path, "/api/stop")) {
        InterlockedExchange(&g.stop_request, 1);
        if (g.ping_thread) WaitForSingleObject(g.ping_thread, 3000);
        InterlockedExchange(&g.running, 0);
        g.ping_thread = NULL;
        op_logf("Stopped");
        update_title_running(0);
        send_json(cs, "{\"ok\":true}");
        return 1;
    }

    send_http(cs, "text/plain; charset=utf-8", "404");
    return 1;
}

static unsigned __stdcall server_thread(void *p) {
    (void)p;
    SOCKET ls = INVALID_SOCKET;
    char req[8192];

    for (int port = SERVER_PORT_MIN; port <= SERVER_PORT_MAX; port++) {
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) continue;

        BOOL yes = TRUE;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons((u_short)port);

        if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) == 0 && listen(s, 8) == 0) {
            g.listen_sock = s;
            g.server_port = port;
            dbg_logf("server bound at 127.0.0.1:%d", port);
            op_logf("Web server started at http://127.0.0.1:%d", port);
            browser_open(port);
            ls = s;
            break;
        }
        closesocket(s);
    }

    if (ls == INVALID_SOCKET) {
        op_logf("bind failed on 38000-38099");
        PostMessageW(g_hwnd, WM_APP_EXIT, 0, 0);
        return 0;
    }

    while (!InterlockedCompareExchange(&g.stop_request, 0, 0)) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(ls, &rfds);
        struct timeval tv = {1, 0};
        int r = select(0, &rfds, NULL, NULL, &tv);
        if (r > 0 && FD_ISSET(ls, &rfds)) {
            SOCKET cs = accept(ls, NULL, NULL);
            if (cs != INVALID_SOCKET) {
                int n = recv(cs, req, sizeof(req) - 1, 0);
                if (n > 0) {
                    req[n] = 0;
                    handle_request(cs, req);
                }
                closesocket(cs);
            }
        }
    }

    if (ls != INVALID_SOCKET) closesocket(ls);
    g.listen_sock = INVALID_SOCKET;
    dbg_logf("server thread exit");
    return 0;
}

static unsigned __stdcall watchdog_thread(void *p) {
    (void)p;
    HANDLE th = g.server_thread;
    while (!InterlockedCompareExchange(&g.stop_request, 0, 0)) {
        if (th && WaitForSingleObject(th, 0) == WAIT_OBJECT_0) {
            op_logf("Server exited unexpectedly, auto quitting");
            PostMessageW(g_hwnd, WM_APP_EXIT, 0, 0);
            break;
        }
        if (g.ping_thread && WaitForSingleObject(g.ping_thread, 0) == WAIT_OBJECT_0) {
            op_logf("Ping worker exited, auto quitting");
            PostMessageW(g_hwnd, WM_APP_EXIT, 0, 0);
            break;
        }
        Sleep(1000);
    }
    return 0;
}

static void stop_all(void) {
    InterlockedExchange(&g.stop_request, 1);

    if (g.listen_sock != INVALID_SOCKET) {
        shutdown(g.listen_sock, SD_BOTH);
        closesocket(g.listen_sock);
        g.listen_sock = INVALID_SOCKET;
    }

    if (g.ping_thread) {
        WaitForSingleObject(g.ping_thread, 3000);
        CloseHandle(g.ping_thread);
        g.ping_thread = NULL;
    }

    if (g.server_thread) {
        WaitForSingleObject(g.server_thread, 3000);
        CloseHandle(g.server_thread);
        g.server_thread = NULL;
    }

    if (g.watchdog_thread) {
        WaitForSingleObject(g.watchdog_thread, 2000);
        CloseHandle(g.watchdog_thread);
        g.watchdog_thread = NULL;
    }

    InterlockedExchange(&g.running, 0);
    update_title_running(0);
}

static void start_server(void) {
    InterlockedExchange(&g.stop_request, 0);
    g.server_thread = (HANDLE)_beginthreadex(NULL, 0, server_thread, NULL, 0, NULL);
    g.watchdog_thread = (HANDLE)_beginthreadex(NULL, 0, watchdog_thread, NULL, 0, NULL);
}

static void show_main_window(void) {
    ShowWindow(g_hwnd, SW_SHOW);
    ShowWindow(g_hwnd, SW_RESTORE);
    SetForegroundWindow(g_hwnd);
    InterlockedExchange(&g.ui_visible, 1);
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        tray_add(hwnd);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case 2001: show_main_window(); break;
        case 2002:
            if (!g.running) {
                if (g.server_thread == NULL) start_server();
                else op_logf("server already started");
            }
            break;
        case 2003:
            stop_all();
            PostQuitMessage(0);
            break;
        }
        return 0;
    case WM_TRAY:
        if (lParam == WM_RBUTTONUP) tray_menu(hwnd);
        else if (lParam == WM_LBUTTONDBLCLK) show_main_window();
        return 0;
    case WM_APP_EXIT:
        stop_all();
        tray_del();
        DestroyWindow(hwnd);
        return 0;
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        InterlockedExchange(&g.ui_visible, 0);
        return 0;
    case WM_DESTROY:
        tray_del();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE hPrev, LPWSTR lpCmdLine, int nCmdShow) {
    (void)hPrev; (void)lpCmdLine; (void)nCmdShow;
    g_hInst = hInstance;
    InitializeCriticalSection(&g.lock);

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) return 1;

    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = wndproc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&wc);

    g_hwnd = CreateWindowExW(0, CLASS_NAME, APP_NAME,
                             WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT, 900, 700,
                             NULL, NULL, hInstance, NULL);

    if (!g_hwnd) {
        WSACleanup();
        DeleteCriticalSection(&g.lock);
        return 1;
    }

    ShowWindow(g_hwnd, SW_HIDE);
    UpdateWindow(g_hwnd);
    InterlockedExchange(&g.ui_visible, 0);

    op_logf("App started");
    dbg_logf("wWinMain entered");

    start_server();

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    stop_all();
    DeleteCriticalSection(&g.lock);
    WSACleanup();
    return 0;
}
