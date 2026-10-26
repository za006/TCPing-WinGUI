#define _WIN32_WINNT 0x0600

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <process.h>
#include <time.h>

#pragma comment(lib, "ws2_32.lib")

#define APP_NAME        L"TCPing_GUI"
#define DEFAULT_PORT    38000
#define PORT_FALLBACK   100

#define ID_TRAY_ICON    1001
#define WM_TRAYICON     (WM_APP + 1)

#define ID_MENU_SHOW    2001
#define ID_MENU_START   2002
#define ID_MENU_STOP    2003
#define ID_MENU_EXIT    2004

#define REQ_BUF_SIZE    8192
#define LOG_BUF_SIZE    32768
#define ROW_MAX         1024

typedef struct {
    char time[32];
    double a_ms;
    double b_ms;
    int has_a;
    int has_b;
    char note[128];
} RowItem;

typedef struct {
    char target[256];
    char port[32];
} TargetCfg;

static HINSTANCE g_hInst = NULL;
static HWND g_hWnd = NULL;
static HWND g_hLogOp = NULL;
static HWND g_hLogDbg = NULL;
static HWND g_hStatus = NULL;

static NOTIFYICONDATAW g_nid = {0};
static SOCKET g_listen = INVALID_SOCKET;
static HANDLE g_http_thread = NULL;
static HANDLE g_ping_thread = NULL;
static HANDLE g_stop_event = NULL;

static volatile LONG g_running = 0;
static volatile LONG g_exit_app = 0;
static volatile LONG g_need_stop = 0;

static int g_server_port = DEFAULT_PORT;

static char g_log_op[LOG_BUF_SIZE] = {0};
static char g_log_dbg[LOG_BUF_SIZE] = {0};

static CRITICAL_SECTION g_log_cs;
static CRITICAL_SECTION g_data_cs;

static RowItem g_rows[ROW_MAX];
static int g_row_count = 0;
static char g_last_log[256] = "-";

static TargetCfg g_cfg_a = {"1.1.1.1", "80"};
static TargetCfg g_cfg_b = {"8.8.8.8", "53"};
static int g_duration_sec = 0;

static void utf8_to_wide(const char* s, wchar_t* out, int outc) {
    MultiByteToWideChar(CP_UTF8, 0, s, -1, out, outc);
}

static void wide_to_utf8(const wchar_t* s, char* out, int outc) {
    WideCharToMultiByte(CP_UTF8, 0, s, -1, out, outc, NULL, NULL);
}

static void trim(char* s) {
    size_t n;
    char* p;
    if (!s) return;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') memmove(s, s + 1, strlen(s));
    n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) s[--n] = 0;
    p = s;
    (void)p;
}

static void append_text(char* buf, size_t cap, const char* line) {
    size_t len = strlen(buf);
    size_t add = strlen(line);
    if (len + add + 1 >= cap) return;
    memcpy(buf + len, line, add);
    buf[len + add] = 0;
}

static void log_op(const char* fmt, ...) {
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    EnterCriticalSection(&g_log_cs);
    append_text(g_log_op, sizeof(g_log_op), line);
    append_text(g_log_op, sizeof(g_log_op), "\r\n");
    if (g_hLogOp) {
        wchar_t wbuf[LOG_BUF_SIZE];
        utf8_to_wide(g_log_op, wbuf, LOG_BUF_SIZE);
        SetWindowTextW(g_hLogOp, wbuf);
    }
    strncpy(g_last_log, line, sizeof(g_last_log) - 1);
    g_last_log[sizeof(g_last_log) - 1] = 0;
    LeaveCriticalSection(&g_log_cs);
}

static void log_dbg(const char* fmt, ...) {
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    EnterCriticalSection(&g_log_cs);
    append_text(g_log_dbg, sizeof(g_log_dbg), line);
    append_text(g_log_dbg, sizeof(g_log_dbg), "\r\n");
    if (g_hLogDbg) {
        wchar_t wbuf[LOG_BUF_SIZE];
        utf8_to_wide(g_log_dbg, wbuf, LOG_BUF_SIZE);
        SetWindowTextW(g_hLogDbg, wbuf);
    }
    LeaveCriticalSection(&g_log_cs);
}

