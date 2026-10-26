#define _WIN32_WINNT 0x0600

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <commctrl.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#define APP_CLASS L"TCPingGUIClass"
#define APP_TITLE L"TCPing GUI"
#define ID_TRAYICON 1001
#define WM_TRAYICON (WM_APP + 1)

#define SERVER_PORT_BASE 38000
#define SERVER_PORT_MAX 38099

#define MAX_LOG 32768
#define MAX_ROWS 64

static HWND g_hwnd = NULL;
static HWND g_editAHost = NULL, g_editAPort = NULL, g_editBHost = NULL, g_editBPort = NULL, g_editDur = NULL;
static HWND g_btnStart = NULL, g_btnStop = NULL, g_btnShow = NULL, g_status = NULL;
static HWND g_list = NULL;
static HWND g_hLogOp = NULL, g_hLogDbg = NULL;

static HANDLE g_server_thread = NULL;
static HANDLE g_ping_thread = NULL;
static volatile LONG g_running = 0;
static volatile LONG g_should_stop = 0;
static SOCKET g_listen_sock = INVALID_SOCKET;
static UINT g_server_port = 0;

static CRITICAL_SECTION g_log_cs;
static CRITICAL_SECTION g_data_cs;

static wchar_t g_log_op[MAX_LOG] = L"";
static wchar_t g_log_dbg[MAX_LOG] = L"";

static wchar_t g_a_host[128] = L"8.8.8.8";
static wchar_t g_b_host[128] = L"1.1.1.1";
static wchar_t g_a_port[16] = L"53";
static wchar_t g_b_port[16] = L"53";
static int g_duration = 30;

static long long g_a_ok = 0, g_a_fail = 0, g_b_ok = 0, g_b_fail = 0;
static double g_a_last = -1.0, g_b_last = -1.0;
static wchar_t g_last_note[256] = L"Idle";

typedef struct ResultRow {
    wchar_t time[32];
    wchar_t a[32];
    wchar_t b[32];
    wchar_t abar[64];
    wchar_t bbar[64];
    wchar_t note[128];
} ResultRow;

static ResultRow g_rows[MAX_ROWS];
static int g_row_count = 0;

static const char g_main_js[] =
"const $=s=>document.querySelector(s);\n"
"async function j(u,o){const r=await fetch(u,o);return await r.json();}\n"
"function bar(v,c){const h=Math.max(4,Math.min(34,Math.round(v||0)));return `<div class='bars'><div class='bar ${c}' style='height:${h}px'></div></div>`;}\n"
"function row(r){const tr=document.createElement('tr');\n"
"tr.innerHTML=`<td>${r.time||''}</td><td>${r.a||''}</td><td>${r.b||''}</td><td>${bar(r.ah,'a')}</td><td>${bar(r.bh,'b')}</td><td>${r.note||''}</td>`;return tr;}\n"
"async function refresh(){const s=await j('/api/status');\n"
"$('#state').textContent=s.running?'Running':'Stopped';\n"
"$('#stats').textContent=`A ${s.a.ok}/${s.a.fail} last ${s.a.last_ms} ms | B ${s.b.ok}/${s.b.fail} last ${s.b.last_ms} ms`;\n"
"$('#lastlog').textContent=s.last_log||'';\n"
"const tb=$('#tbody');tb.innerHTML='';(s.rows||[]).forEach(x=>tb.appendChild(row(x)));\n"
"$('#oplog').value=s.op_log||'';$('#dbglog').value=s.dbg_log||'';}\n"
"async function start(){const o={a:$('#a').value,ap:$('#ap').value,b:$('#b').value,bp:$('#bp').value,d:$('#d').value};await j('/api/start',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(o)});refresh();}\n"
"async function stop(){await j('/api/stop',{method:'POST'});refresh();}\n"
"window.onload=()=>{$('#start').onclick=start;$('#stop').onclick=stop;setInterval(refresh,1000);refresh();};\n";

static void AppendToBuffer(wchar_t* dst, size_t dstsz, const wchar_t* src) {
    size_t cur = wcslen(dst);
    size_t add = wcslen(src);
    if (cur + add + 1 >= dstsz) add = dstsz - cur - 1;
    wcsncat(dst, src, add);
}

