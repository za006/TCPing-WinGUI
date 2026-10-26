(() => {
  const $ = (id) => document.getElementById(id);

  const state = {
    running: false,
    timer: null,
    rows: [],
    lastLog: "-",
    statsA: { sent: 0, ok: 0, fail: 0, min: null, max: null, avg: null },
    statsB: { sent: 0, ok: 0, fail: 0, min: null, max: null, avg: null }
  };

  const DEFAULT_DURATION = 50;

  function esc(s) {
    return String(s ?? "")
      .replaceAll("&", "&amp;")
      .replaceAll("<", "&lt;")
      .replaceAll(">", "&gt;")
      .replaceAll('"', "&quot;")
      .replaceAll("'", "&#39;");
  }

  function nowTime() {
    const d = new Date();
    const p = (n) => String(n).padStart(2, "0");
    return `${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}`;
  }

  function barClass(ms) {
    if (ms == null || !isFinite(ms)) return "bar null";
    if (ms < 50) return "bar good";
    if (ms < 150) return "bar mid";
    return "bar bad";
  }

  function barHeight(ms) {
    if (ms == null || !isFinite(ms)) return 8;
    return Math.max(8, Math.min(120, Math.round(8 + ms * 0.8)));
  }

  function fmtMs(v) {
    if (v == null || !isFinite(v)) return "Timeout";
    return `${Number(v).toFixed(1)} ms`;
  }

  function avg(arr) {
    if (!arr.length) return null;
    return arr.reduce((a, b) => a + b, 0) / arr.length;
  }

  function recomputeStats() {
    const aOk = state.rows.map(r => r.a).filter(v => v != null && isFinite(v));
    const bOk = state.rows.map(r => r.b).filter(v => v != null && isFinite(v));

    state.statsA.sent = state.rows.length;
    state.statsB.sent = state.rows.length;
    state.statsA.ok = aOk.length;
    state.statsB.ok = bOk.length;
    state.statsA.fail = state.rows.length - aOk.length;
    state.statsB.fail = state.rows.length - bOk.length;
    state.statsA.min = aOk.length ? Math.min(...aOk) : null;
    state.statsA.max = aOk.length ? Math.max(...aOk) : null;
    state.statsA.avg = avg(aOk);
    state.statsB.min = bOk.length ? Math.min(...bOk) : null;
    state.statsB.max = bOk.length ? Math.max(...bOk) : null;
    state.statsB.avg = avg(bOk);
  }

  function renderStatus() {
    $("statusRun").textContent = state.running ? "Running" : "Stopped";
    $("statusA").textContent = `A: ${state.statsA.ok}/${state.statsA.sent} ok`;
    $("statusB").textContent = `B: ${state.statsB.ok}/${state.statsB.sent} ok`;
    $("statusLast").textContent = `Last: ${state.lastLog}`;
  }

  function renderTable() {
    const tb = $("resultsBody");
    if (!tb) return;

    tb.innerHTML = state.rows.map((r) => {
      const aH = barHeight(r.a);
      const bH = barHeight(r.b);
      return `
        <tr>
          <td class="time">${esc(r.time)}</td>
          <td class="num">${esc(fmtMs(r.a))}</td>
          <td class="num">${esc(fmtMs(r.b))}</td>
          <td class="barcell"><div class="${barClass(r.a)}" style="height:${aH}px"></div></td>
          <td class="barcell"><div class="${barClass(r.b)}" style="height:${bH}px"></div></td>
          <td class="note">${esc(r.note || "")}</td>
        </tr>
      `;
    }).join("");

    recomputeStats();
    renderStatus();
  }

  function renderLogs(op, dbg) {
    if (typeof op === "string" && $("logOp")) $("logOp").value = op;
    if (typeof dbg === "string" && $("logDbg")) $("logDbg").value = dbg;
  }

  async function apiGet(path) {
    const res = await fetch(path, { cache: "no-store" });
    if (!res.ok) throw new Error(`GET ${path} ${res.status}`);
    return await res.json();
  }

  async function apiPost(path, data) {
    const body = new URLSearchParams();
    for (const [k, v] of Object.entries(data)) body.set(k, v);
    const res = await fetch(path, {
      method: "POST",
      headers: { "Content-Type": "application/x-www-form-urlencoded" },
      body: body.toString()
    });
    if (!res.ok) throw new Error(`POST ${path} ${res.status}`);
    return await res.json();
  }

  async function refreshStatus() {
    try {
      const j = await apiGet("/api/status");
      state.running = !!j.running;
      state.lastLog = j.last_log || "-";

      if (Array.isArray(j.rows)) {
        state.rows = j.rows.map(x => ({
          time: x.time || "",
          a: x.a_ms != null ? Number(x.a_ms) : null,
          b: x.b_ms != null ? Number(x.b_ms) : null,
          note: x.note || ""
        }));
      }

      renderTable();
      renderLogs(j.log_op || "", j.log_dbg || "");
    } catch (e) {
      state.lastLog = "status error";
      renderStatus();
    }
  }

  function readInputs() {
    const targetA = $("targetA").value.trim();
    const portA = $("portA").value.trim();
    const targetB = $("targetB").value.trim();
    const portB = $("portB").value.trim();
    const duration = $("duration").value.trim() || String(DEFAULT_DURATION);

    return { targetA, portA, targetB, portB, duration };
  }

  async function startPing() {
    const { targetA, portA, targetB, portB, duration } = readInputs();

    if (!targetA || !portA || !targetB || !portB) {
      alert("Please fill Target A, Port A, Target B, Port B.");
      return;
    }

    try {
      await apiPost("/api/start", {
        target_a: targetA,
        port_a: portA,
        target_b: targetB,
        port_b: portB,
        duration: duration
      });
      state.running = true;
      state.lastLog = "started";
      await refreshStatus();
    } catch (e) {
      alert("Start failed.");
    }
  }

  async function stopPing() {
    try {
      await apiPost("/api/stop", {});
      state.running = false;
      state.lastLog = "stopped";
      await refreshStatus();
    } catch (e) {
      alert("Stop failed.");
    }
  }

  function bindUI() {
    $("btnStart").addEventListener("click", startPing);
    $("btnStop").addEventListener("click", stopPing);
    $("btnClear").addEventListener("click", () => {
      state.rows = [];
      renderTable();
    });

    ["targetA", "portA", "targetB", "portB", "duration"].forEach(id => {
      $(id).addEventListener("keydown", (e) => {
        if (e.key === "Enter") startPing();
      });
    });
  }

  function injectStyle() {
    const css = document.createElement("style");
    css.textContent = `
      :root{
        --bg:#f3f6fb;
        --panel:#fff;
        --line:#dbe3ec;
        --text:#1f2937;
        --muted:#6b7280;
        --blue:#2f80ed;
        --orange:#f2994a;
        --green:#27ae60;
        --red:#eb5757;
        --shadow:0 4px 18px rgba(18,28,45,.08);
      }
      *{box-sizing:border-box}
      body{
        margin:0;
        font-family:Segoe UI, Arial, sans-serif;
        background:var(--bg);
        color:var(--text);
        font-size:13px;
      }
      .wrap{
        max-width:1280px;
        margin:0 auto;
        padding:12px;
      }
      .topbar,.panel,.statusbar{
        background:var(--panel);
        border:1px solid var(--line);
        border-radius:10px;
        box-shadow:var(--shadow);
      }
      .topbar{
        padding:12px;
        margin-bottom:12px;
      }
      .row{
        display:flex;
        gap:10px;
        flex-wrap:wrap;
        align-items:end;
      }
      .field{
        display:flex;
        flex-direction:column;
        gap:5px;
        min-width:140px;
      }
      .field.small{min-width:92px;width:92px;}
      label{color:var(--muted); font-size:12px}
      input{
        height:32px;
        padding:0 10px;
        border:1px solid var(--line);
        border-radius:8px;
        outline:none;
        background:#fff;
      }
      input:focus{
        border-color:#9ec5ff;
        box-shadow:0 0 0 3px rgba(47,128,237,.12);
      }
      button{
        height:32px;
        padding:0 14px;
        border:1px solid var(--line);
        border-radius:8px;
        background:#fff;
        cursor:pointer;
      }
      button.primary{background:#eaf2ff;border-color:#b8d3ff;}
      button.danger{background:#fff0f0;border-color:#ffcbcb;}
      .grid{
        display:grid;
        grid-template-columns:1fr;
        gap:12px;
      }
      .panel{padding:10px 12px;}
      .title{
        font-weight:600;
        margin:0 0 8px 0;
        font-size:13px;
      }
      table{
        width:100%;
        border-collapse:collapse;
        font-size:12px;
      }
      thead th{
        text-align:left;
        padding:8px 6px;
        border-bottom:1px solid var(--line);
        color:#506070;
        font-weight:600;
      }
      tbody td{
        padding:7px 6px;
        border-bottom:1px solid #eef2f7;
        vertical-align:middle;
      }
      .num{width:110px;white-space:nowrap;}
      .time{width:92px;white-space:nowrap;}
      .barcell{width:72px;text-align:center;}
      .bar{
        width:18px;
        border-radius:6px 6px 2px 2px;
        margin:0 auto;
        border:1px solid rgba(0,0,0,.05);
      }
      .bar.good{background:var(--blue);}
      .bar.mid{background:var(--orange);}
      .bar.bad{background:var(--red);}
      .bar.null{background:#cfd8e3;}
      textarea{
        width:100%;
        min-height:86px;
        resize:vertical;
        border:1px solid var(--line);
        border-radius:8px;
        padding:8px;
        background:#fbfcfe;
        font-family:Consolas, monospace;
        font-size:12px;
      }
      .two{
        display:grid;
        grid-template-columns:1fr 1fr;
        gap:12px;
      }
      .statusbar{
        margin-top:12px;
        padding:10px 12px;
        display:flex;
        gap:16px;
        flex-wrap:wrap;
        color:#334155;
      }
      .muted{color:var(--muted);}
      @media (max-width: 900px){
        .two{grid-template-columns:1fr;}
      }
    `;
    document.head.appendChild(css);
  }

  function injectUI() {
    document.body.innerHTML = `
      <div class="wrap">
        <div class="topbar">
          <div class="row">
            <div class="field">
              <label>Target A</label>
              <input id="targetA" value="1.1.1.1" />
            </div>
            <div class="field small">
              <label>Port A</label>
              <input id="portA" value="80" />
            </div>
            <div class="field">
              <label>Target B</label>
              <input id="targetB" value="8.8.8.8" />
            </div>
            <div class="field small">
              <label>Port B</label>
              <input id="portB" value="53" />
            </div>
            <div class="field small">
              <label>Duration</label>
              <input id="duration" value="${DEFAULT_DURATION}" />
            </div>
            <button id="btnStart" class="primary">Start</button>
            <button id="btnStop" class="danger">Stop</button>
            <button id="btnClear">Clear</button>
          </div>
        </div>

        <div class="grid">
          <div class="panel">
            <div class="title">Results</div>
            <table>
              <thead>
                <tr>
                  <th class="time">Time</th>
                  <th class="num">A</th>
                  <th class="num">B</th>
                  <th class="barcell">A Bar</th>
                  <th class="barcell">B Bar</th>
                  <th>Note</th>
                </tr>
              </thead>
              <tbody id="resultsBody"></tbody>
            </table>
          </div>

          <div class="two">
            <div class="panel">
              <div class="title">Operational Log</div>
              <textarea id="logOp" readonly></textarea>
            </div>
            <div class="panel">
              <div class="title">Debug Log</div>
              <textarea id="logDbg" readonly></textarea>
            </div>
          </div>
        </div>

        <div class="statusbar">
          <div id="statusRun">Stopped</div>
          <div id="statusA">A: 0/0 ok</div>
          <div id="statusB">B: 0/0 ok</div>
          <div id="statusLast">Last: -</div>
        </div>
      </div>
    `;
  }

  async function init() {
    injectStyle();
    injectUI();
    bindUI();
    await refreshStatus();
    state.timer = setInterval(refreshStatus, 1000);
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", init);
  } else {
    init();
  }
})();