static void ui_status_update(void) {
    if (!g_hStatus) return;
    char text[256];
    EnterCriticalSection(&g_data_cs);
    _snprintf(text, sizeof(text), "Running: %s | A:%s:%s | B:%s:%s | Last: %s",
              g_running ? "Yes" : "No",
              g_cfg_a.target, g_cfg_a.port,
              g_cfg_b.target, g_cfg_b.port,
              g_last_log);
    LeaveCriticalSection(&g_data_cs);
    wchar_t w[512];
    utf8_to_wide(text, w, 512);
    SendMessageW(g_hStatus, SB_SETTEXTW, 0, (LPARAM)w);
}

static void add_row(const char* t, double a_ms, int has_a, double b_ms, int has_b, const char* note) {
    EnterCriticalSection(&g_data_cs);
    if (g_row_count < ROW_MAX) {
        RowItem* r = &g_rows[g_row_count++];
        strncpy(r->time, t, sizeof(r->time) - 1);
        r->a_ms = a_ms;
        r->b_ms = b_ms;
        r->has_a = has_a;
        r->has_b = has_b;
        strncpy(r->note, note ? note : "", sizeof(r->note) - 1);
    }
    LeaveCriticalSection(&g_data_cs);
}

static void escape_json(const char* in, char* out, size_t outc) {
    size_t j = 0;
    for (size_t i = 0; in && in[i] && j + 2 < outc; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '\\' || c == '"') {
            if (j + 2 >= outc) break;
            out[j++] = '\\';
            out[j++] = c;
        } else if (c == '\r') {
            if (j + 2 >= outc) break;
            out[j++] = '\\'; out[j++] = 'r';
        } else if (c == '\n') {
            if (j + 2 >= outc) break;
            out[j++] = '\\'; out[j++] = 'n';
        } else if (c < 32) {
            out[j++] = ' ';
        } else {
            out[j++] = c;
        }
    }
    out[j] = 0;
}

