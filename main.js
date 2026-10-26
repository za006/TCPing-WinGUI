(() => {
  const app = document.createElement('div');
  app.id = 'app';
  document.body.appendChild(app);

  const style = document.createElement('style');
  style.textContent = `
    :root {
      --bg:#f4f7fb; --card:#fff; --line:#dbe4ef; --text:#233244; --muted:#6c7a8a;
      --blue:#3c86ff; --orange:#ff9a3c; --green:#24a148; --red:#e55353;
    }
    * { box-sizing:border-box; }
    body {
      margin:0; font-family:system-ui,-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;
      background:var(--bg); color:var(--text);
    }
    #app { max-width:1280px; margin:0 auto; padding:12px; }
    .topbar, .panel, .box, .tablewrap, .footerbar {
      background:var(--card); border:1px solid var(--line); border-radius:10px;
      box-shadow:0 2px 10px rgba(25,40,60,.04);
    }
    .topbar {
      display:flex; justify-content:space-between; align-items:center;
      padding:10px 14px; margin-bottom:10px;
    }
    .title { font-weight:700; font-size:15px; }
    .sub { color:var(--muted); font-size:12px; }
    .panel { padding:12px; margin-bottom:10px; }
    .row {
      display:grid; gap:10px;
      grid-template-columns:1.4fr .7fr 1.4fr .7fr .8fr auto auto auto;
      align-items:center;
    }
    input, button {
      height:34px; border-radius:8px; border:1px solid var(--line);
      font:inherit; font-size:13px;
    }
    input {
      width:100%; padding:0 10px; background:#fff; color:var(--text);
    }
    button {
      padding:0 14px; cursor:pointer; background:#fff; color:var(--text);
    }
    button.primary { background:#2f76f7; color:#fff; border-color:#2f76f7; }
    button.danger { background:#e94c4c; color:#fff; border-color:#e94c4c; }
    .tablewrap { overflow:hidden; margin-bottom:10px; }
    table { width:100%; border-collapse:collapse; font-size:13px; }
    thead th {
      background:#f8fbff; text-align:left; padding:10px 8px;
      border-bottom:1px solid var(--line); color:#516072;
    }
    tbody td { padding:8px; border-bottom:1px solid #edf2f7; }
    tbody tr:hover { background:#fafcff; }
    .barcell { width:120px; }
    .vbar {
      width:10px; height:22px; border-radius:6px; background:#d8e5ff;
      display:inline-block; position:relative; overflow:hidden;
    }
    .vbar > i {
      position:absolute; left:0; bottom:0; width:100%; display:block; border-radius:6px;
    }
    .blue > i { background:var(--blue); }
    .orange > i { background:var(--orange); }
    .logstatus { display:grid; grid-template-columns:1.3fr .9fr; gap:10px; }
    .box { padding:10px; min-height:120px; }
    .box h3 { margin:0 0 8px; font-size:13px; color:#425266; }
    #log, #debug {
      height:160px; overflow:auto; white-space:pre-wrap; font-size:12px; line-height:1.5;
      background:#fff; border:1px solid #e5eaf2; border-radius:8px; padding:8px; color:#334155;
    }
    .footerbar {
      margin-top:10px; padding:8px 12px; color:#5d6d80; font-size:12px;
      display:flex; gap:16px; flex-wrap:wrap;
    }
    .dot {
      display:inline-block; width:8px; height:8px; border-radius:50%;
      background:#aaa; margin-right:6px;
    }
    .on { background:var(--green); }
    .off { background:var(--red); }
    @media (max-width:1100px) {
      .row { grid-template-columns:1fr 1fr; }
      .logstatus { grid-template-columns:1fr; }
      .barcell { width:auto; }
    }
  `;
  document.head.appendChild(style);

  app.innerHTML = `
    <div class="topbar">
      <div>
        <div class="title">TCPing</div>
        <div class="sub">Dual target TCP latency test</div>
      </div>
      <div class="sub" id="topState">Ready</div>
    </div>

    <div class="panel">
      <div class="row">
        <input id="aHost" placeholder="Target A">
        <input id="aPort" placeholder="Port A" value="80">
        <input id="bHost" placeholder="Target B">
        <input id="bPort" placeholder="Port B" value="80">
        <input id="duration" placeholder="Duration" value="60">
        <button class="primary" id="startBtn">Start</button>
        <button class="danger" id="stopBtn">Stop</button>
        <button id="refreshBtn">Refresh</button>
      </div>
    </div>

    <div class="tablewrap">
      <table>
        <thead>
          <tr>
            <th style="width:170px">Time</th>
            <th style="width:90px">A</th>
            <th style="width:90px">B</th>
            <th style="width:140px">A Bar</th>
            <th style="width:140px">B Bar</th>
            <th>Note</th>
          </tr>
        </thead>
        <tbody id="tbody"></tbody>
      </table>
    </div>

    <div class="logstatus">
      <div class="box">
        <h3>Log</h3>
        <div id="log"></div>
      </div>
      <div class="box">
        <h3>Debug</h3>
        <div id="debug"></div>
      </div>
    </div>

    <div class="footerbar">
      <span><i class="dot off" id="runDot"></i><span id="runText">Stopped</span></span>
      <span id="runningVal">No</span>
      <span id="aStats">A stats: -</span>
      <span id="bStats">B stats: -</span>
      <span id="lastLog">Last log: -</span>
    </div>
  `;

  const $ = (id) => document.getElementById(id);
  const tbody = $('tbody');
  const logBox = $('log');
  const debugBox = $('debug');

  function txt(v) {
    return v === null || v === undefined ? '' : String(v);
  }

  function bool(v) {
    if (typeof v === 'boolean') return v;
    if (typeof v === 'number') return v !== 0;
    if (typeof v === 'string') return ['1','true','yes','running','on'].includes(v.trim().toLowerCase());
    return false;
  }

  function pick(o, keys, def = '') {
    if (!o || typeof o !== 'object') return def;
    for (const k of keys) if (o[k] !== undefined && o[k] !== null) return o[k];
    return def;
  }

  function bar(v, cls) {
    const n = Number(v);
    const h = (!isFinite(n) || n < 0) ? 0 : Math.max(10, Math.min(100, Math.round(100 - n)));
    return `<span class="vbar ${cls}"><i style="height:${h}%"></i></span>`;
  }

  function addRow(r) {
    const a = pick(r, ['a','A','lat_a','ms_a','latency_a'], '');
    const b = pick(r, ['b','B','lat_b','ms_b','latency_b'], '');
    const t = pick(r, ['time','t','ts'], '');
    const n = pick(r, ['note','msg','message','status'], '');
    const tr = document.createElement('tr');
    tr.innerHTML = `
      <td>${txt(t)}</td>
      <td>${txt(a)}</td>
      <td>${txt(b)}</td>
      <td class="barcell">${bar(a, 'blue')}</td>
      <td class="barcell">${bar(b, 'orange')}</td>
      <td>${txt(n)}</td>
    `;
    tbody.prepend(tr);
  }

  function setLines(el, v) {
    if (Array.isArray(v)) el.textContent = v.map(txt).join('\n');
    else if (typeof v === 'string') el.textContent = v;
    else el.textContent = '';
    el.scrollTop = el.scrollHeight;
  }

  async function api(path, body) {
    const opt = body ? {
      method: 'POST',
      headers: {'Content-Type':'application/json'},
      body: JSON.stringify(body)
    } : {};
    const r = await fetch(path, opt);
    const s = await r.text();
    try { return JSON.parse(s); } catch { return { raw:s }; }
  }

  function applyStatus(s) {
    const running = bool(pick(s, ['running','is_running','started','active'], false));
    $('runningVal').textContent = running ? 'Yes' : 'No';
    $('runDot').className = 'dot ' + (running ? 'on' : 'off');
    $('runText').textContent = running ? 'Running' : 'Stopped';

    $('aStats').textContent = 'A stats: ' + txt(pick(s, ['a_stats','stats_a','a_summary'], '-'));
    $('bStats').textContent = 'B stats: ' + txt(pick(s, ['b_stats','stats_b','b_summary'], '-'));
    $('lastLog').textContent = 'Last log: ' + txt(pick(s, ['last_log','lastLog','latest_log','last'], '-'));

    const logs = pick(s, ['logs','log','op_logs','operational_logs'], null);
    const dbg = pick(s, ['debug','dbg','debug_logs','dbg_logs'], null);
    const rows = pick(s, ['rows','results','table','data'], []);

    if (logs !== null) setLines(logBox, logs);
    if (dbg !== null) setLines(debugBox, dbg);

    if (Array.isArray(rows)) {
      tbody.innerHTML = '';
      rows.slice().reverse().forEach(addRow);
    }
  }

  async function refresh() {
    try {
      const s = await api('/api/status');
      applyStatus(s);
      $('topState').textContent = 'OK';
    } catch (e) {
      $('topState').textContent = 'Refresh failed';
    }
  }

  $('startBtn').onclick = async () => {
    try {
      await api('/api/start', {
        a_host: $('aHost').value.trim(),
        a_port: $('aPort').value.trim(),
        b_host: $('bHost').value.trim(),
        b_port: $('bPort').value.trim(),
        duration: $('duration').value.trim()
      });
      await refresh();
    } catch (e) {
      $('topState').textContent = 'Start failed';
    }
  };

  $('stopBtn').onclick = async () => {
    try {
      await api('/api/stop');
      await refresh();
    } catch (e) {
      $('topState').textContent = 'Stop failed';
    }
  };

  $('refreshBtn').onclick = refresh;

  setInterval(refresh, 1000);
  refresh();
})();
