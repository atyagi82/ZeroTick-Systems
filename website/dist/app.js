const ACCESS_KEY = 'zerotick';
const state = { market: null, source: null, symbol: 'RELIANCE', range: 'all', file: null };
const $ = selector => document.querySelector(selector);
const $$ = selector => [...document.querySelectorAll(selector)];

function unlock() {
  $('#login').hidden = true;
  $('#portal').hidden = false;
  sessionStorage.setItem('zt-access', '1');
  loadData();
}

$('#login-form').addEventListener('submit', event => {
  event.preventDefault();
  if ($('#password').value === ACCESS_KEY) unlock();
  else { $('#login-error').textContent = 'Access key not recognized.'; $('#password').select(); }
});
$('#lock').addEventListener('click', () => { sessionStorage.removeItem('zt-access'); location.reload(); });
if (sessionStorage.getItem('zt-access') === '1') unlock();

async function loadData() {
  if (state.market) return;
  try {
    const [market, source] = await Promise.all([
      fetch('data/market.json').then(response => response.json()),
      fetch('data/source.json').then(response => response.json())
    ]);
    state.market = market; state.source = source;
    setupMarkets(); setupSource(); showDocument('DesignDocuments/High Level Design.md');
  } catch (error) { console.error(error); }
}

function showView(name) {
  $$('.view').forEach(view => view.classList.toggle('active-view', view.id === `view-${name}`));
  $$('.nav-item').forEach(item => item.classList.toggle('active', item.dataset.view === name));
  $('#current-view').textContent = ` / ${name.toUpperCase()}`;
  $('.sidebar').classList.remove('open');
  if (name === 'markets') requestAnimationFrame(drawCharts);
}
$$('.nav-item').forEach(item => item.addEventListener('click', () => showView(item.dataset.view)));
$$('[data-jump]').forEach(item => item.addEventListener('click', () => showView(item.dataset.jump)));
$('#menu').addEventListener('click', () => $('.sidebar').classList.toggle('open'));

function setupMarkets() {
  const select = $('#symbol-select');
  Object.entries(state.market.stocks).sort((a,b) => a[0].localeCompare(b[0])).forEach(([symbol, stock]) => {
    const option = document.createElement('option'); option.value = symbol;
    option.textContent = `${symbol} · ${stock.name}`; option.selected = symbol === state.symbol;
    select.append(option);
  });
  select.addEventListener('change', () => { state.symbol = select.value; updateMarket(); });
  $$('.range-buttons button').forEach(button => button.addEventListener('click', () => {
    state.range = button.dataset.range;
    $$('.range-buttons button').forEach(item => item.classList.toggle('active', item === button));
    updateMarket();
  }));
  updateMarket();
}

function visibleHistory() {
  const rows = state.market.stocks[state.symbol].history;
  return state.range === 'all' ? rows : rows.slice(-Number(state.range));
}

function formatINR(value) { return new Intl.NumberFormat('en-IN', {style:'currency', currency:'INR', maximumFractionDigits:2}).format(value); }
function updateMarket() {
  const stock = state.market.stocks[state.symbol], rows = stock.history, last = rows.at(-1), previous = rows.at(-2);
  const change = previous ? (last.close / previous.close - 1) * 100 : 0;
  $('#last-close').textContent = formatINR(last.close); $('#last-date').textContent = last.date;
  $('#daily-change').textContent = `${change >= 0 ? '+' : ''}${change.toFixed(2)}%`;
  $('#daily-change').className = change >= 0 ? 'positive' : 'negative';
  $('#rsi-value').textContent = last.rsi14?.toFixed(2) ?? '—';
  $('#rsi-state').textContent = last.rsi14 >= 70 ? 'Overbought zone' : last.rsi14 <= 30 ? 'Oversold zone' : 'Neutral zone';
  $('#sma-value').textContent = last.sma20 ? formatINR(last.sma20) : '—';
  $('#chart-name').textContent = `${state.symbol} · ${stock.name}`; $('#chart-sector').textContent = stock.sector;
  drawCharts();
}

function canvasSetup(canvas) {
  const ratio = devicePixelRatio || 1, rect = canvas.getBoundingClientRect();
  canvas.width = Math.max(600, rect.width * ratio); canvas.height = Number(canvas.getAttribute('height')) * ratio;
  const ctx = canvas.getContext('2d'); ctx.scale(ratio, ratio);
  return {ctx, width: canvas.width / ratio, height: canvas.height / ratio};
}

function line(ctx, points, color, width=2) {
  if (!points.length) return; ctx.beginPath(); ctx.strokeStyle=color; ctx.lineWidth=width;
  points.forEach(([x,y],index) => index ? ctx.lineTo(x,y) : ctx.moveTo(x,y)); ctx.stroke();
}