static void LogOp(const wchar_t* fmt, ...) {
    wchar_t buf[1024];
    wchar_t line[1200];
    va_list ap;
    va_start(ap, fmt);
    vswprintf(buf, 1024, fmt, ap);
    va_end(ap);

    SYSTEMTIME st;
    GetLocalTime(&st);
    swprintf(line, 1200, L"[%02d:%02d:%02d] %s\r\n", st.wHour, st.wMinute, st.wSecond, buf);

    EnterCriticalSection(&g_log_cs);
    AppendToBuffer(g_log_op, MAX_LOG, line);
    LeaveCriticalSection(&g_log_cs);

    if (g_hLogOp) SetWindowTextW(g_hLogOp, g_log_op);
}

static void LogDbg(const wchar_t* fmt, ...) {
    wchar_t buf[1024];
    wchar_t line[1200];
    va_list ap;
    va_start(ap, fmt);
    vswprintf(buf, 1024, fmt, ap);
    va_end(ap);

    SYSTEMTIME st;
    GetLocalTime(&st);
    swprintf(line, 1200, L"[%02d:%02d:%02d] %s\r\n", st.wHour, st.wMinute, st.wSecond, buf);

    EnterCriticalSection(&g_log_cs);
    AppendToBuffer(g_log_dbg, MAX_LOG, line);
    LeaveCriticalSection(&g_log_cs);

    if (g_hLogDbg) SetWindowTextW(g_hLogDbg, g_log_dbg);
}

static void SetStatusText(void) {
    wchar_t buf[512];
    EnterCriticalSection(&g_data_cs);
    swprintf(buf, 512, L"%s | A ok/fail %lld/%lld last %.1f ms | B ok/fail %lld/%lld last %.1f ms | %s",
             g_running ? L"Running" : L"Stopped",
             g_a_ok, g_a_fail, g_a_last,
             g_b_ok, g_b_fail, g_b_last,
             g_last_note);
    LeaveCriticalSection(&g_data_cs);
    if (g_status) SetWindowTextW(g_status, buf);
}

