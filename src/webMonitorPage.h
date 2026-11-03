#ifndef WEB_MONITOR_PAGE_H
#define WEB_MONITOR_PAGE_H

// Live miner monitor served at "/" by webConfig.cpp: one self-contained page
// (no external files, works without internet), responsive for phones and
// computers, light/dark theme from the device. Reads /api/status every 3 s
// and /api/history (1h / 6h / 24h hashrate) every 30 s. UTF-8.
// Texts follow the browser's language (English, French, Spanish, German,
// Italian, Portuguese): elements with data-i="key" and the t() calls in the
// script, from the TR table; English is the fallback for anything missing.

#include <Arduino.h>

static const char MONITOR_HTML[] PROGMEM = R"rawliteral(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="theme-color" content="#0d1117">
<title>NerdMiner</title>
<style>
:root{--bg:#0d1117;--card:#161b22;--line:#30363d;--txt:#e6edf3;--mut:#8b949e;--acc:#f7931a;--ok:#3fb950;--warn:#d29922;--bad:#f85149;--grid:#21262d}
@media (prefers-color-scheme:light){:root{--bg:#f6f8fa;--card:#fff;--line:#d0d7de;--txt:#1f2328;--mut:#59636e;--grid:#eaeef2}}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--txt);font:15px/1.4 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;-webkit-text-size-adjust:100%}
.wrap{max-width:1100px;margin:0 auto;padding:16px max(16px,env(safe-area-inset-right)) 32px max(16px,env(safe-area-inset-left))}
header{display:flex;align-items:center;gap:12px;flex-wrap:wrap;margin-bottom:16px}
h1{font-size:20px;margin:0;font-weight:700;letter-spacing:.2px}
h1 b{color:var(--acc)}
.pill{display:inline-flex;align-items:center;gap:6px;padding:4px 10px;border-radius:999px;font-size:13px;font-weight:600;background:var(--card);border:1px solid var(--line)}
.dot{width:8px;height:8px;border-radius:50%;background:var(--mut)}
.ok .dot{background:var(--ok)}.warn .dot{background:var(--warn)}.bad .dot{background:var(--bad)}
.sp{flex:1}
a.btn{color:var(--txt);text-decoration:none;padding:6px 12px;border-radius:8px;border:1px solid var(--line);background:var(--card);font-size:14px}
a.btn:hover{border-color:var(--acc)}
.card{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:14px 16px;min-width:0}
.lbl{color:var(--mut);font-size:12px;text-transform:uppercase;letter-spacing:.6px;font-weight:600}
.val{font-size:22px;font-weight:700;margin-top:4px;font-variant-numeric:tabular-nums;overflow-wrap:anywhere}
.sub{color:var(--mut);font-size:13px;margin-top:2px;overflow-wrap:anywhere}
.val.s{font-size:17px}
.ok{color:var(--ok)}.bad{color:var(--bad)}
.pill.ok,.pill.bad{color:var(--txt)}
.hero{display:grid;grid-template-columns:minmax(220px,1fr) 2fr;gap:16px;margin-bottom:16px}
.big{font-size:clamp(40px,8vw,64px);font-weight:800;line-height:1.05;font-variant-numeric:tabular-nums;margin-top:6px}
.big small{font-size:.4em;font-weight:600;color:var(--mut);margin-left:6px}
.chart{position:relative;height:190px}
canvas{width:100%;height:100%;display:block}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(160px,1fr));gap:12px;margin-bottom:12px}
.bar{height:6px;background:var(--grid);border-radius:3px;margin-top:8px;overflow:hidden}
.bar i{display:block;height:100%;width:0;background:var(--ok);transition:width .4s}
.sig{display:inline-flex;gap:2px;align-items:flex-end;height:14px;margin-left:6px;vertical-align:-1px}
.sig i{width:4px;background:var(--grid);border-radius:1px}
.sig i.on{background:var(--ok)}
footer{color:var(--mut);font-size:12px;text-align:center;margin-top:18px}
body.stale .card{opacity:.5;transition:opacity .3s}
#off{display:none;background:var(--bad);color:#fff;padding:8px 12px;border-radius:8px;margin-bottom:12px;font-weight:600}
@media (max-width:420px){.t{display:none}}
@media (max-width:700px){.hero{grid-template-columns:1fr}.val{font-size:19px}.chart{height:150px}}
button.btn{font:inherit;cursor:pointer}button[aria-pressed="true"]{border-color:var(--acc);color:var(--acc)}
.range-stats{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:10px;margin-top:12px}.range-stats .lbl{display:block}.range-stats strong{display:block;font-size:16px;overflow-wrap:anywhere}
button.btn:disabled{opacity:.5;cursor:default}
#cv{touch-action:pan-y;cursor:crosshair}#cv:focus-visible{outline:2px solid var(--acc);outline-offset:2px}#chartCursor{position:absolute;top:8px;bottom:18px;width:1px;background:var(--acc);pointer-events:none}#chartReading{min-height:2.8em;margin-top:6px;overflow-wrap:anywhere}
</style>
</head>
<body>
<div class="wrap">
<header>
  <h1>&#9935; Nerd<b>Miner</b></h1>
  <span id="st" class="pill"><span class="dot"></span><span id="stt" data-i="connecting">Connecting&hellip;</span></span>
  <span class="sp"></span>
  <a class="btn" href="/settings" id="setb" title="Settings">&#9881;<span class="t"> <span data-i="settings">Settings</span></span></a>
</header>
<div id="off" data-i="off">Miner unreachable &mdash; retrying&hellip;</div>

<section class="hero">
  <div class="card">
    <div class="lbl" data-i="hashrate">Hashrate</div>
    <div class="big"><span id="hr">&ndash;</span><small id="hru">KH/s</small></div>
    <div class="sub"><span data-i="avg">Average since boot</span>: <b id="avg">&ndash;</b></div>
    <div class="sub"><span data-i="total">Total</span>: <b id="tot">&ndash;</b></div>
  </div>
  <div class="card">
    <div style="display:flex;align-items:center;justify-content:space-between;gap:8px">
      <div class="lbl" data-i="hashrate">Hashrate</div>
      <div id="ranges" role="group" aria-label="Hashrate history">
        <button type="button" class="btn" data-range="1h" aria-pressed="true">1 h</button>
        <button type="button" class="btn" data-range="6h" aria-pressed="false">6 h</button>
        <button type="button" class="btn" data-range="24h" aria-pressed="false">24 h</button>
      </div></div><div id="historyInfo" class="sub" role="status"></div>
    <div class="chart"><canvas id="cv" tabindex="0" role="img" aria-describedby="chartReading"></canvas><div id="chartCursor" hidden></div></div>
    <div id="chartReading" class="sub" role="status" aria-live="polite"></div>
    <div class="range-stats" aria-live="polite">
      <div><span class="lbl" data-i="rangeMin">Minimum</span><strong id="rangeMin">&ndash;</strong></div>
      <div><span class="lbl" data-i="rangeMax">Maximum</span><strong id="rangeMax">&ndash;</strong></div>
      <div><span class="lbl" data-i="rangeAvg">Average</span><strong id="rangeAvg">&ndash;</strong></div>
    </div>
    <div class="sub" id="rangeInfo"></div>
    <div style="margin-top:10px"><button type="button" class="btn" id="exportCsv" disabled data-i="exportCsv">Download CSV</button></div>
  </div>
</section>

<div class="grid">
  <div class="card"><div class="lbl" data-i="s32">32-bit shares</div><div class="val ok" id="s32">&ndash;</div><div class="sub"><span class="ok" data-i="accepted">accepted</span> &middot; <span class="bad" id="s32r">&ndash;</span></div></div>
  <div class="card"><div class="lbl" data-i="best">Best difficulty</div><div class="val" id="bd">&ndash;</div><div class="sub" data-i="alltime">all time</div></div>
  <div class="card"><div class="lbl" data-i="blocks">Blocks found</div><div class="val" id="blk">&ndash;</div><div class="sub">&#127920; <span data-i="oneday">maybe one day</span></div></div>
  <div class="card"><div class="lbl" data-i="pshares">Pool shares (low diff)</div><div class="val" id="acc">&ndash;</div><div class="sub"><span data-i="accepted">accepted</span> &middot; <span id="rej">&ndash;</span></div></div>
  <div class="card"><div class="lbl" data-i="mtime">Mining time</div><div class="val" id="mt">&ndash;</div><div class="sub"><span data-i="templates">templates</span>: <span id="tpl">&ndash;</span></div></div>
</div>

<section class="card" style="margin:12px 0" aria-label="Extended mining statistics">
  <div class="lbl" data-i="advanced">Extended statistics</div>
  <div class="grid" style="margin:10px 0 0">
    <div><div class="lbl" data-i="shareRate">Share acceptance</div><div class="val" id="shareRate">&#8211;</div><div class="sub" id="shareCounts">&#8211;</div></div>
    <div><div class="lbl" data-i="median">Hashrate median (1h)</div><div class="val" id="median">&#8211;</div></div>
    <div><div class="lbl" data-i="dips">Dips below 90% of median</div><div class="val" id="dips">&#8211;</div><div class="sub" data-i="dipsInfo">Last hour; individual samples</div></div>
    <div><div class="lbl" data-i="ram">Free RAM</div><div class="val" id="ram">&#8211;</div><div class="sub" id="ramInfo">&#8211;</div></div>
  </div>
  <div class="lbl" data-i="alerts">Status and warnings</div>
  <div id="alerts" class="sub" role="status" aria-live="polite" style="margin-top:6px">&#8211;</div>
</section>

<div class="grid">
  <div class="card"><div class="lbl">Bitcoin</div><div class="val" id="px">&ndash;</div><div class="sub"><span data-i="block">block</span> <span id="bh">&ndash;</span></div></div>
  <div class="card"><div class="lbl" data-i="time">Time</div><div class="val" id="tm">&ndash;</div><div class="sub" id="dt">&ndash;</div></div>
  <div class="card" id="batc" hidden><div class="lbl" data-i="battery">Battery</div><div class="val" id="bat">&ndash;</div><div class="sub" id="batv">&ndash;</div><div class="bar"><i id="batb"></i></div></div>
  <div class="card"><div class="lbl">Wi-Fi</div><div class="val s" id="ssid">&ndash;</div><div class="sub"><span id="rssi">&ndash;</span><span class="sig" id="sig"><i style="height:4px"></i><i style="height:7px"></i><i style="height:10px"></i><i style="height:14px"></i></span></div><div class="sub" id="ip">&ndash;</div></div>
  <div class="card"><div class="lbl" data-i="pool">Pool</div><div class="val s" id="pool">&ndash;</div><div class="sub" id="pinfo">&ndash;</div></div>
</div>

<footer><span id="ver">NerdMiner</span> &middot; <span data-i="up">up</span> <span id="up">&ndash;</span> &middot; <span data-i="heap">free heap</span> <span id="heap">&ndash;</span> &middot; <span data-i="updated">updated</span> <span id="ts">&ndash;</span></footer>
</div>
<script>
(function(){
var $=function(i){return document.getElementById(i)};
function set(i,v){var e=$(i);if(e)e.textContent=v}
var EN={settings:'Settings',connecting:'Connecting\u2026',off:'Miner unreachable \u2014 retrying\u2026',hashrate:'Hashrate',avg:'Average since boot',total:'Total',hour:'Last hour',
 s32:'32-bit shares',accepted:'accepted',rejected:'rejected',best:'Best difficulty',alltime:'all time',blocks:'Blocks found',oneday:'maybe one day',pshares:'Pool shares (low diff)',
 mtime:'Mining time',templates:'templates',block:'block',time:'Time',battery:'Battery',pool:'Pool',up:'up',heap:'free heap',updated:'updated',
 starting:'Starting',mining:'Mining',topool:'Connecting to pool',refused:'Wallet refused by the pool',offline:'Offline',notime:'time not set yet',
 backup:'backup',port:'port',diff:'diff',collecting:'Collecting data\u2026',live:'live',now:'now',min:'min',day:'d'};
var TR={
 fr:{settings:'R\u00e9glages',connecting:'Connexion\u2026',off:'Mineur injoignable \u2014 nouvel essai\u2026',hashrate:'Hashrate',avg:'Moyenne depuis le d\u00e9marrage',total:'Total',hour:'Derni\u00e8re heure',
  s32:'Partages 32 bits',accepted:'accept\u00e9s',rejected:'refus\u00e9s',best:'Meilleure difficult\u00e9',alltime:'depuis toujours',blocks:'Blocs trouv\u00e9s',oneday:'peut-\u00eatre un jour',pshares:'Partages pool (faible diff.)',
  mtime:'Temps de minage',templates:'mod\u00e8les',block:'bloc',time:'Heure',battery:'Batterie',pool:'Pool',up:'allum\u00e9 depuis',heap:'m\u00e9moire libre',updated:'mis \u00e0 jour',
  starting:'D\u00e9marrage',mining:'Minage',topool:'Connexion au pool',refused:'Portefeuille refus\u00e9 par le pool',offline:'Hors ligne',notime:'heure pas encore r\u00e9gl\u00e9e',
  backup:'secours',port:'port',diff:'diff.',collecting:'Collecte des donn\u00e9es\u2026',live:'direct',now:'maintenant',min:'min',day:'j'},
 es:{settings:'Ajustes',connecting:'Conectando\u2026',off:'Minero inaccesible \u2014 reintentando\u2026',hashrate:'Hashrate',avg:'Media desde el arranque',total:'Total',hour:'\u00daltima hora',
  s32:'Shares de 32 bits',accepted:'aceptados',rejected:'rechazados',best:'Mejor dificultad',alltime:'hist\u00f3rica',blocks:'Bloques encontrados',oneday:'quiz\u00e1 alg\u00fan d\u00eda',pshares:'Shares del pool (dif. baja)',
  mtime:'Tiempo minando',templates:'plantillas',block:'bloque',time:'Hora',battery:'Bater\u00eda',pool:'Pool',up:'encendido',heap:'memoria libre',updated:'actualizado',
  starting:'Iniciando',mining:'Minando',topool:'Conectando al pool',refused:'Monedero rechazado por el pool',offline:'Sin conexi\u00f3n',notime:'hora a\u00fan no ajustada',
  backup:'respaldo',port:'puerto',diff:'dif.',collecting:'Recogiendo datos\u2026',live:'en vivo',now:'ahora',min:'min'},
 de:{settings:'Einstellungen',connecting:'Verbinde\u2026',off:'Miner nicht erreichbar \u2014 neuer Versuch\u2026',hashrate:'Hashrate',avg:'Durchschnitt seit Start',total:'Gesamt',hour:'Letzte Stunde',
  s32:'32-Bit-Shares',accepted:'angenommen',rejected:'abgelehnt',best:'Beste Difficulty',alltime:'aller Zeiten',blocks:'Gefundene Bl\u00f6cke',oneday:'vielleicht eines Tages',pshares:'Pool-Shares (niedrige Diff.)',
  mtime:'Mining-Zeit',templates:'Vorlagen',block:'Block',time:'Uhrzeit',battery:'Akku',pool:'Pool',up:'l\u00e4uft seit',heap:'freier Speicher',updated:'aktualisiert',
  starting:'Startet',mining:'Mining',topool:'Verbinde mit Pool',refused:'Wallet vom Pool abgelehnt',offline:'Offline',notime:'Uhrzeit noch nicht gestellt',
  advanced:'Erweiterte Statistiken',shareRate:'Share-Erfolgsquote',median:'Hashrate-Median (1h)',dips:'Einbr\u00fcche unter 90 % des Medians',dipsInfo:'Letzte Stunde; einzelne Messwerte',ram:'Freier RAM',alerts:'Status und Warnungen',healthy:'Keine Warnungen',alertPool:'Pool-Verbindung unterbrochen',alertWallet:'Pool-Anmeldung fehlgeschlagen',alertHash:'Hashrate unter 70 % des Durchschnitts',alertReject:'Abgelehnte Shares \u00fcber 2 %',alertRam:'Freier RAM unter 30 KiB',alertRate:'Noch zu wenige Shares',backup:'Backup',port:'Port',diff:'Diff.',collecting:'Sammle Daten\u2026',live:'live',now:'jetzt',min:'Min.',day:'T'},
 it:{settings:'Impostazioni',connecting:'Connessione\u2026',off:'Miner non raggiungibile \u2014 nuovo tentativo\u2026',hashrate:'Hashrate',avg:'Media dall\'avvio',total:'Totale',hour:'Ultima ora',
  s32:'Share a 32 bit',accepted:'accettati',rejected:'rifiutati',best:'Miglior difficolt\u00e0',alltime:'di sempre',blocks:'Blocchi trovati',oneday:'forse un giorno',pshares:'Share del pool (diff. bassa)',
  mtime:'Tempo di mining',templates:'template',block:'blocco',time:'Ora',battery:'Batteria',pool:'Pool',up:'acceso da',heap:'memoria libera',updated:'aggiornato',
  starting:'Avvio',mining:'Mining',topool:'Connessione al pool',refused:'Wallet rifiutato dal pool',offline:'Offline',notime:'ora non ancora impostata',
  backup:'riserva',port:'porta',diff:'diff.',collecting:'Raccolta dati\u2026',live:'live',now:'ora',min:'min',day:'g'},
 pt:{settings:'Configura\u00e7\u00f5es',connecting:'Conectando\u2026',off:'Minerador inacess\u00edvel \u2014 tentando de novo\u2026',hashrate:'Hashrate',avg:'M\u00e9dia desde o in\u00edcio',total:'Total',hour:'\u00daltima hora',
  s32:'Shares de 32 bits',accepted:'aceitos',rejected:'recusados',best:'Melhor dificuldade',alltime:'de sempre',blocks:'Blocos encontrados',oneday:'talvez um dia',pshares:'Shares do pool (dif. baixa)',
  mtime:'Tempo minerando',templates:'modelos',block:'bloco',time:'Hora',battery:'Bateria',pool:'Pool',up:'ligado h\u00e1',heap:'mem\u00f3ria livre',updated:'atualizado',
  starting:'Iniciando',mining:'Minerando',topool:'Conectando ao pool',refused:'Carteira recusada pelo pool',offline:'Offline',notime:'hora ainda n\u00e3o definida',
  backup:'reserva',port:'porta',diff:'dif.',collecting:'Coletando dados\u2026',live:'ao vivo',now:'agora',min:'min'}};
var LANG='en';
(navigator.languages||[navigator.language||'en']).some(function(l){l=String(l||'').slice(0,2).toLowerCase();if(TR[l]||l==='en'){LANG=l;return true}return false});
EN.rangeMin='Minimum';EN.rangeMax='Maximum';EN.rangeAvg='Average';
EN.rangeSamples='Values';EN.rangeNote='Statistics of displayed values; 6 h / 24 h use 2 / 5 minute averages. Missing values excluded; zero included.';
TR.de.rangeMin='Minimum';TR.de.rangeMax='Maximum';TR.de.rangeAvg='Durchschnitt';
TR.de.rangeSamples='Werte';TR.de.rangeNote='Statistik der Diagrammwerte; 6 h / 24 h aus 2- / 5-Minuten-Mittelwerten. Fehlende Werte ausgeschlossen; Nullwerte enthalten.';
EN.exportCsv='Download CSV';TR.de.exportCsv='CSV herunterladen';
EN.chartHint='Hover, tap or use arrow keys to inspect values.';
EN.chartMissing='No measurement';EN.chartAgo='Approx. age';EN.chartMean='minute average';EN.chartSample='Sample';EN.chartBucket='bucket start';
TR.de.chartHint='Mit Maus, Antippen oder Pfeiltasten Messwerte anzeigen.';
TR.de.chartMissing='Kein Messwert';TR.de.chartAgo='Alter ca.';TR.de.chartMean='Minuten-Mittelwert';TR.de.chartSample='Messwert';TR.de.chartBucket='Abschnittsbeginn';
function t(k){var d=TR[LANG];return d&&d[k]||EN[k]||k}
document.documentElement.lang=LANG;
[].forEach.call(document.querySelectorAll('[data-i]'),function(e){e.textContent=t(e.getAttribute('data-i'))});
$('setb').title=t('settings');
var nf=new Intl.NumberFormat(LANG);
function hash(k){return k>=1000?[(k/1000).toFixed(2),'MH/s']:[k.toFixed(1),'KH/s']}
function total(m){return m>=1e6?(m/1e6).toFixed(2)+' TH':m>=1e3?(m/1e3).toFixed(2)+' GH':nf.format(Math.round(m))+' MH'}
function diff(d){if(!(d>0))return '0';if(d<1)return String(+d.toPrecision(2));var u=['','k','M','G','T','P'],i=0;while(d>=1000&&i<u.length-1){d/=1000;i++}return (d<10?d.toFixed(2):d<100?d.toFixed(1):d.toFixed(0))+u[i]}
function dur(s){s=Math.floor(s||0);var d=Math.floor(s/86400),h=Math.floor(s%86400/3600),m=Math.floor(s%3600/60),x=s%60;function p(n){return (n<10?'0':'')+n}return (d?d+t('day')+' ':'')+p(h)+':'+p(m)+':'+p(x)}
function state(j){
  var st=$('st'),c='warn',x=t('starting');
  if(j.pool&&j.pool.authorized===false){c='bad';x=t('refused')}
  else if(j.state==='mining'&&j.pool&&j.pool.subscribed){c='ok';x=t('mining')}
  else if(j.state==='connecting'||(j.pool&&!j.pool.subscribed)){c='warn';x=t('topool')}
  st.className='pill '+c;set('stt',x)}
function update(j){
  $('off').style.display='none';state(j);
  var h=hash(j.hashrate_khs||0);set('hr',h[0]);set('hru',h[1]);
  var a=hash(j.hashrate_avg_khs||0);set('avg',a[0]+' '+a[1]);
  set('tot',total(j.total_mhashes||0));
  set('s32',nf.format(j.shares_32bit||0));set('s32r',nf.format(j.shares_32bit_rejected||0)+' '+t('rejected'));set('bd',diff(j.best_diff));set('blk',nf.format(j.blocks_found||0));
  set('acc',nf.format(j.shares_accepted||0));set('rej',nf.format(j.shares_rejected||0)+' '+t('rejected'));
  set('mt',dur(j.mining_time_s));set('tpl',nf.format(j.templates||0));
  var b=j.btc||{};set('px',b.price?(b.prefix||'')+nf.format(b.price):'\u2013');set('bh',b.block_height?nf.format(b.block_height):'\u2013');
  var c=j.clock||{};set('tm',c.synced?c.time:'\u2013');set('dt',c.synced?c.date:t('notime'));
  if(j.battery&&j.battery.mv>0){$('batc').hidden=false;var p=Math.max(0,Math.min(100,j.battery.percent|0));set('bat',p+' %');set('batv',(j.battery.mv/1000).toFixed(2)+' V');
    var bb=$('batb');bb.style.width=p+'%';bb.style.background=p>50?'var(--ok)':p>20?'var(--warn)':'var(--bad)'}
  var w=j.wifi||{};set('ssid',w.ssid||'\u2013');set('rssi',w.rssi?w.rssi+' dBm':'\u2013');set('ip',w.ip||'\u2013');
  var q=w.rssi>=-55?4:w.rssi>=-65?3:w.rssi>=-75?2:w.rssi<0?1:0,bars=$('sig').children;for(var i=0;i<4;i++)bars[i].className=i<q?'on':'';
  var po=j.pool||{};set('pool',po.address||'\u2013');
  set('pinfo',(po.backup?t('backup')+' \u00b7 ':'')+t('port')+' '+(po.port||'\u2013')+' \u00b7 '+t('diff')+' '+(po.difficulty!=null?diff(po.difficulty):'\u2013'));
  set('ver','NerdMiner '+(j.version||''));set('up',dur(j.uptime_s));set('heap',j.free_heap?Math.round(j.free_heap/1024)+' KB':'\u2013');
  var accepted=Number(j.shares_accepted)||0,rejected=Number(j.shares_rejected)||0,n=accepted+rejected;
  set('shareRate',n?((100*accepted/n).toFixed(2)+' %'):'\u2013');
  set('shareCounts',nf.format(accepted)+' / '+nf.format(rejected));
  var heap=Number(j.free_heap)||0;
  set('ram',nf.format(Math.round(heap/1024))+' KiB');
  set('ramInfo',heap?nf.format(heap)+' bytes':'\u2013');
  var warnings=[],po=j.pool||{},avg=Number(j.hashrate_avg_khs)||0,current=Number(j.hashrate_khs)||0;
  if(po.authorized===false)warnings.push(t('alertWallet'));
  else if(j.state==='mining'&&(!po.subscribed||po.authorized!==true))warnings.push(t('alertPool'));
  if(j.state==='mining'&&avg>0&&current<avg*.70)warnings.push(t('alertHash'));
  if(n>=50&&rejected/n>.02)warnings.push(t('alertReject'));
  if(heap>0&&heap<30720)warnings.push(t('alertRam'));
  var el=$('alerts');el.textContent=warnings.length?warnings.join(' \u00b7 '):t('healthy');
  el.style.color=warnings.length?'var(--bad)':'var(--ok)';
  set('ts',new Date().toLocaleTimeString(LANG==='en'?'en-GB':LANG));
  live.push(j.hashrate_khs||0);if(live.length>40)live.shift();
}
var hist=[],hourHist=[],step=30,live=[],range='1h',historyRequest=0,historyAge=0;
EN.historyError='History unavailable \u2014 retrying\u2026';EN.historyInfo='History since boot; gaps = missing data';TR.de.historyError='Verlauf nicht erreichbar \u2014 neuer Versuch\u2026';TR.de.historyInfo='Verlauf seit Neustart; L\u00fccken = fehlende Daten';
var chartGeometry=null,chartIndex=-1,historyReceivedAt=Date.now();
function hideChartReading(){
  chartIndex=-1;$('chartCursor').hidden=true;set('chartReading',t('chartHint'));
}
function historyIndexAt(x,start,spacing,count){
  if(!count||!(spacing>0)||x<start-spacing/2||x>start+(count-0.5)*spacing)return -1;
  return Math.max(0,Math.min(count-1,Math.round((x-start)/spacing)));
}
function showChartReading(index){
  var g=chartGeometry;if(!g||index<0||index>=hist.length){hideChartReading();return}
  chartIndex=index;
  var value=hist[index],valid=typeof value==='number'&&isFinite(value)&&value>=0;
  var h=valid?hash(value):null,rate=valid?h[0]+' '+h[1]:t('chartMissing');
  var seconds=Math.round(historyAge+(hist.length-1-index)*step+Math.max(0,Date.now()-historyReceivedAt)/1000);
  var age=seconds<60?seconds+' s':seconds<3600?Math.floor(seconds/60)+' min '+seconds%60+' s':Math.floor(seconds/3600)+' h '+Math.floor(seconds%3600/60)+' min';
  var kind=step===30?t('chartSample'):(step/60)+' '+t('chartMean');
  set('chartReading',rate+' \u00b7 '+t('chartAgo')+' '+age+' ('+t('chartBucket')+') \u00b7 '+kind);
  var cursor=$('chartCursor');cursor.style.left=(g.start+index*g.spacing)+'px';cursor.hidden=false;
}
function chartPointer(event){
  var g=chartGeometry;if(!g)return;
  var rect=$('cv').getBoundingClientRect(),x=event.clientX-rect.left,y=event.clientY-rect.top;
  if(x<44||x>rect.width||y<8||y>rect.height-18){hideChartReading();return}
  showChartReading(historyIndexAt(x,g.start,g.spacing,hist.length));
}
$('cv').setAttribute('aria-label',t('hashrate')+'. '+t('chartHint'));
$('cv').addEventListener('pointermove',chartPointer);
$('cv').addEventListener('pointerdown',chartPointer);
$('cv').addEventListener('pointerleave',function(e){if(e.pointerType!=='touch')hideChartReading()});
$('cv').addEventListener('pointercancel',hideChartReading);
$('cv').addEventListener('blur',hideChartReading);
$('cv').addEventListener('keydown',function(e){
  if(e.key==='Escape'){hideChartReading();return}
  if(!chartGeometry||!hist.length)return;
  if(e.key!=='ArrowLeft'&&e.key!=='ArrowRight'&&e.key!=='Home'&&e.key!=='End')return;
  e.preventDefault();
  var i=chartIndex<0?hist.length-1:chartIndex;
  if(e.key==='Home')i=0;else if(e.key==='End')i=hist.length-1;
  else if(chartIndex>=0)i+=e.key==='ArrowLeft'?-1:1;
  showChartReading(Math.max(0,Math.min(hist.length-1,i)));
});
function draw(){
  chartGeometry=null;hideChartReading();
  var cv=$('cv'),r=cv.getBoundingClientRect(),dp=window.devicePixelRatio||1;
  if(!r.width)return;cv.width=r.width*dp;cv.height=r.height*dp;
  var g=cv.getContext('2d');g.setTransform(dp,0,0,dp,0,0);
  var W=r.width,H=r.height,L=44,B=18,T=8,cs=getComputedStyle(document.documentElement);
  var data=hist,col=cs.getPropertyValue('--acc').trim(),mut=cs.getPropertyValue('--mut').trim(),grd=cs.getPropertyValue('--grid').trim();
  g.clearRect(0,0,W,H);g.font='11px system-ui,sans-serif';g.fillStyle=mut;
  if(!data.length){g.fillText(t('collecting'),L,H/2);return}
  var valid=data.filter(function(v){return typeof v==='number'&&isFinite(v)});if(!valid.length){g.fillText(t('collecting'),L,H/2);return}var mx=Math.max.apply(null,valid),mn=Math.min.apply(null,valid);if(mx-mn<mx*0.1){mn=Math.max(0,mx*0.9);}if(mx<=mn)mx=mn+1;
  var pad=(mx-mn)*0.15;mx+=pad;mn=Math.max(0,mn-pad);
  function y(v){return T+(H-T-B)*(1-(v-mn)/(mx-mn))}
  g.strokeStyle=grd;g.lineWidth=1;g.textAlign='right';g.textBaseline='middle';
  for(var k=0;k<=3;k++){var v=mn+(mx-mn)*k/3,yy=y(v);g.beginPath();g.moveTo(L,yy);g.lineTo(W,yy);g.stroke();g.fillText(mx>=1000?(v/1000).toFixed(2)+'M':v.toFixed(0)+'k',L-6,yy)}
  var n=data.length,spanSeconds=range==='24h'?86400:range==='6h'?21600:3600;
  var dx=(W-L)*step/spanSeconds,startX=W-(n-1)*dx-(W-L)*historyAge/spanSeconds;
  chartGeometry={start:startX,spacing:dx};
  g.beginPath();var connected=false;
  for(var i=0;i<n;i++){
    if(data[i]===null){connected=false;continue}
    var xx=startX+i*dx,yv=y(data[i]);
    if(connected)g.lineTo(xx,yv);else g.moveTo(xx,yv);connected=true;
  }
  g.strokeStyle=col;g.lineWidth=2;g.lineJoin='round';g.stroke();
  g.fillStyle=col;for(var j=0;j<n;j++){if(data[j]!==null&&(n===1||(j===0||data[j-1]===null)&&(j===n-1||data[j+1]===null))){g.beginPath();g.arc(startX+j*dx,y(data[j]),2.5,0,Math.PI*2);g.fill()}}
  g.fillStyle=mut;g.textAlign='left';g.textBaseline='alphabetic';
  g.fillText('\u2212'+range,L,H-4);g.textAlign='right';g.fillText(t('now'),W,H-4);
}
// JSON with a time limit: a stick that dropped off the WiFi otherwise keeps
// the request (and the page) waiting for a minute or more.
function getJSON(url,ms){
  var ctl=window.AbortController?new AbortController():null;
  var t=setTimeout(function(){if(ctl)ctl.abort()},ms);
  return fetch(url,{cache:'no-store',signal:ctl?ctl.signal:undefined})
    .then(function(r){clearTimeout(t);if(!r.ok)throw new Error('HTTP '+r.status);return r.json()},
          function(e){clearTimeout(t);throw e})}
function offline(){
  $('off').style.display='block';document.body.classList.add('stale');
  var st=$('st');st.className='pill bad';set('stt',t('offline'))}
var pollTimer=0,histTimer=0;
function poll(){
  clearTimeout(pollTimer);
  if(document.hidden)return; // resumes on visibilitychange
  getJSON('/api/status',5000).then(function(j){
    document.body.classList.remove('stale');
    try{update(j);if(hist.length<2)draw()}catch(e){if(window.console)console.error(e)} // a display bug is not "offline"
  },offline).then(function(){pollTimer=setTimeout(poll,3000)})}
function updateRangeStats(){
  var values=hist.filter(function(v){return typeof v==='number'&&isFinite(v)&&v>=0});
  var label=range==='24h'?'24 h':range==='6h'?'6 h':'1 h';
  if(!values.length){
    set('rangeMin','\u2013');set('rangeMax','\u2013');set('rangeAvg','\u2013');
    set('rangeInfo',label+' \u00b7 '+t('collecting'));return;
  }
  var min=values[0],max=min,sum=0;
  values.forEach(function(v){min=Math.min(min,v);max=Math.max(max,v);sum+=v});
  function format(v){var h=hash(v);return h[0]+' '+h[1]}
  set('rangeMin',format(min));set('rangeMax',format(max));set('rangeAvg',format(sum/values.length));
  set('rangeInfo',label+' \u00b7 '+t('rangeSamples')+': '+nf.format(values.length)+' / '+nf.format(hist.length)+' \u00b7 '+t('rangeNote'));
}
function updateHistoryStats(){
  var a=hourHist.filter(function(x){return typeof x==='number'&&isFinite(x)&&x>0}).slice().sort(function(x,y){return x-y});
  if(!a.length){set('median','\u2013');set('dips','\u2013');return}
  var m=a.length,median=m%2?a[(m-1)/2]:(a[m/2-1]+a[m/2])/2;
  set('median',median.toFixed(1)+' KH/s');
  var dips=hourHist.filter(function(x){return typeof x==='number'&&isFinite(x)&&x>0&&x<median*.9}).length;
  set('dips',nf.format(dips)+' / '+nf.format(hourHist.length));
}
// Export only the displayed snapshot. Relative bucket offsets avoid invented
// wall-clock measurement timestamps; the device API supplies no UTC times.
function makeHistoryCsv(data,selected,interval,exportedAt){
  var comma=LANG==='de'||LANG==='fr'||LANG==='es'||LANG==='it'||LANG==='pt';
  var rows=['export_utc;range;interval_s;offset_to_latest_bucket_s;hashrate_khs;status;value_kind'];
  data.forEach(function(value,i){
    var valid=typeof value==='number'&&isFinite(value)&&value>=0;
    var rate=valid?value.toFixed(1):'';
    if(comma)rate=rate.replace('.',',');
    rows.push([exportedAt,selected,interval,(i-data.length+1)*interval,rate,valid?'present':'missing',interval===30?'sample':'bucket_mean'].join(';'));
  });
  return '\uFEFF'+rows.join('\r\n')+'\r\n';
}
function syncExportButton(){
  $('exportCsv').disabled=!hist.some(function(v){return typeof v==='number'&&isFinite(v)&&v>=0});
}
$('exportCsv').addEventListener('click',function(){
  if($('exportCsv').disabled)return;
  var stamp=new Date().toISOString(),blob=new Blob([makeHistoryCsv(hist,range,step,stamp)],{type:'text/csv;charset=utf-8'});
  var url=URL.createObjectURL(blob),link=document.createElement('a');
  link.href=url;link.download='NerdMiner-'+range+'-'+stamp.replace(/[:.]/g,'-')+'.csv';
  document.body.appendChild(link);link.click();link.remove();
  setTimeout(function(){URL.revokeObjectURL(url)},10000);
});
function history(){
  clearTimeout(histTimer);
  if(document.hidden)return;
  var request=++historyRequest,selected=range;
  getJSON('/api/history?range='+selected,5000).then(function(h){
    if(request!==historyRequest)return;
    historyReceivedAt=Date.now();step=h.interval_s||30;hist=h.khs||[];hourHist=h.khs_1h||[];historyAge=h.age_s||0;
    set('historyInfo',t('historyInfo'));updateHistoryStats();updateRangeStats();syncExportButton();draw();
  },function(){if(request===historyRequest){set('historyInfo',t('historyError'));$('exportCsv').disabled=true}})
  .then(function(){if(request===historyRequest)histTimer=setTimeout(history,30000)})}
[].forEach.call(document.querySelectorAll('[data-range]'),function(button){
  button.addEventListener('click',function(){
    range=button.getAttribute('data-range');hist=[];updateRangeStats();syncExportButton();draw();
    [].forEach.call(document.querySelectorAll('[data-range]'),function(b){b.setAttribute('aria-pressed',String(b===button))});
    history();
  });
});
// Nothing is fetched while the page is in the background (phone locked,
// other tab): no battery use, no load on the stick. Fresh data on return.
document.addEventListener('visibilitychange',function(){if(!document.hidden){poll();history()}});
window.addEventListener('resize',draw);
if(window.matchMedia){var mq=matchMedia('(prefers-color-scheme: light)');if(mq.addEventListener)mq.addEventListener('change',draw);else if(mq.addListener)mq.addListener(draw)}
hideChartReading();updateRangeStats();poll();history();
})();
</script>
</body>
</html>
)rawliteral";

#endif // WEB_MONITOR_PAGE_H
