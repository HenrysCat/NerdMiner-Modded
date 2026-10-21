#ifndef WEB_MONITOR_PAGE_H
#define WEB_MONITOR_PAGE_H

// Live miner monitor served at "/" by webConfig.cpp: one self-contained page
// (no external files, works without internet), responsive for phones and
// computers, light/dark theme from the device. Reads /api/status every 3 s
// and /api/history (last hour of hashrate) every 30 s. UTF-8.

#include <Arduino.h>

static const char MONITOR_HTML[] PROGMEM = R"rawliteral(<!doctype html>
<html lang="fr">
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
</style>
</head>
<body>
<div class="wrap">
<header>
  <h1>&#9935; Nerd<b>Miner</b></h1>
  <span id="st" class="pill"><span class="dot"></span><span id="stt">Connexion&hellip;</span></span>
  <span class="sp"></span>
  <a class="btn" href="/settings" title="R&eacute;glages">&#9881;<span class="t"> R&eacute;glages</span></a>
</header>
<div id="off">Stick injoignable &mdash; nouvel essai en cours&hellip;</div>

<section class="hero">
  <div class="card">
    <div class="lbl">Hashrate</div>
    <div class="big"><span id="hr">&ndash;</span><small id="hru">KH/s</small></div>
    <div class="sub">Moyenne depuis le d&eacute;marrage : <b id="avg">&ndash;</b></div>
    <div class="sub">Total : <b id="tot">&ndash;</b></div>
  </div>
  <div class="card">
    <div class="lbl">Derni&egrave;re heure</div>
    <div class="chart"><canvas id="cv"></canvas></div>
  </div>
</section>

<div class="grid">
  <div class="card"><div class="lbl">Shares 32 bits</div><div class="val ok" id="s32">&ndash;</div><div class="sub"><span class="ok">accept&eacute;es</span> &middot; <span class="bad" id="s32r">&ndash;</span></div></div>
  <div class="card"><div class="lbl">Meilleure diff</div><div class="val" id="bd">&ndash;</div><div class="sub">depuis toujours</div></div>
  <div class="card"><div class="lbl">Blocs trouv&eacute;s</div><div class="val" id="blk">&ndash;</div><div class="sub">&#127920; un jour peut-&ecirc;tre</div></div>
  <div class="card"><div class="lbl">Shares pool (diff. basse)</div><div class="val" id="acc">&ndash;</div><div class="sub">accept&eacute;es &middot; <span id="rej">&ndash;</span></div></div>
  <div class="card"><div class="lbl">Temps de minage</div><div class="val" id="mt">&ndash;</div><div class="sub">templates : <span id="tpl">&ndash;</span></div></div>
</div>

<div class="grid">
  <div class="card"><div class="lbl">Bitcoin</div><div class="val" id="px">&ndash;</div><div class="sub">bloc <span id="bh">&ndash;</span></div></div>
  <div class="card"><div class="lbl">Heure</div><div class="val" id="tm">&ndash;</div><div class="sub" id="dt">&ndash;</div></div>
  <div class="card" id="batc" hidden><div class="lbl">Batterie</div><div class="val" id="bat">&ndash;</div><div class="sub" id="batv">&ndash;</div><div class="bar"><i id="batb"></i></div></div>
  <div class="card"><div class="lbl">Wi-Fi</div><div class="val s" id="ssid">&ndash;</div><div class="sub"><span id="rssi">&ndash;</span><span class="sig" id="sig"><i style="height:4px"></i><i style="height:7px"></i><i style="height:10px"></i><i style="height:14px"></i></span></div><div class="sub" id="ip">&ndash;</div></div>
  <div class="card"><div class="lbl">Pool</div><div class="val s" id="pool">&ndash;</div><div class="sub" id="pinfo">&ndash;</div></div>
</div>

