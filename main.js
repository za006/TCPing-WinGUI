(() => {
  const css = `
    :root{
      --bg:#0b1020;
      --card:#111a33;
      --line:#24365f;
      --text:#e8eefc;
      --muted:#8ea2d6;
      --a:#2f7cff;
      --b:#ff9f2f;
      --ok:#35c759;
      --bad:#ff4d4f;
    }
    *{box-sizing:border-box}
    body{
      margin:0;
      font-family:Segoe UI,Arial,sans-serif;
      background:linear-gradient(180deg,#0b1020,#08111f);
      color:var(--text);
    }
    .wrap{max-width:1400px;margin:0 auto;padding:18px}
    .card{
      background:rgba(17,26,51,.96);
      border:1px solid rgba(142,162,214,.16);
      border-radius:14px;
      padding:14px;
      box-shadow:0 10px 28px rgba(0,0,0,.22);
      margin-bottom:12px;
    }
    .form{
      display:grid;
      grid-template-columns:110px 1fr 80px 110px 1fr 80px 110px 120px 120px;
      gap:10px;
      align-items:center;
    }
    label{color:var(--muted);font-size:14px}
    input,button{
      height:40px;
      border-radius:10px;
      border:1px solid #2d406f;
      background:#0d1730;
      color:var(--text);
      padding:0 12px;
      font-size:16px;
      outline:none;
    }
    input:focus{border-color:#5c8cff;box-shadow:0 0 0 3px rgba(47,124,255,.18)}
    button{cursor:pointer;font-weight:700}
    .primary{background:linear-gradient(180deg,#2f7cff,#1f5ed9)}
    .danger{background:linear-gradient(180deg,#ff6b6b,#d93a3a)}
    .gray{background:linear-gradient(180deg,#42506f,#2f3952)}
    .stats{display:flex;gap:12px;flex-wrap:wrap;margin:12px 0}
    .pill{
      padding:10px 12px;
      border-radius:999px;
      border:1px solid rgba(142,162,214,.18);
      background:rgba(13,23,48,.95);
      font-size:14px;
    }
    .grid{display:grid;grid-template-columns:1.35fr .9fr;gap:12px}
    table{width:100%;border-collapse:collapse;font-size:14px}
    thead th{
      position:sticky;top:0;
      background:#132042;
      color:#cfe0ff;
      text-align:left;
      padding:10px;
      border-bottom:1px solid var(--line);
    }
    tbody td{
      padding:8px 10px;
      border-bottom:1px solid #1c2846;
      vertical-align:middle;
    }
    tbody tr:nth-child(2n){background:rgba(255,255,255,.02)}
    .barbox{
      height:16px;
      background:#0d1730;
      border-radius:999px;
      overflow:hidden;
      border:1px solid #27406f;
      min-width:180px;
    }
    .bar{
      height:100%;
      border-radius:999px;
    }
    .a{background:linear-gradient(90deg,#59a8ff,#2f7cff)}
    .b{background:linear-gradient(90deg,#ffca66,#ff9f2f)}
    .ok{color:var(--ok)}
    .bad{color:var(--bad)}
    .muted{color:var(--muted)}
    .log{
      height:240px;
      overflow:auto;
      background:#081021;
      border:1px solid #22345a;
      border-radius:12px;
      padding:10px;
      font-family:Consolas,monospace;
      font-size:13px;
      white-space:pre-wrap;
    }
    .hint{margin-top:10px;color:var(--muted);font-size:13px}
  `;

  document.head.insertAdjacentHTML('beforeend', `<style>${css}</style>`);

  document.body.innerHTML = `
    <div class="wrap">
      <div class="card">
        <div class="form">
          <label>Target A</label><input id="aHost" value="360.com">
          <label>Port A</label><input id="aPort" value="80">
          <label>Target B</label><input id="bHost" value="1.1.1.1">
          <label>Port B</label><input id="bPort" value="443">
          <label>Duration</label><input id="dur" value="60">
          <button class="primary" id="startBtn">开始</button>
          <button class="danger" id="stopBtn">停止</button>
          <button class="gray" id="refreshBtn">刷新</button>
        </div>
      </div>

      <div class="stats">
        <div class="pill" id="st">状态：等待</div>
        <div class="pill" id="sa">A：-</div>
        <div class="pill" id="sb">B：-</div>
        <div class="pill" id="sl">日志：-</div>
      </div>

      <div class="grid">
        <div class="card">
          <div class="muted" style="margin-bottom:8px">结果</div>
          <div style="max-height:520px;overflow:auto;border-radius:12px;border:1px solid #22345a">
            <table>
              <thead>
                <tr>
                  <th style="width:95px">Time</th>
                  <th style="width:170px">A</th>
                  <th style="width:170px">B</th>
                  <th>A Bar</th>
                  <th>B Bar</th>
                  <th style="width:110px">Note</th>
                </tr>
              </thead>
              <tbody id="rows"></tbody>
            </table>
          </div>
        </div>

        <div class="card">
          <div class="muted" style="margin-bottom:8px">日志</div>
          <div class="log" id="log"></div>
          <div class="hint">本地网页：127.0.0.1:8080</div>
        </div>
      </div>
    </div>
  `;

  function api(url, body) {
    const opt = { method: 'POST', headers: { 'Content-Type': 'application/json' } };
    if (body) opt.body = JSON.stringify(body);
    return fetch(url, opt).then(r => r.json());
  }

  document.getElementById('startBtn').onclick = async () => {
    await api('/api/start', {
      aHost: document.getElementById('aHost').value,
      aPort: parseInt(document.getElementById('aPort').value || '0', 10),
      bHost: document.getElementById('bHost').value,
      bPort: parseInt(document.getElementById('bPort').value || '0', 10),
      durationSec: parseInt(document.getElementById('dur').value || '0', 10)
    });
    refreshNow();
  };

  document.getElementById('stopBtn').onclick = async () => {
    await api('/api/stop');
    refreshNow();
  };

  document.getElementById('refreshBtn').onclick = refreshNow;

  async function refreshNow() {
    const j = await (await fetch('/api/status')).json();
    document.getElementById('st').textContent = '状态：' + j.runningText;
    document.getElementById('sa').textContent = 'A：' + j.aStat;
    document.getElementById('sb').textContent = 'B：' + j.bStat;
    document.getElementById('sl').textContent = '日志：' + j.lastLog;
    document.getElementById('rows').innerHTML = j.rowsHtml;
    document.getElementById('log').textContent = j.lastLog;
  }

  setInterval(refreshNow, 1000);
  refreshNow();
})();
