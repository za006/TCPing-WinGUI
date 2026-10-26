const $ = id => document.getElementById(id);

function bar(v, c) {
  v = v > 9998 ? 0 : v;
  const h = Math.max(2, Math.min(18, Math.round(v / 10)));
  return `<div class="bars"><div class="${c}" style="height:${h}px"></div></div>`;
}

function row(r) {
  return `<tr>
    <td>${r.t}</td><td>${r.a}</td><td>${r.b}</td>
    <td>${bar(r.ams, 'a')}</td><td>${bar(r.bms, 'b')}</td><td>${r.note}</td>
  </tr>`;
}

async function api(u) {
  return await fetch(u, { cache: 'no-store' });
}

async function refresh() {
  const s = await (await api('/api/status')).json();
  $('st').textContent = 'Status: ' + (s.running ? 'Running' : 'Stopped');
  $('sa').textContent = 'A: ' + s.a_stats;
  $('sb').textContent = 'B: ' + s.b_stats;
  $('sl').textContent = 'Last log: ' + s.last_log;
  $('op').textContent = s.op_logs || '';
  $('dbg').textContent = s.debug || '';
  $('tb').innerHTML = (s.rows || []).map(row).join('');
}

async function start() {
  const q = `/api/start?a=${encodeURIComponent($('a').value)}&pa=${$('pa').value}&b=${encodeURIComponent($('b').value)}&pb=${$('pb').value}&d=${$('d').value}`;
  await api(q);
  refresh();
}

async function stop() {
  await api('/api/stop');
  refresh();
}

document.getElementById('app').innerHTML = `
<div class="c">
  <div class="r">
    <div><label>Target A</label><input id="a" class="wide" value="1.1.1.1"></div>
    <div><label>Port A</label><input id="pa" class="small" type="number" value="80"></div>
    <div><label>Target B</label><input id="b" class="wide" value="8.8.8.8"></div>
    <div><label>Port B</label><input id="pb" class="small" type="number" value="80"></div>
    <div><label>Duration</label><input id="d" class="small" type="number" value="10"></div>
    <div><button onclick="start()">Start</button></div>
    <div><button onclick="stop()">Stop</button></div>
    <div><button onclick="refresh()">Refresh</button></div>
  </div>
</div>
<div class="c"><div class="s"><span class="p" id="st">Status: -</span><span class="p" id="sa">A: -</span><span class="p" id="sb">B: -</span><span class="p" id="sl">Last log: -</span></div></div>
<div class="c"><table><thead><tr><th>Time</th><th>A</th><th>B</th><th>A Bar</th><th>B Bar</th><th>Note</th></tr></thead><tbody id="tb"></tbody></table></div>
<div class="c"><div>Operational Logs</div><div id="op" class="box"></div></div>
<div class="c"><div>Debug Log</div><div id="dbg" class="box dbg"></div></div>
`;

setInterval(refresh, 1000);
refresh();