<footer><span id="ver">NerdMiner</span> &middot; en marche depuis <span id="up">&ndash;</span> &middot; m&eacute;moire libre <span id="heap">&ndash;</span> &middot; mis &agrave; jour <span id="ts">&ndash;</span></footer>
</div>
<script>
(function(){
var $=function(i){return document.getElementById(i)};
function set(i,v){var e=$(i);if(e)e.textContent=v}
var nf=new Intl.NumberFormat('fr-FR');
function hash(k){return k>=1000?[(k/1000).toFixed(2),'MH/s']:[k.toFixed(1),'KH/s']}
function total(m){return m>=1e6?(m/1e6).toFixed(2)+' TH':m>=1e3?(m/1e3).toFixed(2)+' GH':nf.format(Math.round(m))+' MH'}
function diff(d){if(!(d>0))return '0';if(d<1)return String(+d.toPrecision(2));var u=['','k','M','G','T','P'],i=0;while(d>=1000&&i<u.length-1){d/=1000;i++}return (d<10?d.toFixed(2):d<100?d.toFixed(1):d.toFixed(0))+u[i]}
function dur(s){s=Math.floor(s||0);var d=Math.floor(s/86400),h=Math.floor(s%86400/3600),m=Math.floor(s%3600/60),x=s%60;function p(n){return (n<10?'0':'')+n}return (d?d+' j ':'')+p(h)+':'+p(m)+':'+p(x)}
function state(j){
  var st=$('st'),c='warn',t='Démarrage';
  if(j.state==='mining'&&j.pool&&j.pool.subscribed){c='ok';t='Minage'}
  else if(j.state==='connecting'||(j.pool&&!j.pool.subscribed)){c='warn';t='Connexion au pool'}
  st.className='pill '+c;set('stt',t)}
function update(j){
  $('off').style.display='none';state(j);
  var h=hash(j.hashrate_khs||0);set('hr',h[0]);set('hru',h[1]);
  var a=hash(j.hashrate_avg_khs||0);set('avg',a[0]+' '+a[1]);
  set('tot',total(j.total_mhashes||0));
  set('s32',nf.format(j.shares_32bit||0));set('s32r',nf.format(j.shares_32bit_rejected||0)+' refusées');set('bd',diff(j.best_diff));set('blk',nf.format(j.blocks_found||0));
  set('acc',nf.format(j.shares_accepted||0));set('rej',nf.format(j.shares_rejected||0)+' refusées');
  set('mt',dur(j.mining_time_s));set('tpl',nf.format(j.templates||0));
  var b=j.btc||{};set('px',b.price?(b.prefix||'')+nf.format(b.price):'–');set('bh',b.block_height?nf.format(b.block_height):'–');
  var c=j.clock||{};set('tm',c.synced?c.time:'–');set('dt',c.synced?c.date:'heure pas encore réglée');
  if(j.battery&&j.battery.mv>0){$('batc').hidden=false;var p=Math.max(0,Math.min(100,j.battery.percent|0));set('bat',p+' %');set('batv',(j.battery.mv/1000).toFixed(2)+' V');
    var bb=$('batb');bb.style.width=p+'%';bb.style.background=p>50?'var(--ok)':p>20?'var(--warn)':'var(--bad)'}
  var w=j.wifi||{};set('ssid',w.ssid||'–');set('rssi',w.rssi?w.rssi+' dBm':'–');set('ip',w.ip||'–');
  var q=w.rssi>=-55?4:w.rssi>=-65?3:w.rssi>=-75?2:w.rssi<0?1:0,bars=$('sig').children;for(var i=0;i<4;i++)bars[i].className=i<q?'on':'';
  var po=j.pool||{};set('pool',po.address||'–');
  set('pinfo',(po.backup?'secours · ':'')+'port '+(po.port||'–')+' · diff '+(po.difficulty!=null?diff(po.difficulty):'–'));
  set('ver','NerdMiner '+(j.version||''));set('up',dur(j.uptime_s));set('heap',j.free_heap?Math.round(j.free_heap/1024)+' Ko':'–');
  set('ts',new Date().toLocaleTimeString('fr-FR'));
  live.push(j.hashrate_khs||0);if(live.length>40)live.shift();
}
var hist=[],step=30,live=[];
function draw(){
  var cv=$('cv'),r=cv.getBoundingClientRect(),dp=window.devicePixelRatio||1;
  if(!r.width)return;cv.width=r.width*dp;cv.height=r.height*dp;
  var g=cv.getContext('2d');g.setTransform(dp,0,0,dp,0,0);
  var W=r.width,H=r.height,L=44,B=18,T=8,cs=getComputedStyle(document.documentElement);
  var data=hist.length>1?hist:live,col=cs.getPropertyValue('--acc').trim(),mut=cs.getPropertyValue('--mut').trim(),grd=cs.getPropertyValue('--grid').trim();
  g.clearRect(0,0,W,H);g.font='11px system-ui,sans-serif';g.fillStyle=mut;
  if(data.length<2){g.fillText('Collecte des données…',L,H/2);return}
  var mx=Math.max.apply(null,data),mn=Math.min.apply(null,data);if(mx-mn<mx*0.1){mn=Math.max(0,mx*0.9);}if(mx<=mn)mx=mn+1;
  var pad=(mx-mn)*0.15;mx+=pad;mn=Math.max(0,mn-pad);
  function y(v){return T+(H-T-B)*(1-(v-mn)/(mx-mn))}
  g.strokeStyle=grd;g.lineWidth=1;g.textAlign='right';g.textBaseline='middle';
  for(var k=0;k<=3;k++){var v=mn+(mx-mn)*k/3,yy=y(v);g.beginPath();g.moveTo(L,yy);g.lineTo(W,yy);g.stroke();g.fillText(mx>=1000?(v/1000).toFixed(2)+'M':v.toFixed(0)+'k',L-6,yy)}
  var n=data.length,dx=(W-L)/(n-1);
  g.beginPath();for(var i=0;i<n;i++){var xx=L+i*dx,yv=y(data[i]);i?g.lineTo(xx,yv):g.moveTo(xx,yv)}
  g.strokeStyle=col;g.lineWidth=2;g.lineJoin='round';g.stroke();
  g.lineTo(W,H-B);g.lineTo(L,H-B);g.closePath();var gr=g.createLinearGradient(0,T,0,H-B);gr.addColorStop(0,col+'55');gr.addColorStop(1,col+'00');g.fillStyle=gr;g.fill();
  g.fillStyle=mut;g.textAlign='left';g.textBaseline='alphabetic';
  var span=data===hist?Math.round((n-1)*step/60):Math.round((n-1)*3/60);
  g.fillText(data===hist?'−'+span+' min':'en direct',L,H-4);g.textAlign='right';g.fillText('maintenant',W,H-4);
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
  var st=$('st');st.className='pill bad';set('stt','Hors ligne')}
var pollTimer=0,histTimer=0;
function poll(){
  clearTimeout(pollTimer);
  if(document.hidden)return; // resumes on visibilitychange
  getJSON('/api/status',5000).then(function(j){
    document.body.classList.remove('stale');
    try{update(j);if(hist.length<2)draw()}catch(e){if(window.console)console.error(e)} // a display bug is not "offline"
  },offline).then(function(){pollTimer=setTimeout(poll,3000)})}
function history(){
  clearTimeout(histTimer);
  if(document.hidden)return;
  getJSON('/api/history',5000).then(function(h){step=h.interval_s||30;hist=h.khs||[];draw()},function(){})
  .then(function(){histTimer=setTimeout(history,30000)})}
// Nothing is fetched while the page is in the background (phone locked,
// other tab): no battery use, no load on the stick. Fresh data on return.
document.addEventListener('visibilitychange',function(){if(!document.hidden){poll();history()}});
window.addEventListener('resize',draw);
if(window.matchMedia){var mq=matchMedia('(prefers-color-scheme: light)');if(mq.addEventListener)mq.addEventListener('change',draw);else if(mq.addListener)mq.addListener(draw)}
poll();history();
})();
</script>
</body>
</html>
)rawliteral";

#endif // WEB_MONITOR_PAGE_H