static void AddResultRow(const wchar_t* a, const wchar_t* b, const wchar_t* abar, const wchar_t* bbar, const wchar_t* note) {
    SYSTEMTIME st;
    GetLocalTime(&st);

    EnterCriticalSection(&g_data_cs);
    if (g_row_count < MAX_ROWS) {
        ResultRow* r = &g_rows[g_row_count++];
        swprintf(r->time, 32, L"%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);
        wcsncpy(r->a, a, 31); r->a[31] = 0;
        wcsncpy(r->b, b, 31); r->b[31] = 0;
        wcsncpy(r->abar, abar, 63); r->abar[63] = 0;
        wcsncpy(r->bbar, bbar, 63); r->bbar[63] = 0;
        wcsncpy(r->note, note, 127); r->note[127] = 0;
    } else {
        memmove(&g_rows[0], &g_rows[1], sizeof(ResultRow) * (MAX_ROWS - 1));
        ResultRow* r = &g_rows[MAX_ROWS - 1];
        swprintf(r->time, 32, L"%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);
        wcsncpy(r->a, a, 31); r->a[31] = 0;
        wcsncpy(r->b, b, 31); r->b[31] = 0;
        wcsncpy(r->abar, abar, 63); r->abar[63] = 0;
        wcsncpy(r->bbar, bbar, 63); r->bbar[63] = 0;
        wcsncpy(r->note, note, 127); r->note[127] = 0;
    }
    LeaveCriticalSection(&g_data_cs);
}

static void UpdateListView(void) {
    if (!g_list) return;
    ListView_DeleteAllItems(g_list);

    EnterCriticalSection(&g_data_cs);
    for (int i = 0; i < g_row_count; i++) {
        LVITEMW it;
        ZeroMemory(&it, sizeof(it));
        it.mask = LVIF_TEXT;
        it.iItem = i;

        it.iSubItem = 0;
        it.pszText = g_rows[i].time;
        ListView_InsertItem(g_list, &it);

        ListView_SetItemText(g_list, i, 1, g_rows[i].a);
        ListView_SetItemText(g_list, i, 2, g_rows[i].b);
        ListView_SetItemText(g_list, i, 3, g_rows[i].abar);
        ListView_SetItemText(g_list, i, 4, g_rows[i].bbar);
        ListView_SetItemText(g_list, i, 5, g_rows[i].note);
    }
    LeaveCriticalSection(&g_data_cs);
}

static void WideToUtf8(char* out, size_t outsz, const wchar_t* in) {
    WideCharToMultiByte(CP_UTF8, 0, in, -1, out, (int)outsz, NULL, NULL);
}

static void JsonEscapeAppend(char* dst, size_t dstsz, const wchar_t* src) {
    size_t len = strlen(dst);
    for (; *src && len + 8 < dstsz; src++) {
        unsigned ch = (unsigned)*src;
        if (*src == L'\\' || *src == L'"') {
            dst[len++] = '\\';
            dst[len++] = (char)*src;
        } else if (*src == L'\r') {
            continue;
        } else if (*src == L'\n') {
            dst[len++] = '\\';
            dst[len++] = 'n';
        } else if (ch < 0x20) {
            len += _snprintf(dst + len, dstsz - len, "\\u%04x", ch);
        } else {
            char tmp[8];
            WideCharToMultiByte(CP_UTF8, 0, src, 1, tmp, sizeof(tmp), NULL, NULL);
            dst[len++] = tmp[0];
        }
    }
    dst[len] = 0;
}

static void EscapeUtf8Json(char* dst, size_t dstsz, const char* src) {
    size_t len = strlen(dst);
    for (; *src && len + 8 < dstsz; src++) {
        unsigned char c = (unsigned char)*src;
        if (c == '\\' || c == '"') {
            dst[len++] = '\\';
            dst[len++] = (char)c;
        } else if (c == '\r') {
            continue;
        } else if (c == '\n') {
            dst[len++] = '\\';
            dst[len++] = 'n';
        } else if (c < 0x20) {
            len += _snprintf(dst + len, dstsz - len, "\\u%04x", c);
        } else {
            dst[len++] = (char)c;
        }
    }
    dst[len] = 0;
}

static int SendAll(SOCKET s, const char* data, int len) {
    int sent = 0;
    while (sent < len) {
        int n = send(s, data + sent, len - sent, 0);
        if (n <= 0) return 0;
        sent += n;
    }
    return 1;
}

static void SendHttpText(SOCKET s, const char* code, const char* ctype, const char* body) {
    char hdr[512];
    int blen = (int)strlen(body);
    _snprintf(hdr, sizeof(hdr),
              "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %d\r\nConnection: close\r\n\r\n",
              code, ctype, blen);
    SendAll(s, hdr, (int)strlen(hdr));
    SendAll(s, body, blen);
}

static int ParseContentLength(const char* req) {
    const char* p = strstr(req, "Content-Length:");
    if (!p) p = strstr(req, "content-length:");
    if (!p) return 0;
    p += 15;
    while (*p == ' ' || *p == '\t') p++;
    return atoi(p);
}

static void UrlDecodeInPlace(char* s) {
    char* o = s;
    while (*s) {
        if (*s == '%' && s[1] && s[2]) {
            char hex[3] = { s[1], s[2], 0 };
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

static void ExtractFormValue(const char* body, const char* key, char* out, int outsz) {
    out[0] = 0;
    const char* p = strstr(body, key);
    if (!p) return;
    p += strlen(key);
    if (*p != '=') return;
    p++;
    const char* e = strchr(p, '&');
    if (!e) e = p + strlen(p);
    int n = (int)(e - p);
    if (n >= outsz) n = outsz - 1;
    memcpy(out, p, n);
    out[n] = 0;
    UrlDecodeInPlace(out);
}

static void ExtractJsonValue(const char* body, const char* key, char* out, int outsz) {
    out[0] = 0;
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char* p = strstr(body, pat);
    if (!p) return;
    p = strchr(p, ':');
    if (!p) return;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '"') {
        p++;
        const char* e = p;
        while (*e) {
            if (*e == '"' && e > p && e[-1] != '\\') break;
            e++;
        }
        int n = (int)(e - p);
        if (n >= outsz) n = outsz - 1;
        memcpy(out, p, n);
        out[n] = 0;
    } else {
        const char* e = p;
        while (*e && *e != ',' && *e != '}' && *e != '\r' && *e != '\n') e++;
        int n = (int)(e - p);
        if (n >= outsz) n = outsz - 1;
        memcpy(out, p, n);
        out[n] = 0;
    }
}

static int TcpPingOne(const wchar_t* host, const wchar_t* port, double* ms_out) {
    struct addrinfo hints, *res = NULL, *rp = NULL;
    SOCKET s = INVALID_SOCKET;
    char host8[128], port8[16];
    WideToUtf8(host8, sizeof(host8), host);
    WideToUtf8(port8, sizeof(port8), port);

    ZeroMemory(&hints, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    if (getaddrinfo(host8, port8, &hints, &res) != 0) return 0;

    for (rp = res; rp; rp = rp->ai_next) {
        s = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (s == INVALID_SOCKET) continue;

        DWORD tv = 1000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));

        LARGE_INTEGER f, t1, t2;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t1);

        int rc = connect(s, rp->ai_addr, (int)rp->ai_addrlen);

        QueryPerformanceCounter(&t2);
        closesocket(s);
        s = INVALID_SOCKET;

        if (rc == 0) {
            *ms_out = (double)(t2.QuadPart - t1.QuadPart) * 1000.0 / (double)f.QuadPart;
            freeaddrinfo(res);
            return 1;
        }
    }

    freeaddrinfo(res);
    return 0;
}

static unsigned __stdcall PingThread(void* arg) {
    (void)arg;
    LogOp(L"Ping started");

    while (!g_should_stop) {
        wchar_t a_ms[32] = L"timeout";
        wchar_t b_ms[32] = L"timeout";
        wchar_t note[128] = L"";

        double am = -1.0, bm = -1.0;
        int aok = TcpPingOne(g_a_host, g_a_port, &am);
        int bok = TcpPingOne(g_b_host, g_b_port, &bm);

        EnterCriticalSection(&g_data_cs);
        if (aok) { g_a_ok++; g_a_last = am; swprintf(a_ms, 32, L"%.1f", am); }
        else { g_a_fail++; g_a_last = -1.0; }

        if (bok) { g_b_ok++; g_b_last = bm; swprintf(b_ms, 32, L"%.1f", bm); }
        else { g_b_fail++; g_b_last = -1.0; }

        if (aok && bok) {
            swprintf(note, 128, L"Δ %.1f ms", am - bm);
            wcscpy(g_last_note, note);
        } else if (!aok && !bok) {
            wcscpy(g_last_note, L"Both timeout");
        } else if (!aok) {
            wcscpy(g_last_note, L"A timeout");
        } else {
            wcscpy(g_last_note, L"B timeout");
        }
        LeaveCriticalSection(&g_data_cs);

        AddResultRow(a_ms, b_ms, aok ? L"|" : L"", bok ? L"|" : L"", note);
        UpdateListView();
        SetStatusText();

        Sleep(1000);
    }

    LogOp(L"Ping stopped");
    g_ping_thread = NULL;
    return 0;
}

static void BuildStatusJson(char* out, size_t outsz) {
    char op[MAX_LOG * 2] = {0};
    char dbg[MAX_LOG * 2] = {0};
    char note[512] = {0};
    char ahost[256], bhost[256], aport[32], bport[32];

    WideToUtf8(ahost, sizeof(ahost), g_a_host);
    WideToUtf8(bhost, sizeof(bhost), g_b_host);
    WideToUtf8(aport, sizeof(aport), g_a_port);
    WideToUtf8(bport, sizeof(bport), g_b_port);

    EnterCriticalSection(&g_log_cs);
    WideToUtf8(op, sizeof(op), g_log_op);
    WideToUtf8(dbg, sizeof(dbg), g_log_dbg);
    LeaveCriticalSection(&g_log_cs);

    EnterCriticalSection(&g_data_cs);
    WideToUtf8(note, sizeof(note), g_last_note);

    _snprintf(out, outsz,
        "{"
        "\"running\":%s,"
        "\"server_port\":%u,"
        "\"a\":{\"host\":\"%s\",\"port\":\"%s\",\"ok\":%lld,\"fail\":%lld,\"last_ms\":%.1f},"
        "\"b\":{\"host\":\"%s\",\"port\":\"%s\",\"ok\":%lld,\"fail\":%lld,\"last_ms\":%.1f},"
        "\"last_log\":\"",
        g_running ? "true" : "false", g_server_port,
        ahost, aport, g_a_ok, g_a_fail, g_a_last,
        bhost, bport, g_b_ok, g_b_fail, g_b_last);

    JsonEscapeAppend(out, outsz, L"");
    {
        char* p = out + strlen(out);
        EscapeUtf8Json(p, outsz - (size_t)(p - out), note);
    }
    strncat(out, "\",\"rows\":[", outsz - strlen(out) - 1);

    for (int i = 0; i < g_row_count; i++) {
        char t[64], aa[64], bb[64], ab[128], bb2[128], no[128];
        WideToUtf8(t, sizeof(t), g_rows[i].time);
        WideToUtf8(aa, sizeof(aa), g_rows[i].a);
        WideToUtf8(bb, sizeof(bb), g_rows[i].b);
        WideToUtf8(ab, sizeof(ab), g_rows[i].abar);
        WideToUtf8(bb2, sizeof(bb2), g_rows[i].bbar);
        WideToUtf8(no, sizeof(no), g_rows[i].note);

        char row[1024];
        _snprintf(row, sizeof(row),
            "%s{\"time\":\"%s\",\"a\":\"%s\",\"b\":\"%s\",\"abar\":\"%s\",\"bbar\":\"%s\",\"note\":\"%s\",\"ah\":%d,\"bh\":%d}",
            i ? "," : "", t, aa, bb, ab, bb2, no,
            (int)((g_a_last < 0 ? 0 : g_a_last) > 34 ? 34 : (g_a_last < 0 ? 0 : g_a_last)),
            (int)((g_b_last < 0 ? 0 : g_b_last) > 34 ? 34 : (g_b_last < 0 ? 0 : g_b_last)));

        strncat(out, row, outsz - strlen(out) - 1);
    }

    strncat(out, "],\"op_log\":\"", outsz - strlen(out) - 1);
    strncat(out, op, outsz - strlen(out) - 1);
    strncat(out, "\",\"dbg_log\":\"", outsz - strlen(out) - 1);
    strncat(out, dbg, outsz - strlen(out) - 1);
    strncat(out, "\"}", outsz - strlen(out) - 1);

    LeaveCriticalSection(&g_data_cs);
}

static int HandleRequest(SOCKET s, char* req) {
    char method[8] = {0}, path[256] = {0};
    sscanf(req, "%7s %255s", method, path);

    if (strcmp(method, "GET") == 0 && strcmp(path, "/") == 0) {
        const char* html =
"<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>TCPing GUI</title><style>"
"body{font-family:Segoe UI,Arial,sans-serif;background:#f5f7fb;color:#1f2937;margin:12px}"
".row{display:flex;gap:8px;flex-wrap:wrap;margin-bottom:8px;align-items:end}"
"input,button,textarea{font:13px Segoe UI,Arial;padding:6px 8px;border:1px solid #cfd8e3;border-radius:6px;background:#fff}"
"button{background:#e9eef8;cursor:pointer}"
"button:hover{background:#dfe8f8}"
"table{width:100%;border-collapse:collapse;background:#fff;border:1px solid #d8e0ea}"
"th,td{font-size:12px;border-bottom:1px solid #edf1f5;padding:5px 6px;text-align:left;vertical-align:bottom}"
".bars{height:34px;display:flex;align-items:flex-end}"
".bar{width:12px;border-radius:4px 4px 0 0;opacity:.9}"
".a{background:#2b7cff}.b{background:#ff9800}"
".panel{background:#fff;border:1px solid #d8e0ea;border-radius:8px;padding:8px;margin-top:8px}"
".status{padding:8px;background:#eef3fb;border:1px solid #ced9ea;border-radius:8px;margin:8px 0;font-size:12px}"
"textarea{width:100%;height:120px;resize:vertical;white-space:pre;font-family:Consolas,monospace}"
"label{font-size:12px;color:#4b5563}"
"</style></head><body>"
"<div class='row'>"
"<label>Target A<br><input id='a' value='8.8.8.8'></label>"
"<label>Port A<br><input id='ap' value='53' style='width:80px'></label>"
"<label>Target B<br><input id='b' value='1.1.1.1'></label>"
"<label>Port B<br><input id='bp' value='53' style='width:80px'></label>"
"<label>Duration<br><input id='d' value='30' style='width:80px'></label>"
"<button id='start'>Start</button><button id='stop'>Stop</button>"
"</div>"
"<div class='status'><span id='state'>Stopped</span> | <span id='stats'></span> | <span id='lastlog'></span></div>"
"<div class='panel'><table><thead><tr><th>Time</th><th>A</th><th>B</th><th>A Bar</th><th>B Bar</th><th>Note</th></tr></thead><tbody id='tbody'></tbody></table></div>"
"<div class='panel'><div style='font-size:12px;margin-bottom:4px'>Operational Log</div><textarea id='oplog' readonly></textarea></div>"
"<div class='panel'><div style='font-size:12px;margin-bottom:4px'>Debug Log</div><textarea id='dbglog' readonly></textarea></div>"
"<script src='/main.js'></script></body></html>";
        SendHttpText(s, "200 OK", "text/html; charset=utf-8", html);
        return 1;
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/main.js") == 0) {
        SendHttpText(s, "200 OK", "application/javascript; charset=utf-8", g_main_js);
        return 1;
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/status") == 0) {
        char json[16384];
        json[0] = 0;
        BuildStatusJson(json, sizeof(json));
        SendHttpText(s, "200 OK", "application/json; charset=utf-8", json);
        return 1;
    }

    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/stop") == 0) {
        g_should_stop = 1;
        if (g_ping_thread) {
            WaitForSingleObject(g_ping_thread, INFINITE);
            CloseHandle(g_ping_thread);
            g_ping_thread = NULL;
        }
        g_running = 0;
        LogOp(L"Stopped");
        SetStatusText();
        SendHttpText(s, "200 OK", "application/json; charset=utf-8", "{\"ok\":true}");
        return 1;
    }

    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/start") == 0) {
        char *body = strstr(req, "\r\n\r\n");
        if (!body) {
            SendHttpText(s, "400 Bad Request", "application/json; charset=utf-8", "{\"ok\":false}");
            return 1;
        }
        body += 4;

        char a[128] = {0}, ap[16] = {0}, b[128] = {0}, bp[16] = {0}, d[16] = {0};

        if (strstr(req, "application/json")) {
            ExtractJsonValue(body, "a", a, sizeof(a));
            ExtractJsonValue(body, "ap", ap, sizeof(ap));
            ExtractJsonValue(body, "b", b, sizeof(b));
            ExtractJsonValue(body, "bp", bp, sizeof(bp));
            ExtractJsonValue(body, "d", d, sizeof(d));
        } else {
            ExtractFormValue(body, "a", a, sizeof(a));
            ExtractFormValue(body, "ap", ap, sizeof(ap));
            ExtractFormValue(body, "b", b, sizeof(b));
            ExtractFormValue(body, "bp", bp, sizeof(bp));
            ExtractFormValue(body, "d", d, sizeof(d));
        }

        if (a[0]) MultiByteToWideChar(CP_UTF8, 0, a, -1, g_a_host, 128);
        if (ap[0]) MultiByteToWideChar(CP_UTF8, 0, ap, -1, g_a_port, 16);
        if (b[0]) MultiByteToWideChar(CP_UTF8, 0, b, -1, g_b_host, 128);
        if (bp[0]) MultiByteToWideChar(CP_UTF8, 0, bp, -1, g_b_port, 16);
        if (d[0]) g_duration = atoi(d);

        EnterCriticalSection(&g_data_cs);
        g_a_ok = g_a_fail = g_b_ok = g_b_fail = 0;
        g_a_last = g_b_last = -1.0;
        wcscpy(g_last_note, L"Running");
        g_row_count = 0;
        LeaveCriticalSection(&g_data_cs);

        g_should_stop = 0;
        g_running = 1;

        if (!g_ping_thread) {
            unsigned tid = 0;
            g_ping_thread = (HANDLE)_beginthreadex(NULL, 0, PingThread, NULL, 0, &tid);
        }

        LogOp(L"Start A=%s:%s B=%s:%s Duration=%d", g_a_host, g_a_port, g_b_host, g_b_port, g_duration);
        SetStatusText();
        SendHttpText(s, "200 OK", "application/json; charset=utf-8", "{\"ok\":true}");
        return 1;
    }

    SendHttpText(s, "404 Not Found", "text/plain; charset=utf-8", "not found");
    return 1;
}

