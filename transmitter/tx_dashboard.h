// Transmitter dashboard (HTML + CSS + JavaScript), stored in flash.
// One raw string literal: no escaping needed, and it is never copied to RAM.
// The text must not contain the closing delimiter )TXHTML".
#pragma once
#include <Arduino.h>

static const char TX_DASHBOARD_HTML[] PROGMEM = R"TXHTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32 Transmitter</title>
<link rel="icon" href="data:,">
<style>
:root{--bg:#f8f9fa;--card:#fff;--line:#e3e6ea;--ink:#16191d;--mute:#5d656f;--ok:#15803d;--warn:#eab308;--bad:#dc2626;--off:#c3c9d0}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--ink);font:15px/1.45 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif}
main{max-width:880px;margin:0 auto;padding:20px 16px 32px}
header{display:flex;flex-wrap:wrap;gap:8px 16px;align-items:center;justify-content:space-between;margin-bottom:16px}
h1{font-size:21px;margin:0}
h2{font-size:14px;font-weight:600;margin:0 0 12px;color:var(--mute)}
.state{display:inline-flex;align-items:center;gap:8px;font-weight:600}
.dot{width:12px;height:12px;border-radius:50%;background:var(--off);flex:none}
.dot.ok{background:var(--ok)}.dot.warn{background:var(--warn)}.dot.bad{background:var(--bad)}
#note{display:none;margin:0 0 16px;padding:10px 14px;border:1px solid #f1c0c0;border-radius:8px;background:#fdf1f1}
#note.show{display:block}
.grid{display:grid;gap:16px;grid-template-columns:repeat(auto-fit,minmax(260px,1fr))}
.card{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:18px}
.wide{grid-column:1/-1}
.big{font-size:52px;line-height:1;font-weight:650;font-variant-numeric:tabular-nums}
.big small{font-size:24px;font-weight:500;color:var(--mute)}
.row{display:flex;flex-wrap:wrap;gap:12px 40px;align-items:flex-end;justify-content:space-between}
.bar{position:relative;height:14px;border-radius:4px;background:#eceff2;margin:18px 0 6px;overflow:hidden}
.bar i{display:block;height:100%;width:0;border-radius:4px;background:var(--off);transition:width .2s linear}
.bar b{position:absolute;top:0;width:2px;height:100%;background:var(--card)}
.scale{position:relative;height:18px;color:var(--mute);font-size:12px}
.scale span{position:absolute;transform:translateX(-50%)}
.scale span:first-child{transform:none}.scale span:last-child{transform:translateX(-100%)}
dl{margin:0;display:grid;grid-template-columns:auto 1fr;gap:8px 16px}
dt{color:var(--mute)}dd{margin:0;text-align:right;font-variant-numeric:tabular-nums;overflow-wrap:anywhere}
.old{opacity:.45}
</style>
</head>
<body>
<main>
<header>
<h1>ESP32 Transmitter</h1>
<span class="state" role="status"><span class="dot" id="dot"></span><span id="state">Loading</span></span>
</header>
<p id="note"></p>
<div class="grid" id="live">
<section class="card wide">
<h2>Potentiometer</h2>
<div class="row">
<div class="big"><span id="pct">--</span><small> %</small></div>
<dl><dt>Raw ADC</dt><dd id="raw">--</dd><dt>Range</dt><dd id="range">--</dd></dl>
</div>
<div class="bar" id="bar" role="progressbar" aria-label="Potentiometer" aria-valuemin="0" aria-valuemax="100"><i id="fill"></i><b style="left:33.5%"></b><b style="left:66.5%"></b></div>
<div class="scale"><span style="left:0">0</span><span style="left:33.5%">33 | 34</span><span style="left:66.5%">66 | 67</span><span style="left:100%">100</span></div>
</section>
<section class="card">
<h2>Switch</h2>
<span class="state"><span class="dot" id="swdot"></span><span id="sw">--</span></span>
</section>
<section class="card">
<h2>Access point</h2>
<dl>
<dt>State</dt><dd id="ap">--</dd>
<dt>SSID</dt><dd id="ssid">--</dd>
<dt>IP address</dt><dd id="ip">--</dd>
<dt>Connected clients</dt><dd id="clients">--</dd>
</dl>
</section>
<section class="card">
<h2>Device</h2>
<dl>
<dt>Device</dt><dd id="device">--</dd>
<dt>Status</dt><dd id="status">--</dd>
<dt>Uptime</dt><dd id="uptime">--</dd>
<dt>HTTP server</dt><dd id="server">--</dd>
<dt>API</dt><dd id="api">--</dd>
<dt>Sequence</dt><dd id="seq">--</dd>
</dl>
</section>
</div>
</main>
<script>
(function(){
"use strict";
var DATA_MS=400,STATUS_MS=2000,TIMEOUT_MS=1500,OFFLINE_AFTER=3;
var fails=0,lastStatus=Date.now()-STATUS_MS+100;
function $(i){return document.getElementById(i);}
function set(i,v){var e=$(i);v=String(v);if(e.textContent!==v)e.textContent=v;}
function dot(i,c){$(i).className="dot "+c;}
function uptime(ms){
  var s=Math.floor(ms/1000),d=Math.floor(s/86400),h=Math.floor(s/3600)%24,m=Math.floor(s/60)%60;
  return (d?d+"d ":"")+h+"h "+m+"m "+(s%60)+"s";
}
function api(path,done,fail){
  var ctl=new AbortController(),t=setTimeout(function(){ctl.abort();},TIMEOUT_MS);
  fetch(path,{cache:"no-store",signal:ctl.signal}).then(function(r){
    if(!r.ok)throw {api:"HTTP "+r.status};
    return r.text();
  }).then(function(x){
    clearTimeout(t);
    var o;
    try{o=JSON.parse(x);}catch(e){throw {api:"invalid JSON"};}
    if(!o||typeof o!=="object")throw {api:"invalid JSON"};
    done(o);
  }).catch(function(e){
    clearTimeout(t);
    fail(e&&e.api?e.api:null);
  });
}
function note(m){$("note").textContent=m||"";$("note").className=m?"show":"";}
function showData(d){
  if(typeof d.pot_percent!=="number"||typeof d.pot_raw!=="number"||typeof d["switch"]!=="number"){failed("missing fields");return;}
  fails=0;note("");$("live").className="grid";
  var p=Math.max(0,Math.min(100,d.pot_percent)),r=p<=33?["Green","ok"]:p<=66?["Yellow","warn"]:["Red","bad"];
  set("pct",p);set("raw",d.pot_raw);set("range",r[0]);
  $("fill").style.width=p+"%";$("fill").style.background="var(--"+r[1]+")";
  $("bar").setAttribute("aria-valuenow",p);
  set("sw",d["switch"]?"Pressed (ON)":"Released (OFF)");dot("swdot",d["switch"]?"ok":"");
  set("clients",d.wifi_clients);set("device",d.device);set("status",d.status);
  set("uptime",uptime(d.uptime_ms));set("seq",d.sequence);set("api","Responding");
  var on=d.status==="ONLINE";
  set("state",on?"Online":d.status);dot("dot",on?"ok":"bad");
  if(!on)note("The transmitter reports a sampling fault. The receiver ignores its data.");
}
function showStatus(s){
  set("ap",s.ap_state);set("ssid",s.ssid);set("ip",s.ip);set("server",s.server_state);
}
function failed(msg){
  fails++;
  if(msg){
    set("api","Error: "+msg);set("state","API error");dot("dot","warn");
    note("The transmitter answered, but the data could not be used ("+msg+").");
    $("live").className="grid old";
  }else if(fails>=OFFLINE_AFTER){
    set("api","No response");set("server","Unknown");set("state","Offline");dot("dot","bad");
    note("No response from the transmitter. Check that this device is still on the esp_tx network. Any values below are the last ones received.");
    $("live").className="grid old";
  }
}
function tick(){
  if(document.hidden){setTimeout(tick,DATA_MS);return;}
  var now=Date.now();
  function next(){setTimeout(tick,fails?DATA_MS*2:DATA_MS);}
  if(now-lastStatus>=STATUS_MS){
    lastStatus=now;
    api("/api/status",function(s){showStatus(s);next();},function(m){failed(m);next();});
  }else{
    api("/api/data",function(d){showData(d);next();},function(m){failed(m);next();});
  }
}
tick();
})();
</script>
</body>
</html>
)TXHTML";

static const size_t TX_DASHBOARD_HTML_LEN = sizeof(TX_DASHBOARD_HTML) - 1;