static const char* MAIN_JS =
"(() => {\n"
"  const $ = (id) => document.getElementById(id);\n"
"  const state = { running:false, rows:[], lastLog:'-' };\n"
"  function esc(s){return String(s??'').replaceAll('&','&amp;').replaceAll('<','&lt;').replaceAll('>','&gt;').replaceAll('\"','&quot;').replaceAll(\"'\",'&#39;');}\n"
"  function fmt(v){ return (v==null || !isFinite(v)) ? 'Timeout' : v.toFixed(1)+' ms'; }\n"
"  function bh(v){ if(v==null||!isFinite(v)) return 8; return Math.max(8, Math.min(120, 8 + v*0.8)); }\n"
"  function bc(v){ if(v==null||!isFinite(v)) return 'bar null'; if(v<50) return 'bar a'; if(v<150) return 'bar b'; return 'bar bad'; }\n"
"  function render(){\n"
"    const tb=$('resultsBody');\n"
"    tb.innerHTML=state.rows.map(r=>`<tr><td class=time>${esc(r.time)}</td><td class=num>${esc(fmt(r.a))}</td><td class=num>${esc(fmt(r.b))}</td><td class=barcell><div class=\"${bc(r.a)}\" style=\"height:${bh(r.a)}px\"></div></td><td class=barcell><div class=\"${bc(r.b)}\" style=\"height:${bh(r.b)}px\"></div></td><td>${esc(r.note)}</td></tr>`).join('');\n"
"    $('statusRun').textContent = state.running ? 'Running' : 'Stopped';\n"
"    $('statusLast').textContent = 'Last: ' + state.lastLog;\n"
"  }\n"
"  async function g(path){ const r=await fetch(path,{cache:'no-store'}); return await r.json(); }\n"
"  async function p(path,obj){ const b=new URLSearchParams(); for(const [k,v] of Object.entries(obj)) b.set(k,v); const r=await fetch(path,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b.toString()}); return await r.json(); }\n"
"  async function refresh(){ try{ const j=await g('/api/status'); state.running=!!j.running; state.lastLog=j.last_log||'-'; state.rows=(j.rows||[]).map(x=>({time:x.time||'', a:x.a_ms!=null?Number(x.a_ms):null, b:x.b_ms!=null?Number(x.b_ms):null, note:x.note||''})); $('logOp').value=j.log_op||''; $('logDbg').value=j.log_dbg||''; render(); }catch(e){} }\n"
"  async function start(){ await p('/api/start',{target_a:$('targetA').value, port_a:$('portA').value, target_b:$('targetB').value, port_b:$('portB').value, duration:$('duration').value}); await refresh(); }\n"
"  async function stop(){ await p('/api/stop',{}); await refresh(); }\n"
"  function style(){ const s=document.createElement('style'); s.textContent=`:root{--bg:#f4f7fb;--panel:#fff;--line:#d9e1ea;--text:#1f2a37;--muted:#667085;--blue:#2f80ed;--orange:#f2994a;--shadow:0 4px 18px rgba(18,28,45,.08)}*{box-sizing:border-box}body{margin:0;font-family:Segoe UI,Arial,sans-serif;background:var(--bg);color:var(--text);font-size:13px}.wrap{max-width:1180px;margin:0 auto;padding:14px}.topbar,.panel,.statusbar{background:var(--panel);border:1px solid var(--line);border-radius:10px;box-shadow:var(--shadow)}.topbar{padding:12px;margin-bottom:12px}.row{display:flex;gap:10px;flex-wrap:wrap;align-items:end}.field{display:flex;flex-direction:column;gap:5px;min-width:140px}.field.small{min-width:92px;width:92px}label{color:var(--muted);font-size:12px}input{height:32px;padding:0 10px;border:1px solid var(--line);border-radius:8px}.panel{padding:10px 12px}.title{font-weight:600;margin:0 0 8px 0;font-size:13px}table{width:100%;border-collapse:collapse;font-size:12px}thead th{text-align:left;padding:8px 6px;border-bottom:1px solid var(--line);color:#506070;font-weight:600}tbody td{padding:7px 6px;border-bottom:1px solid #eef2f7}.num{width:110px;white-space:nowrap}.time{width:92px;white-space:nowrap}.barcell{width:72px;text-align:center}.bar{width:18px;border-radius:6px 6px 2px 2px;margin:0 auto}.bar.a{background:var(--blue)}.bar.b{background:var(--orange)}.bar.bad{background:#d9534f}.bar.null{background:#cfd8e3}textarea{width:100%;min-height:86px;resize:vertical;border:1px solid var(--line);border-radius:8px;padding:8px;background:#fbfcfe;font-family:Consolas,monospace;font-size:12px}.two{display:grid;grid-template-columns:1fr 1fr;gap:12px}.statusbar{margin-top:12px;padding:10px 12px;display:flex;gap:16px;flex-wrap:wrap;color:#334155}@media (max-width: 900px){.two{grid-template-columns:1fr}}`; document.head.appendChild(s);} \n"
"  function ui(){ document.body.innerHTML = `<div class=wrap><div class=topbar><div class=row><div class=field><label>Target A</label><input id=targetA value='1.1.1.1'></div><div class='field small'><label>Port A</label><input id=portA value='80'></div><div class=field><label>Target B</label><input id=targetB value='8.8.8.8'></div><div class='field small'><label>Port B</label><input id=portB value='53'></div><div class='field small'><label>Duration</label><input id=duration value='0'></div><button id=btnStart>Start</button><button id=btnStop>Stop</button></div></div><div class=panel><div class=title>Results</div><table><thead><tr><th class=time>Time</th><th class=num>A</th><th class=num>B</th><th class=barcell>A Bar</th><th class=barcell>B Bar</th><th>Note</th></tr></thead><tbody id=resultsBody></tbody></table></div><div class='two'><div class=panel><div class=title>Operational Log</div><textarea id=logOp readonly></textarea></div><div class=panel><div class=title>Debug Log</div><textarea id=logDbg readonly></textarea></div></div><div class=statusbar><div id=statusRun>Stopped</div><div id=statusLast>Last: -</div></div></div>`; }\n"
"  function bind(){ $('btnStart').onclick=start; $('btnStop').onclick=stop; ['targetA','portA','targetB','portB','duration'].forEach(id=>$(id).addEventListener('keydown',e=>{ if(e.key==='Enter') start(); })); }\n"
"  (async()=>{ style(); ui(); bind(); render(); await refresh(); setInterval(refresh,1000); })();\n"
"})();\n";

static const char* HTML_PAGE =
"<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>TCPing GUI</title></head><body>"
"<script src='/main.js'></script>"
"</body></html>";

static int send_all(SOCKET s, const char* data, int len) {
    int sent = 0;
    while (sent < len) {
        int n = send(s, data + sent, len - sent, 0);
        if (n == SOCKET_ERROR) return -1;
        sent += n;
    }
    return 0;
}

static void send_text(SOCKET s, const char* ctype, const char* body) {
    char head[1024];
    int blen = (int)strlen(body);
    _snprintf(head, sizeof(head),
        "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %d\r\nConnection: close\r\n\r\n",
        ctype, blen);
    send_all(s, head, (int)strlen(head));
    send_all(s, body, blen);
}

