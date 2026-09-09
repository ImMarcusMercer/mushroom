#pragma once
#include <Arduino.h>

static const char DASHBOARD_HTML[] PROGMEM = R"FARMHTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>MushroomFarm Monitor</title><style>
:root{font-family:Arial,sans-serif;color:#1d2b23;background:#f3f5f2;color-scheme:light}
body{margin:0}main{max-width:1040px;margin:auto;padding:28px 20px}h1{margin:0 0 6px;font-size:28px}
h2{font-size:20px;margin:0 0 16px}.muted{color:#57645b}.panel{background:white;border:1px solid #d9e1d9;border-radius:8px;padding:20px;margin-top:20px}
.bar{display:flex;gap:14px;align-items:center;flex-wrap:wrap}.bar span{padding:7px 10px;background:#edf2ec;border-radius:4px}
table{width:100%;border-collapse:collapse;text-align:left}th,td{padding:12px 8px;border-bottom:1px solid #e2e7e2}th{font-size:13px;color:#526054}
.table-wrap{overflow:auto}button{background:#245c3b;color:white;border:1px solid #245c3b;border-radius:5px;padding:9px 14px;cursor:pointer;font:inherit}
button.secondary{background:white;color:#245c3b}button:disabled{opacity:.45;cursor:wait}button:focus-visible{outline:3px solid #c8a34b;outline-offset:2px}
.relays{display:grid;grid-template-columns:repeat(auto-fit,minmax(260px,1fr));gap:12px}.relay{border:1px solid #d9e1d9;padding:16px;border-radius:6px}
.relay h3{font-size:16px;margin:0 0 8px}.relay p{margin:0 0 12px}.actions{display:flex;gap:7px;flex-wrap:wrap}.error{color:#aa2e21}#notice{min-height:22px;margin:14px 0 0}
.spectra{display:grid;grid-template-columns:repeat(auto-fit,minmax(140px,1fr));gap:12px}.spectra div{padding:12px;background:#f3f5f2;border-radius:5px}.spectra p{margin-bottom:0}
@media(max-width:540px){main{padding:20px 12px}.panel{padding:15px}th,td{padding:10px 6px;font-size:13px}}
</style></head><body><main>
<h1>MushroomFarm Monitor</h1><p class="muted">Farm conditions and equipment control</p>
<div class="bar"><span id="connection">Connecting...</span><span id="clock">Clock: --</span><span id="sd">SD: --</span><span id="hardware">Relays: --</span></div>
<p id="notice" role="status" aria-live="polite"></p>
<section class="panel"><h2>Sensor readings</h2><div class="table-wrap"><table><thead><tr><th>Sensor</th><th>Temperature</th><th>Humidity</th><th>Status</th><th>Last update</th></tr></thead><tbody id="sensors"></tbody></table></div></section>
<section class="panel"><h2>Equipment</h2><p class="muted">ON and OFF select manual control. AUTO restores the configured rules. A sprinkler run stops after 30 seconds.</p><div id="relays" class="relays"></div><p><button id="reset" class="secondary">Return all to automatic</button></p></section>
<section class="panel"><h2>Spectrometer readings</h2><p id="spectro-note" class="muted"></p><div id="spectra" class="spectra"></div></section>
<section class="panel"><h2>Clock and logging</h2><p id="history"></p><p id="log-error" class="error"></p><p id="last-log" class="muted"></p><div class="actions"><button id="sync">Sync Manila time</button><button id="log" class="secondary">Save pending readings</button></div><p class="muted">Automatic CSV export runs hourly. Readings waiting in RAM are lost if the main controller loses power.</p></section>
</main><script>
const $=id=>document.getElementById(id);let current=null,busy=false,synced=false;
function message(text,error=false){$('notice').textContent=text;$('notice').className=error?'error':'';}
async function call(path,body){const response=await fetch(path,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body||{}),signal:AbortSignal.timeout(8000)});const text=await response.text();if(!response.ok)throw new Error(text);return text;}
async function action(path,body){if(busy)return;busy=true;disableControls();try{message(await call(path,body));}catch(e){message(e.message,true);}finally{busy=false;await refresh();}}
function disableControls(){document.querySelectorAll('button').forEach(b=>b.disabled=busy||!current);if(current){document.querySelectorAll('[data-relay],#reset').forEach(b=>b.disabled=busy||!current.relays_available);$('log').disabled=busy||current.logging;}}
function cell(row,text){const c=document.createElement('td');c.textContent=text;row.appendChild(c);}
function render(s){current=s;$('connection').textContent='Connected · v'+s.version;$('clock').textContent=s.clock_valid?s.time:'Clock needs sync';$('sd').textContent='SD: '+(s.sd_available?'ready':'unavailable');$('hardware').textContent='Relays: '+(s.relays_available?'ready':'unavailable');
$('sensors').replaceChildren();s.sensors.forEach(x=>{const r=document.createElement('tr');cell(r,x.id);cell(r,x.fresh?x.temp.toFixed(1)+' °C':'—');cell(r,x.fresh?x.hum.toFixed(1)+'%':'—');cell(r,x.fresh?'Online':x.seen?'Stale':'Waiting');cell(r,x.seen?Math.floor(x.age_ms/1000)+'s ago':'—');$('sensors').appendChild(r);});
$('relays').replaceChildren();s.relays.forEach(x=>{const box=document.createElement('div');box.className='relay';const h=document.createElement('h3');h.textContent=x.name;const p=document.createElement('p');p.textContent=(s.relays_available?(x.on?'ON':'OFF'):'Unknown')+' · '+(x.mode==='auto'?'Automatic':'Manual');const a=document.createElement('div');a.className='actions';['on','off','auto'].forEach(mode=>{const b=document.createElement('button');b.type='button';b.dataset.relay=x.id;b.textContent=mode.toUpperCase();b.className=mode===x.mode?'':'secondary';b.onclick=()=>action('/relay-control',{relay:x.id,action:mode});a.appendChild(b);});box.append(h,p,a);$('relays').appendChild(box);});
$('spectro-note').textContent=s.lights_on?'Collection enabled while the light output is ON.':'Collection disabled because the light output is OFF.';$('spectra').replaceChildren();s.treatments.forEach(x=>{const box=document.createElement('div');const h=document.createElement('strong');h.textContent='Treatment '+x.treatment;const p=document.createElement('p');p.textContent=x.values.length?x.values.join(' · '):'No current data';box.append(h,p);$('spectra').appendChild(box);});
$('history').textContent=s.history_count+' / '+s.history_capacity+' pending records · '+s.frozen_count+' in pending export · '+s.dropped_history+' records omitted because the buffer was full';$('log-error').textContent=s.log_error;$('last-log').textContent=s.logging?'Writing and verifying CSV files...':s.last_log?'Last completed export: '+s.last_log:'No completed export this boot.';disableControls();}
async function refresh(){try{const r=await fetch('/status',{cache:'no-store',signal:AbortSignal.timeout(7000)});if(!r.ok)throw new Error('Status request failed ('+r.status+')');const s=await r.json();render(s);if(s.rtc_available&&!s.clock_valid&&!synced){synced=true;await syncTime();}}catch(e){current=null;$('connection').textContent='Connection lost';disableControls();message(e.message,true);}}
function syncTime(){const t=new Date(Date.now()+8*3600000);return action('/sync-time',{year:t.getUTCFullYear(),month:t.getUTCMonth()+1,day:t.getUTCDate(),hour:t.getUTCHours(),minute:t.getUTCMinutes(),second:t.getUTCSeconds()});}
$('sync').onclick=syncTime;$('log').onclick=()=>action('/log-now');$('reset').onclick=()=>action('/reset-overrides');
async function poll(){await refresh();setTimeout(poll,2000);}poll();
</script></body></html>)FARMHTML";