static unsigned __stdcall ServerThread(void* arg) {
    (void)arg;
    char buf[8192];
    struct sockaddr_in addr;

    for (UINT p = SERVER_PORT_BASE; p <= SERVER_PORT_MAX; p++) {
        g_listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (g_listen_sock == INVALID_SOCKET) continue;

        BOOL yes = TRUE;
        setsockopt(g_listen_sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));

        ZeroMemory(&addr, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons((u_short)p);

        if (bind(g_listen_sock, (struct sockaddr*)&addr, sizeof(addr)) == 0 &&
            listen(g_listen_sock, 8) == 0) {
            g_server_port = p;
            break;
        }

        closesocket(g_listen_sock);
        g_listen_sock = INVALID_SOCKET;
    }

    if (g_listen_sock == INVALID_SOCKET) {
        LogOp(L"Server bind failed");
        return 0;
    }

    LogOp(L"Server listening on 127.0.0.1:%u", g_server_port);
    SetStatusText();
    ShellExecuteW(NULL, L"open", L"http://127.0.0.1:38000", NULL, NULL, SW_SHOWNORMAL);

    while (!g_should_stop) {
        fd_set rfds;
        struct timeval tv;
        FD_ZERO(&rfds);
        FD_SET(g_listen_sock, &rfds);
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        int sel = select(0, &rfds, NULL, NULL, &tv);
        if (sel > 0 && FD_ISSET(g_listen_sock, &rfds)) {
            SOCKET client = accept(g_listen_sock, NULL, NULL);
            if (client != INVALID_SOCKET) {
                int n = recv(client, buf, sizeof(buf) - 1, 0);
                if (n > 0) {
                    buf[n] = 0;
                    HandleRequest(client, buf);
                }
                closesocket(client);
            }
        }
    }

    if (g_listen_sock != INVALID_SOCKET) {
        closesocket(g_listen_sock);
        g_listen_sock = INVALID_SOCKET;
    }

    LogOp(L"Server stopped");
    return 0;
}