static void send_404(SOCKET s) {
    const char* body = "Not Found";
    char head[256];
    _snprintf(head, sizeof(head),
        "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\nContent-Length: %d\r\nConnection: close\r\n\r\n",
        (int)strlen(body));
    send_all(s, head, (int)strlen(head));
    send_all(s, body, (int)strlen(body));
}

static int get_content_length(const char* req) {
    const char* p = strstr(req, "Content-Length:");
    if (!p) return 0;
    p += 15;
    while (*p == ' ' || *p == '\t') p++;
    return atoi(p);
}

static void url_decode(char* s) {
    char* d = s;
    while (*s) {
        if (*s == '+') *d++ = ' ';
        else if (*s == '%' && s[1] && s[2]) {
            char hex[3] = { s[1], s[2], 0 };
            *d++ = (char)strtol(hex, NULL, 16);
            s += 2;
        } else {
            *d++ = *s;
        }
        s++;
    }
    *d = 0;
}

static void parse_pair(char* body, const char* key, char* out, int outc) {
    char* p = strstr(body, key);
    if (!p) return;
    p += strlen(key);
    if (*p != '=') return;
    p++;
    char* e = strpbrk(p, "&\r\n");
    int n = e ? (int)(e - p) : (int)strlen(p);
    if (n >= outc) n = outc - 1;
    memcpy(out, p, n);
    out[n] = 0;
    url_decode(out);
    trim(out);
}

static void parse_json_basic(const char* body, const char* key, char* out, int outc) {
    char pat[128];
    _snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char* p = strstr(body, pat);
    if (!p) return;
    p += strlen(pat);
    while (*p && *p != ':') p++;
    if (*p != ':') return;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '"') {
        p++;
        const char* e = strchr(p, '"');
        if (!e) return;
        int n = (int)(e - p);
        if (n >= outc) n = outc - 1;
        memcpy(out, p, n);
        out[n] = 0;
        trim(out);
    } else {
        const char* e = strpbrk(p, ",}\r\n");
        int n = e ? (int)(e - p) : (int)strlen(p);
        if (n >= outc) n = outc - 1;
        memcpy(out, p, n);
        out[n] = 0;
        trim(out);
    }
}

static int parse_request_body(const char* req, char* out, int outc) {
    const char* p = strstr(req, "\r\n\r\n");
    if (!p) return 0;
    p += 4;
    strncpy(out, p, outc - 1);
    out[outc - 1] = 0;
    return (int)strlen(out);
}

static void get_line_time(char* out, int outc) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    _snprintf(out, outc, "%02u:%02u:%02u", st.wHour, st.wMinute, st.wSecond);
}

static double tcp_connect_once(const char* host, const char* port, int timeout_ms, char* err, int errc) {
    struct addrinfo hints, *res = NULL, *ai = NULL;
    SOCKET s = INVALID_SOCKET;
    DWORD t0, t1;
    double result = -1.0;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    if (getaddrinfo(host, port, &hints, &res) != 0) {
        snprintf(err, errc, "DNS fail");
        return -1.0;
    }

    for (ai = res; ai; ai = ai->ai_next) {
        s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == INVALID_SOCKET) continue;

        u_long nb = 1;
        ioctlsocket(s, FIONBIO, &nb);

        t0 = GetTickCount();
        int rc = connect(s, ai->ai_addr, (int)ai->ai_addrlen);
        if (rc == SOCKET_ERROR) {
            int w = WSAGetLastError();
            if (w != WSAEWOULDBLOCK && w != WSAEINPROGRESS) {
                closesocket(s);
                s = INVALID_SOCKET;
                continue;
            }
        }

        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(s, &wfds);
        struct timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        rc = select(0, NULL, &wfds, NULL, &tv);
        if (rc > 0 && FD_ISSET(s, &wfds)) {
            int so_error = 0;
            int slen = sizeof(so_error);
            getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&so_error, &slen);
            if (so_error == 0) {
                t1 = GetTickCount();
                result = (double)(t1 - t0);
                closesocket(s);
                s = INVALID_SOCKET;
                break;
            }
        }

        closesocket(s);
        s = INVALID_SOCKET;
    }

    if (res) freeaddrinfo(res);
    if (result < 0.0 && err && errc > 0) snprintf(err, errc, "Timeout");
    return result;
}