function drawCharts() {
  if (!state.market || !$('#price-chart').offsetParent) return;
  const rows = visibleHistory(), pad = {l:54,r:18,t:16,b:28};
  const priceCanvas = canvasSetup($('#price-chart')), ctx = priceCanvas.ctx, w=priceCanvas.width, h=priceCanvas.height;
  const values = rows.flatMap(row => [row.close, row.sma20].filter(Boolean));
  const min = Math.min(...values), max = Math.max(...values), span = max-min || 1;
  ctx.clearRect(0,0,w,h); ctx.font='11px DM Mono'; ctx.fillStyle='#71857d';
  for(let i=0;i<5;i++){const y=pad.t+(h-pad.t-pad.b)*i/4;ctx.strokeStyle='#23372f';ctx.beginPath();ctx.moveTo(pad.l,y);ctx.lineTo(w-pad.r,y);ctx.stroke();ctx.fillText((max-span*i/4).toFixed(0),4,y+4)}
  const x=i=>pad.l+(w-pad.l-pad.r)*i/Math.max(1,rows.length-1), y=v=>pad.t+(max-v)/span*(h-pad.t-pad.b);
  line(ctx, rows.map((row,i)=>[x(i),y(row.close)]), '#b8ff5b', 2);
  line(ctx, rows.map((row,i)=>row.sma20?[x(i),y(row.sma20)]:null).filter(Boolean), '#42d9a1', 1.3);
  [0,Math.floor(rows.length/2),rows.length-1].forEach(i=>{ctx.fillText(rows[i]?.date.slice(5)||'',x(i)-24,h-5)});

  const rsiCanvas=canvasSetup($('#rsi-chart')), rc=rsiCanvas.ctx, rw=rsiCanvas.width, rh=rsiCanvas.height;
  rc.clearRect(0,0,rw,rh); const ry=v=>10+(100-v)/100*(rh-28), rx=i=>pad.l+(rw-pad.l-pad.r)*i/Math.max(1,rows.length-1);
  [30,50,70].forEach(v=>{rc.strokeStyle=v===50?'#23372f':'#55482b';rc.setLineDash(v===50?[]:[4,5]);rc.beginPath();rc.moveTo(pad.l,ry(v));rc.lineTo(rw-pad.r,ry(v));rc.stroke();rc.setLineDash([]);rc.fillStyle='#71857d';rc.font='10px DM Mono';rc.fillText(v,18,ry(v)+3)});
  line(rc, rows.map((row,i)=>row.rsi14!=null?[rx(i),ry(row.rsi14)]:null).filter(Boolean), '#c5a7ff', 1.6);
}
addEventListener('resize', () => { clearTimeout(window.chartTimer); window.chartTimer=setTimeout(drawCharts,120); });

function setupSource() {
  renderFileList(''); $('#file-search').addEventListener('input', event => renderFileList(event.target.value));
  $('#copy-code').addEventListener('click', async () => { if(state.file) await navigator.clipboard.writeText(state.file.content); });
  openFile(state.source.files.find(file => file.path === 'src/main.cpp') || state.source.files[0]);
}
function renderFileList(query) {
  const list=$('#file-list'); list.innerHTML='';
  state.source.files.filter(file=>file.path.toLowerCase().includes(query.toLowerCase())).forEach(file=>{
    const button=document.createElement('button'); button.className='file-button'; button.textContent=file.path;
    button.addEventListener('click',()=>openFile(file)); list.append(button);
  });
}
function openFile(file) {
  state.file=file; $('#file-path').textContent=file.path; $('#code-content').textContent=file.content;
  $$('.file-button').forEach(button=>button.classList.toggle('active',button.textContent===file.path));
}

function escapeHtml(text){return text.replace(/[&<>]/g,char=>({'&':'&amp;','<':'&lt;','>':'&gt;'}[char]));}
function markdown(text) {
  let code=false, list=false, html='';
  for(const raw of text.split('\n')){
    if(raw.startsWith('```')){if(list){html+='</ul>';list=false;}html+=code?'</code></pre>':'<pre><code>';code=!code;continue;}
    if(code){html+=escapeHtml(raw)+'\n';continue;}
    const heading=raw.match(/^(#{1,4})\s+(.*)/); if(heading){if(list){html+='</ul>';list=false;}const level=heading[1].length;html+=`<h${level}>${escapeHtml(heading[2])}</h${level}>`;continue;}
    const bullet=raw.match(/^[-*]\s+(.*)/); if(bullet){if(!list){html+='<ul>';list=true;}html+=`<li>${escapeHtml(bullet[1])}</li>`;continue;}
    if(list){html+='</ul>';list=false;} if(!raw.trim()){continue;}
    html+=`<p>${escapeHtml(raw).replace(/`([^`]+)`/g,'<code>$1</code>')}</p>`;
  }
  return html+(list?'</ul>':'')+(code?'</code></pre>':'');
}
function showDocument(path){if(!state.source)return;const file=state.source.files.find(item=>item.path===path);if(file)$('#document').innerHTML=markdown(file.content);}
$$('.doc-tabs button').forEach(button=>button.addEventListener('click',()=>{$$('.doc-tabs button').forEach(item=>item.classList.toggle('active',item===button));showDocument(button.dataset.doc);}));