static void TrayAdd(HWND hwnd) {
    NOTIFYICONDATAW nid;
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = ID_TRAYICON;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    lstrcpyW(nid.szTip, L"TCPing GUI");
    Shell_NotifyIconW(NIM_ADD, &nid);
}

static void TrayDel(HWND hwnd) {
    NOTIFYICONDATAW nid;
    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = ID_TRAYICON;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

static void ShowMainWindow(HWND hwnd) {
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
}

static void StopPingThread(void) {
    g_should_stop = 1;
    if (g_ping_thread) {
        WaitForSingleObject(g_ping_thread, INFINITE);
        CloseHandle(g_ping_thread);
        g_ping_thread = NULL;
    }
    g_running = 0;
    SetStatusText();
}

static void StopAll(void) {
    g_should_stop = 1;
    StopPingThread();
    if (g_server_thread) {
        WaitForSingleObject(g_server_thread, INFINITE);
        CloseHandle(g_server_thread);
        g_server_thread = NULL;
    }
}

static void CreateControls(HWND hwnd) {
    g_editAHost = CreateWindowW(L"EDIT", g_a_host, WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, 10, 10, 140, 24, hwnd, NULL, NULL, NULL);
    g_editAPort = CreateWindowW(L"EDIT", g_a_port, WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, 160, 10, 60, 24, hwnd, NULL, NULL, NULL);
    g_editBHost = CreateWindowW(L"EDIT", g_b_host, WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, 230, 10, 140, 24, hwnd, NULL, NULL, NULL);
    g_editBPort = CreateWindowW(L"EDIT", g_b_port, WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, 380, 10, 60, 24, hwnd, NULL, NULL, NULL);
    g_editDur = CreateWindowW(L"EDIT", L"30", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, 450, 10, 60, 24, hwnd, NULL, NULL, NULL);

    g_btnStart = CreateWindowW(L"BUTTON", L"Start", WS_CHILD | WS_VISIBLE, 520, 10, 70, 24, hwnd, (HMENU)1, NULL, NULL);
    g_btnStop = CreateWindowW(L"BUTTON", L"Stop", WS_CHILD | WS_VISIBLE, 595, 10, 70, 24, hwnd, (HMENU)2, NULL, NULL);
    g_btnShow = CreateWindowW(L"BUTTON", L"Show", WS_CHILD | WS_VISIBLE, 670, 10, 70, 24, hwnd, (HMENU)3, NULL, NULL);

    g_status = CreateWindowW(L"STATIC", L"Stopped", WS_CHILD | WS_VISIBLE, 10, 42, 760, 20, hwnd, NULL, NULL, NULL);

    g_list = CreateWindowW(WC_LISTVIEWW, NULL, WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL, 10, 66, 760, 200, hwnd, NULL, NULL, NULL);
    ListView_SetExtendedListViewStyle(g_list, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

    LVCOLUMNW c;
    ZeroMemory(&c, sizeof(c));
    c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;

    c.pszText = L"Time"; c.cx = 80; c.iSubItem = 0; ListView_InsertColumn(g_list, 0, &c);
    c.pszText = L"A"; c.cx = 90; c.iSubItem = 1; ListView_InsertColumn(g_list, 1, &c);
    c.pszText = L"B"; c.cx = 90; c.iSubItem = 2; ListView_InsertColumn(g_list, 2, &c);
    c.pszText = L"A Bar"; c.cx = 120; c.iSubItem = 3; ListView_InsertColumn(g_list, 3, &c);
    c.pszText = L"B Bar"; c.cx = 120; c.iSubItem = 4; ListView_InsertColumn(g_list, 4, &c);
    c.pszText = L"Note"; c.cx = 250; c.iSubItem = 5; ListView_InsertColumn(g_list, 5, &c);

    g_hLogOp = CreateWindowW(L"EDIT", NULL, WS_CHILD | WS_VISIBLE | WS_BORDER | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL, 10, 275, 370, 160, hwnd, NULL, NULL, NULL);
    g_hLogDbg = CreateWindowW(L"EDIT", NULL, WS_CHILD | WS_VISIBLE | WS_BORDER | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL, 400, 275, 370, 160, hwnd, NULL, NULL, NULL);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        CreateControls(hwnd);
        TrayAdd(hwnd);
        if (!g_server_thread) {
            unsigned tid = 0;
            g_server_thread = (HANDLE)_beginthreadex(NULL, 0, ServerThread, NULL, 0, &tid);
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case 1: {
            GetWindowTextW(g_editAHost, g_a_host, 128);
            GetWindowTextW(g_editAPort, g_a_port, 16);
            GetWindowTextW(g_editBHost, g_b_host, 128);
            GetWindowTextW(g_editBPort, g_b_port, 16);
            wchar_t d[16];
            GetWindowTextW(g_editDur, d, 16);
            g_duration = _wtoi(d);

            EnterCriticalSection(&g_data_cs);
            g_a_ok = g_a_fail = g_b_ok = g_b_fail = 0;
            g_a_last = g_b_last = -1.0;
            wcscpy(g_last_note, L"Running");
            g_row_count = 0;
            LeaveCriticalSection(&g_data_cs);

            g_should_stop = 0;
            g_running = 1;
            if (!g_ping_thread) {
                unsigned tid = 0;
                g_ping_thread = (HANDLE)_beginthreadex(NULL, 0, PingThread, NULL, 0, &tid);
            }
            LogOp(L"Manual start");
            SetStatusText();
            return 0;
        }
        case 2:
            StopPingThread();
            LogOp(L"Manual stop");
            return 0;
        case 3:
            ShowMainWindow(hwnd);
            return 0;
        }
        break;

    case WM_TRAYICON:
        if (lParam == WM_LBUTTONDBLCLK) {
            ShowMainWindow(hwnd);
        } else if (lParam == WM_RBUTTONUP) {
            POINT pt;
            GetCursorPos(&pt);
            HMENU m = CreatePopupMenu();
            AppendMenuW(m, MF_STRING, 1, L"Show Window");
            AppendMenuW(m, MF_STRING, 2, L"Start");
            AppendMenuW(m, MF_STRING, 3, L"Stop");
            AppendMenuW(m, MF_STRING, 4, L"Exit");
            SetForegroundWindow(hwnd);
            int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
            DestroyMenu(m);
            if (cmd == 1) ShowMainWindow(hwnd);
            else if (cmd == 2) SendMessageW(hwnd, WM_COMMAND, 1, 0);
            else if (cmd == 3) SendMessageW(hwnd, WM_COMMAND, 2, 0);
            else if (cmd == 4) DestroyWindow(hwnd);
        }
        return 0;

    case WM_DESTROY:
        TrayDel(hwnd);
        StopAll();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, PWSTR cmd, int show) {
    (void)hPrev;
    (void)cmd;
    (void)show;

    InitializeCriticalSection(&g_log_cs);
    InitializeCriticalSection(&g_data_cs);

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 1;

    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icc);

    WNDCLASSW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = APP_CLASS;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassW(&wc);

    g_hwnd = CreateWindowW(APP_CLASS, APP_TITLE, WS_OVERLAPPEDWINDOW,
                           CW_USEDEFAULT, CW_USEDEFAULT, 800, 480,
                           NULL, NULL, hInst, NULL);
    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);
    SetStatusText();

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    DeleteCriticalSection(&g_log_cs);
    DeleteCriticalSection(&g_data_cs);
    WSACleanup();
    return 0;
}