static unsigned __stdcall ping_thread_proc(void* arg) {
    (void)arg;
    log_op("Ping started");
    EnterCriticalSection(&g_data_cs);
    g_row_count = 0;
    LeaveCriticalSection(&g_data_cs);

    while (InterlockedCompareExchange(&g_need_stop, 0, 0) == 0) {
        char timebuf[32];
        char errA[64] = {0}, errB[64] = {0};
        double a = tcp_connect_once(g_cfg_a.target, g_cfg_a.port, 1000, errA, sizeof(errA));
        double b = tcp_connect_once(g_cfg_b.target, g_cfg_b.port, 1000, errB, sizeof(errB));

        get_line_time(timebuf, sizeof(timebuf));

        if (a >= 0.0 || b >= 0.0) {
            char note[128];
            if (a >= 0.0 && b >= 0.0) {
                _snprintf(note, sizeof(note), "A OK / B OK");
                add_row(timebuf, a, 1, b, 1, note);
            } else if (a >= 0.0) {
                _snprintf(note, sizeof(note), "A OK / B %s", errB[0] ? errB : "Fail");
                add_row(timebuf, a, 1, -1.0, 0, note);
            } else {
                _snprintf(note, sizeof(note), "A %s / B OK", errA[0] ? errA : "Fail");
                add_row(timebuf, -1.0, 0, b, 1, note);
            }
        } else {
            add_row(timebuf, -1.0, 0, -1.0, 0, "Both Timeout");
        }

        ui_status_update();
        Sleep(1000);

        if (g_duration_sec > 0) {
            static DWORD start_tick = 0;
            if (start_tick == 0) start_tick = GetTickCount();
            if ((GetTickCount() - start_tick) >= (DWORD)g_duration_sec * 1000UL) {
                break;
            }
        }
    }

    InterlockedExchange(&g_running, 0);
    InterlockedExchange(&g_need_stop, 1);
    g_ping_thread = NULL;
    log_op("Ping stopped");
    ui_status_update();
    return 0;
}

static void start_ping(void) {
    if (InterlockedCompareExchange(&g_running, 1, 0) != 0) {
        log_op("Already running");
        return;
    }
    InterlockedExchange(&g_need_stop, 0);
    log_op("Start requested");
    g_ping_thread = (HANDLE)_beginthreadex(NULL, 0, ping_thread_proc, NULL, 0, NULL);
    if (!g_ping_thread) {
        InterlockedExchange(&g_running, 0);
        log_op("Failed to create ping thread");
    }
    ui_status_update();
}

static void stop_ping(void) {
    if (InterlockedCompareExchange(&g_running, 0, 1) == 0) {
        log_op("Already stopped");
        return;
    }
    InterlockedExchange(&g_need_stop, 1);
    log_op("Stop requested");
    if (g_ping_thread) {
        WaitForSingleObject(g_ping_thread, 2000);
        CloseHandle(g_ping_thread);
        g_ping_thread = NULL;
    }
    ui_status_update();
}

