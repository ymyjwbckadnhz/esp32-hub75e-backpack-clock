#line 1 "X:\\DEV\\MCU\\Luat\\project\\led-backpack-hub75e\\src\\ntp_clock_bmpfont\\web_page.h"
#pragma once
// 网页源码单独放头文件：Arduino 的 .ino 预处理器会破坏 raw string 字面量
const char INDEX_HTML[] PROGMEM = R"HTMLPAGE(<!DOCTYPE html>
<html lang="zh"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>LED 屏配置</title>
<style>
body{font-family:system-ui,Arial,sans-serif;margin:0;padding:16px;background:#f2f4f7;color:#222}
.card{background:#fff;border-radius:12px;padding:16px;margin-bottom:14px;box-shadow:0 2px 8px rgba(0,0,0,.06)}
h1{font-size:18px;margin:0 0 4px}.sub{color:#888;font-size:12px;margin:0 0 12px}
label{display:block;font-size:13px;color:#555;margin:10px 0 4px}
input[type=range]{width:100%}
input[type=time],input[type=text],input[type=number]{width:100%;padding:9px;border:1px solid #ddd;border-radius:8px;font-size:14px;box-sizing:border-box}
.row{display:flex;gap:10px}.row>div{flex:1}
button{width:100%;padding:12px;border:0;border-radius:10px;background:#1976d2;color:#fff;font-size:15px;font-weight:600}
button:active{background:#145ea8}
.mode{display:flex;gap:10px;margin-top:6px}
.mode div{flex:1;text-align:center;padding:12px;border:2px solid #e3e6ea;border-radius:10px;font-size:14px;cursor:pointer}
.mode div.on{border-color:#1976d2;background:#e8f1fb;color:#1976d2;font-weight:600}
.stat{display:grid;grid-template-columns:1fr 1fr;gap:8px;font-size:13px}
.stat b{display:block;color:#888;font-weight:400;font-size:11px}
#toast{position:fixed;left:50%;top:20px;transform:translateX(-50%);background:#333;color:#fff;padding:10px 18px;border-radius:8px;font-size:13px;opacity:0;transition:.3s;pointer-events:none}
#toast.show{opacity:1}
</style></head><body>

<div class="card">
  <h1>LED 屏配置</h1>
  <p class="sub">96×128 HUB75E</p>
  <div class="stat">
    <div><b>当前时间</b><span id="stTime">--</span></div>
    <div><b>屏幕</b><span id="stScreen">--</span></div>
    <div><b>模式</b><span id="stMode">--</span></div>
    <div><b>WiFi</b><span id="stWifi">--</span></div>
  </div>
</div>

<div class="card">
  <label>亮度 <span id="brVal">80</span></label>
  <input type="range" id="bright" min="0" max="255" value="80" oninput="brVal.textContent=this.value">
</div>

<div class="card">
  <label>显示模式</label>
  <div class="mode">
    <div id="m0" onclick="setMode(0)">时钟</div>
    <div id="m1" onclick="setMode(1)">空气质量</div>
    <div id="m2" onclick="setMode(2)">电脑状态</div>
  </div>
</div>

<div class="card">
  <label>开屏时间</label>
  <input type="time" id="onTime" value="07:00">
  <label>关屏时间</label>
  <input type="time" id="offTime" value="23:00">
  <p class="sub" style="margin-top:10px">关屏时间早于开屏时间 = 跨零点</p>
</div>

<div class="card">
  <label>传感器地址</label>
  <input type="text" id="sensor" value="http://192.168.1.27/data">
  <label>刷新间隔（秒）</label>
  <input type="number" id="refresh" min="10" max="3600" value="60">
  <label>电脑状态地址（AIDA64）</label>
  <input type="text" id="pcUrl" value="http://192.168.1.28/sse">
  <label>电脑状态刷新间隔（秒）</label>
  <input type="number" id="pcRefresh" min="1" max="3600" value="5">
</div>

<div class="card">
  <div class="stat">
    <div><b>PM2.5</b><span id="aPm">--</span></div>
    <div><b>CO2</b><span id="aCo2">--</span></div>
    <div><b>温度</b><span id="aTemp">--</span></div>
    <div><b>湿度</b><span id="aHum">--</span></div>
  </div>
</div>

<div class="card">
  <button onclick="save()">保存并应用</button>
</div>

<div id="toast"></div>
<script>
var curMode = 0;

function pad(n){return n<10?'0'+n:''+n}
function toast(t){var e=document.getElementById('toast');e.textContent=t;e.className='show';setTimeout(function(){e.className=''},1600)}
function setMode(m){curMode=m;['m0','m1','m2'].forEach(function(id,i){document.getElementById(id).className=(m==i)?'on':''})}

function load(){
  fetch('/api/config').then(r=>r.json()).then(c=>{
    document.getElementById('bright').value=c.bright;
    document.getElementById('brVal').textContent=c.bright;
    document.getElementById('onTime').value=pad(c.onH)+':'+pad(c.onM);
    document.getElementById('offTime').value=pad(c.offH)+':'+pad(c.offM);
    document.getElementById('sensor').value=c.sensor;
    document.getElementById('refresh').value=c.airRefresh;
    document.getElementById('pcUrl').value=c.pcUrl;
    document.getElementById('pcRefresh').value=c.pcRefresh;
    setMode(c.mode);
  }).catch(()=>toast('读取配置失败'));
}

function save(){
  var on=document.getElementById('onTime').value.split(':');
  var off=document.getElementById('offTime').value.split(':');
  var body={
    bright:+document.getElementById('bright').value,
    onH:+on[0], onM:+on[1],
    offH:+off[0], offM:+off[1],
    mode:curMode,
    airRefresh:+document.getElementById('refresh').value,
    sensor:document.getElementById('sensor').value,
    pcUrl:document.getElementById('pcUrl').value,
    pcRefresh:+document.getElementById('pcRefresh').value
  };
  fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)})
    .then(r=>r.text()).then(()=>toast('已保存'));
}

function poll(){
  fetch('/api/status').then(r=>r.json()).then(s=>{
    document.getElementById('stTime').textContent=s.time||'--';
    document.getElementById('stScreen').textContent=s.screen?'开':'关';
    document.getElementById('stMode').textContent=s.mode==1?'空气质量':'时钟';
    document.getElementById('stWifi').textContent=s.rssi+' dBm';
    document.getElementById('aPm').textContent=s.pm25>=0?s.pm25.toFixed(1):'--';
    document.getElementById('aCo2').textContent=s.co2>=0?s.co2.toFixed(0):'--';
    document.getElementById('aTemp').textContent=s.temp>-100?s.temp.toFixed(1)+'°C':'--';
    document.getElementById('aHum').textContent=s.hum>-100?s.hum.toFixed(0)+'%':'--';
  }).catch(()=>{});
}

load();
setInterval(poll,2000);
poll();
</script></body></html>)HTMLPAGE";
