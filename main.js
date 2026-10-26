(() => {
  const app = document.createElement('div');
  document.body.appendChild(app);

  const style = document.createElement('style');
  style.textContent = `
    :root{--bg:#f4f7fb;--card:#fff;--line:#dde5ef;--text:#243244;--muted:#6d7c8f;--blue:#3c86ff;--orange:#ff9a3c;--green:#24a148;--red:#e55353;}
    *{box-sizing:border-box}
    body{margin:0;font-family:system-ui,-apple-system,BlinkMacSystemFont,"Segoe UI","PingFang SC","Microsoft YaHei",sans-serif;background:var(--bg);color:var(--text)}
    #app{max-width:1280px;margin:0 auto;padding:12px}
    .topbar,.panel,.box,.tablewrap,.footerbar{background:var(--card);border:1px solid var(--line);border-radius:10px;box-shadow:0 2px 10px rgba(25,40,60,.04)}
    .topbar{display:flex;align-items:center;justify-content:space-between;padding:10px 14px;margin-bottom:10px;min-height:48px}
    .brand{font-weight:700;font-size:15px}
    .hint{color:var(--muted);font-size:12px}
    .panel{padding:12px;margin-bottom:10px}
    .row{display:grid;gap:10px;grid-template-columns:1.4fr .7fr 1.4fr .7fr .8fr auto auto auto;align-items:center}
    input,button{height:34px;border-radius:8px;border:1px solid var(--line);font:inherit;font-size:13px}
    input{width:100%;padding:0 10px;background:#fff;color:var(--text);outline:none}
    input:focus{border-color:#9fc0ff;box-shadow:0 0 0 3px rgba(60,134,255,.12)}
    button{padding:0 14px;cursor:pointer;background:#fff;color:var(--text)}
    button.primary{background:linear-gradient(180deg,#4b92ff,#2f76f7);color:#fff;border-color:#2f76f7}
    button.danger{background:linear-gradient(180deg,#ff6f6f,#e94c4c);color:#fff;border-color:#e94c4c}
    button:active{transform:translateY(1px)}
    .tablewrap{overflow:hidden;margin-bottom:10px}
    table{width:100%;border-collapse:collapse;font-size:13px}
    thead th{background:#f8fbff;color:#516072;text-align:left;padding:10px 8px;border-bottom:1px solid var(--line);font-weight:600}
    tbody td{padding:8px;border-bottom:1px solid #edf2f7;vertical-align:middle}
    tbody tr:hover{background:#fafcff}
    .barcell{width:120px}
    .vbar{width:10px;height:22px;border-radius:6px;background:#d8e5ff;position:relative;overflow:hidden;display:inline-block;vertical-align:middle}
    .vbar > i{position:absolute;left:0;bottom:0;width:100%;border-radius:6px;display:block}
    .vblue > i{background:var(--blue)}
    .vorange > i{background:var(--orange)}
    .logstatus{display:grid;grid-template-columns:1.3fr .9fr;gap:10px}
    .box{padding:10px;min-height:120px}
    .box h3{margin:0 0 8px;font-size:13px;color:#425266}
    #log,#debug{height:160px;overflow:auto;white-space:pre-wrap;font-size:12px;line-height:1.5;background:#fff;border:1px solid #e5eaf2;border-radius:8px;padding:8px;color:#334155}
    .kv{display:grid;grid-template-columns:90px 1fr;gap:6px 10px;font-size:12px;line-height:1.6}
    .k{color:var(--muted)}
    .v{color:var(--text);word-break:break-all}
    .footerbar{margin-top:10px;padding:8px 12px;color:#5d6d80;font-size:12px;display:flex;gap:16px;flex-wrap:wrap}
    .dot{display:inline-block;width:8px;height:8px;border-radius:50%;background:#aaa;margin-right:6px;vertical-align:middle}
    .dot.on{background:var(--green)}
    .dot.off{background:var(--red)}
    @media (max-width:1100px){.row{grid-template-columns:1fr 1fr}.logstatus{grid-template-columns:1fr}.barcell{width:auto}}
  `;
  document.head.appendChild(style);

  app.id = 'app';
  app.innerHTML = `
    <div class="topbar">
      <div>
        <div class="brand">TCPing</div>
        <div class="hint">双目标 TCP 延迟测试</div>
      </div>
      <div class="hint" id="topState">就绪</div>
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
        <h3>日志</h3>
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

  const $ = id => document.getElementById(id);
  const tbody = $('tbody');
  const logBox = $('log');
  const debugBox = $('debug');

  function barHTML(v, blue) {
    const n = Number(v);
    if (!isFinite(n) || n < 0) return `<span class="vbar ${blue ? 'vblue' : 'vorange'}"><i style="height:0%"></i></span>`;
    const h = Math.max(10, Math.min(100, Math.round(100 - n)));
    return `<span class="vbar ${blue ? 'vblue' : 'vorange'}"><i style="height:${h}%"></i></span>`;
  }

  function addRow(row) {
    const tr = document.createElement('tr');
    tr.innerHTML = `
      <td>${row.time || ''}</td>
      <td>${row.a ?? ''}</td>
      <td>${row.b ?? ''}</td>
      <td class="barcell">${barHTML(row.a, true)}</td>
      <td class="barcell">${barHTML(row.b, false)}</td>
      <td>${row.note || ''}</td>
    `;
    tbody.prepend(tr);
  }

  function setLog(lines) {
    logBox.textContent = Array.isArray(lines) ? lines.join('\n') : String(lines || '');
    logBox.scrollTop = logBox.scrollHeight;
  }

  function setDebug(lines) {
    debugBox.textContent = Array.isArray(lines) ? lines.join('\n') : String(lines || '');
    debugBox.scrollTop = debugBox.scrollHeight;
  }

  async function api(path, body) {
    const opt = body ? {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify(body)
    } : {};
    const r = await fetch(path, opt);
    return r.json();
  }

  async function refresh() {
    try {
      const s = await api('/api/status');
      $('runningVal').textContent = s.running ? 'Yes' : 'No';
      $('runDot').className = 'dot ' + (s.running ? 'on' : 'off');
      $('runText').textContent = s.running ? 'Running' : 'Stopped';
      $('aStats').textContent = 'A stats: ' + (s.a_stats || '-');
      $('bStats').textContent = 'B stats: ' + (s.b_stats || '-');
      if (Array.isArray(s.logs)) setLog(s.logs);
      if (Array.isArray(s.debug)) setDebug(s.debug);
      if (Array.isArray(s.rows)) {
        tbody.innerHTML = '';
        s.rows.slice().reverse().forEach(addRow);
      }
    } catch (e) {
      $('topState').textContent = 'refresh failed';
    }
  }

  $('startBtn').onclick = async () => {
    await api('/api/start', {
      a_host: $('aHost').value.trim(),
      a_port: $('aPort').value.trim(),
      b_host: $('bHost').value.trim(),
      b_port: $('bPort').value.trim(),
      duration: $('duration').value.trim()
    });
    await refresh();
  };

  $('stopBtn').onclick = async () => {
    await api('/api/stop');
    await refresh();
  };

  $('refreshBtn').onclick = refresh;

  setInterval(refresh, 1000);
  refresh();
})();