static DWORD WINAPI http_thread_proc(LPVOID arg) {
    (void)arg;
    char req[REQ_BUF_SIZE];
    char body[REQ_BUF_SIZE];
    for (;;) {
        if (InterlockedCompareExchange(&g_exit_app, 0, 0)) break;

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(g_listen, &rfds);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 200000;
        int rc = select(0, &rfds, NULL, NULL, &tv);
        if (rc <= 0) continue;
        if (!FD_ISSET(g_listen, &rfds)) continue;

        SOCKET cs = accept(g_listen, NULL, NULL);
        if (cs == INVALID_SOCKET) continue;

        int n = recv(cs, req, sizeof(req) - 1, 0);
        if (n <= 0) {
            closesocket(cs);
            continue;
        }
        req[n] = 0;

        char method[16] = {0};
        char path[128] = {0};
        sscanf(req, "%15s %127s", method, path);

        if (strcmp(method, "GET") == 0 && strcmp(path, "/") == 0) {
            send_text(cs, "text/html; charset=utf-8", HTML_PAGE);
        } else if (strcmp(method, "GET") == 0 && strcmp(path, "/main.js") == 0) {
            send_text(cs, "application/javascript; charset=utf-8", MAIN_JS);
        } else if (strcmp(method, "GET") == 0 && strcmp(path, "/api/status") == 0) {
            char json[65536];
            char op_esc[LOG_BUF_SIZE * 2];
            char dbg_esc[LOG_BUF_SIZE * 2];
            char last_esc[512];
            char rows[32768];
            int pos = 0, i;

            EnterCriticalSection(&g_log_cs);
            escape_json(g_log_op, op_esc, sizeof(op_esc));
            escape_json(g_log_dbg, dbg_esc, sizeof(dbg_esc));
            escape_json(g_last_log, last_esc, sizeof(last_esc));
            LeaveCriticalSection(&g_log_cs);

            EnterCriticalSection(&g_data_cs);
            pos += _snprintf(rows + pos, sizeof(rows) - pos, "[");
            for (i = 0; i < g_row_count && pos < (int)sizeof(rows) - 256; i++) {
                char note_esc[256];
                escape_json(g_rows[i].note, note_esc, sizeof(note_esc));
                pos += _snprintf(rows + pos, sizeof(rows) - pos,
                    "%s{\"time\":\"%s\",\"a_ms\":%s,\"b_ms\":%s,\"note\":\"%s\"}",
                    (i ? "," : ""),
                    g_rows[i].time,
                    g_rows[i].has_a ? "" : "null",
                    g_rows[i].has_b ? "" : "null",
                    note_esc);
                if (g_rows[i].has_a) {
                    pos -= 0;
                }
            }
            if (pos < (int)sizeof(rows) - 2) {
                if (g_row_count > 0) {
                    rows[pos - 0] = rows[pos - 0];
                }
            }
            if (g_row_count > 0) {
                for (i = 0; i < g_row_count; i++) {
                    if (i == 0) continue;
                }
            }
            if (g_row_count > 0) {
                pos = 1;
                rows[0] = '[';
                rows[1] = 0;
                for (i = 0; i < g_row_count; i++) {
                    char note_esc[256];
                    char tmp[512];
                    escape_json(g_rows[i].note, note_esc, sizeof(note_esc));
                    _snprintf(tmp, sizeof(tmp),
                        "%s{\"time\":\"%s\",\"a_ms\":%s,\"b_ms\":%s,\"note\":\"%s\"}",
                        (i ? "," : ""),
                        g_rows[i].time,
                        g_rows[i].has_a ? "" : "null",
                        g_rows[i].has_b ? "" : "null",
                        note_esc);
                    if (g_rows[i].has_a) {
                        /* nothing */
                    }
                    strcat(rows, tmp);
                }
                strcat(rows, "]");
            } else {
                strcpy(rows, "[]");
            }
            LeaveCriticalSection(&g_data_cs);

            _snprintf(json, sizeof(json),
                "{\"running\":%s,\"last_log\":\"%s\",\"log_op\":\"%s\",\"log_dbg\":\"%s\",\"rows\":%s}",
                g_running ? "true" : "false",
                last_esc, op_esc, dbg_esc, rows);
            send_text(cs, "application/json; charset=utf-8", json);
        } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/start") == 0) {
            int clen = get_content_length(req);
            body[0] = 0;
            if (clen > 0) {
                parse_request_body(req, body, sizeof(body));
            }
            if (strstr(req, "application/json")) {
                parse_json_basic(body, "target_a", g_cfg_a.target, sizeof(g_cfg_a.target));
                parse_json_basic(body, "port_a", g_cfg_a.port, sizeof(g_cfg_a.port));
                parse_json_basic(body, "target_b", g_cfg_b.target, sizeof(g_cfg_b.target));
                parse_json_basic(body, "port_b", g_cfg_b.port, sizeof(g_cfg_b.port));
                char dur[32] = {0};
                parse_json_basic(body, "duration", dur, sizeof(dur));
                g_duration_sec = atoi(dur);
            } else {
                parse_pair(body, "target_a", g_cfg_a.target, sizeof(g_cfg_a.target));
                parse_pair(body, "port_a", g_cfg_a.port, sizeof(g_cfg_a.port));
                parse_pair(body, "target_b", g_cfg_b.target, sizeof(g_cfg_b.target));
                parse_pair(body, "port_b", g_cfg_b.port, sizeof(g_cfg_b.port));
                char dur[32] = {0};
                parse_pair(body, "duration", dur, sizeof(dur));
                g_duration_sec = atoi(dur);
            }
            if (g_cfg_a.target[0] == 0) strcpy(g_cfg_a.target, "1.1.1.1");
            if (g_cfg_b.target[0] == 0) strcpy(g_cfg_b.target, "8.8.8.8");
            if (g_cfg_a.port[0] == 0) strcpy(g_cfg_a.port, "80");
            if (g_cfg_b.port[0] == 0) strcpy(g_cfg_b.port, "53");
            start_ping();
            send_text(cs, "application/json; charset=utf-8", "{\"ok\":true}");
        } else if (strcmp(method, "POST") == 0 && strcmp(path, "/api/stop") == 0) {
            stop_ping();
            send_text(cs, "application/json; charset=utf-8", "{\"ok\":true}");
        } else {
            send_404(cs);
        }

        closesocket(cs);
    }
    return 0;
}

static void tray_add(HWND hwnd) {
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = ID_TRAY_ICON;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    lstrcpyW(g_nid.szTip, L"TCPing GUI");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

static void tray_remove(void) {
    if (g_nid.cbSize) Shell_NotifyIconW(NIM_DELETE, &g_nid);
}

static void tray_menu(HWND hwnd) {
    POINT pt;
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, ID_MENU_SHOW, L"Show Window");
    AppendMenuW(menu, MF_STRING, ID_MENU_START, L"Start");
    AppendMenuW(menu, MF_STRING, ID_MENU_STOP, L"Stop");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, ID_MENU_EXIT, L"Exit");
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
    DestroyMenu(menu);
}

static void open_browser_port(int port) {
    char url[128];
    _snprintf(url, sizeof(url), "http://127.0.0.1:%d/", port);
    ShellExecuteA(NULL, "open", url, NULL, NULL, SW_SHOWNORMAL);
}

static int create_server_socket(int* out_port) {
    SOCKET s;
    struct sockaddr_in addr;
    int p;

    for (p = DEFAULT_PORT; p < DEFAULT_PORT + PORT_FALLBACK; p++) {
        s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) continue;
        BOOL reuse = TRUE;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        addr.sin_port = htons((u_short)p);
        if (bind(s, (struct sockaddr*)&addr, sizeof(addr)) == 0 && listen(s, 8) == 0) {
            *out_port = p;
            return (int)s;
        }
        closesocket(s);
    }
    return -1;
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        tray_add(hwnd);
        return 0;
    case WM_TRAYICON:
        if (lParam == WM_RBUTTONUP || lParam == WM_LBUTTONDBLCLK) {
            tray_menu(hwnd);
        }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case ID_MENU_SHOW:
            ShowWindow(hwnd, SW_SHOW);
            SetForegroundWindow(hwnd);
            break;
        case ID_MENU_START:
            start_ping();
            break;
        case ID_MENU_STOP:
            stop_ping();
            break;
        case ID_MENU_EXIT:
            DestroyWindow(hwnd);
            break;
        }
        return 0;
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED) ShowWindow(hwnd, SW_HIDE);
        return 0;
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    case WM_DESTROY:
        InterlockedExchange(&g_exit_app, 1);
        stop_ping();
        tray_remove();
        if (g_listen != INVALID_SOCKET) {
            closesocket(g_listen);
            g_listen = INVALID_SOCKET;
        }
        if (g_http_thread) {
            WaitForSingleObject(g_http_thread, 1000);
            CloseHandle(g_http_thread);
            g_http_thread = NULL;
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static HWND create_main_window(HINSTANCE hInst) {
    WNDCLASSW wc;
    HWND hwnd;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = APP_NAME;
    wc.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&wc);

    hwnd = CreateWindowExW(0, APP_NAME, L"TCPing GUI",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1000, 720,
        NULL, NULL, hInst, NULL);
    return hwnd;
}

static void build_ui(HWND hwnd) {
    int y = 10;
    HWND hA, hB, hPA, hPB, hD, hBtn1, hBtn2, hStatic;

    hA = CreateWindowW(L"STATIC", L"Target A", WS_CHILD | WS_VISIBLE, 10, y, 70, 20, hwnd, NULL, g_hInst, NULL);
    g_hWnd = hwnd;
    (void)hA;
    CreateWindowW(L"EDIT", L"1.1.1.1", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, 80, y, 150, 24, hwnd, (HMENU)101, g_hInst, NULL);
    CreateWindowW(L"STATIC", L"Port A", WS_CHILD | WS_VISIBLE, 240, y, 50, 20, hwnd, NULL, g_hInst, NULL);
    CreateWindowW(L"EDIT", L"80", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, 290, y, 70, 24, hwnd, (HMENU)102, g_hInst, NULL);
    CreateWindowW(L"STATIC", L"Target B", WS_CHILD | WS_VISIBLE, 380, y, 70, 20, hwnd, NULL, g_hInst, NULL);
    CreateWindowW(L"EDIT", L"8.8.8.8", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, 450, y, 150, 24, hwnd, (HMENU)103, g_hInst, NULL);
    CreateWindowW(L"STATIC", L"Port B", WS_CHILD | WS_VISIBLE, 620, y, 50, 20, hwnd, NULL, g_hInst, NULL);
    CreateWindowW(L"EDIT", L"53", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, 670, y, 70, 24, hwnd, (HMENU)104, g_hInst, NULL);
    CreateWindowW(L"STATIC", L"Duration", WS_CHILD | WS_VISIBLE, 760, y, 60, 20, hwnd, NULL, g_hInst, NULL);
    CreateWindowW(L"EDIT", L"0", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, 820, y, 70, 24, hwnd, (HMENU)105, g_hInst, NULL);

    y += 34;
    hBtn1 = CreateWindowW(L"BUTTON", L"Start", WS_CHILD | WS_VISIBLE, 10, y, 100, 28, hwnd, (HMENU)201, g_hInst, NULL);
    hBtn2 = CreateWindowW(L"BUTTON", L"Stop", WS_CHILD | WS_VISIBLE, 120, y, 100, 28, hwnd, (HMENU)202, g_hInst, NULL);
    (void)hBtn1; (void)hBtn2;

    y += 40;
    CreateWindowW(L"STATIC", L"Operational Log", WS_CHILD | WS_VISIBLE, 10, y, 200, 20, hwnd, NULL, g_hInst, NULL);
    CreateWindowW(L"STATIC", L"Debug Log", WS_CHILD | WS_VISIBLE, 500, y, 200, 20, hwnd, NULL, g_hInst, NULL);

    y += 20;
    g_hLogOp = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
        10, y, 460, 180, hwnd, NULL, g_hInst, NULL);
    g_hLogDbg = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
        500, y, 470, 180, hwnd, NULL, g_hInst, NULL);

    y += 200;
    CreateWindowW(L"STATIC", L"Results", WS_CHILD | WS_VISIBLE, 10, y, 100, 20, hwnd, NULL, g_hInst, NULL);
    y += 20;
    hStatic = CreateWindowExW(WS_EX_CLIENTEDGE, L"STATIC", L"",
        WS_CHILD | WS_VISIBLE,
        10, y, 960, 280, hwnd, NULL, g_hInst, NULL);
    (void)hStatic;

    g_hStatus = CreateWindowExW(0, STATUSCLASSNAMEW, NULL,
        WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, NULL, g_hInst, NULL);

    ui_status_update();
}

static void bind_controls(HWND hwnd) {
    (void)hwnd;
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR lpCmd, int nShow) {
    WSADATA wsa;
    int sret;
    HWND hwnd;
    MSG msg;
    INITCOMMONCONTROLSEX icc;

    (void)hPrev;
    (void)lpCmd;

    g_hInst = hInst;
    InitializeCriticalSection(&g_log_cs);
    InitializeCriticalSection(&g_data_cs);

    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES | ICC_BAR_CLASSES;
    InitCommonControlsEx(&icc);

    sret = WSAStartup(MAKEWORD(2, 2), &wsa);
    if (sret != 0) return 0;

    hwnd = create_main_window(hInst);
    if (!hwnd) return 0;

    g_hWnd = hwnd;
    build_ui(hwnd);
    bind_controls(hwnd);
    ShowWindow(hwnd, nShow);
    UpdateWindow(hwnd);

    g_listen = (SOCKET)create_server_socket(&g_server_port);
    if (g_listen == INVALID_SOCKET) {
        MessageBoxW(hwnd, L"Failed to bind 127.0.0.1:38000-38099", L"TCPing GUI", MB_ICONERROR);
        DestroyWindow(hwnd);
        WSACleanup();
        DeleteCriticalSection(&g_log_cs);
        DeleteCriticalSection(&g_data_cs);
        return 0;
    }

    g_http_thread = CreateThread(NULL, 0, http_thread_proc, NULL, 0, NULL);
    log_op("Server listening on 127.0.0.1:%d", g_server_port);
    open_browser_port(g_server_port);

    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_http_thread) {
        WaitForSingleObject(g_http_thread, 1000);
        CloseHandle(g_http_thread);
        g_http_thread = NULL;
    }

    if (g_listen != INVALID_SOCKET) {
        closesocket(g_listen);
        g_listen = INVALID_SOCKET;
    }

    WSACleanup();
    DeleteCriticalSection(&g_log_cs);
    DeleteCriticalSection(&g_data_cs);
    return 0;
}
