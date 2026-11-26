#pragma once
// ============================================================
//  web_page.h —— 坏点检测控制页
//  单独放头文件：Arduino 的 .ino 预处理器会破坏 raw string 字面量
// ============================================================

const char INDEX_HTML[] PROGMEM = R"HTMLPAGE(<!DOCTYPE html>
<html lang="zh"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>坏点检测</title>
<style>
:root{--bg:#0f1216;--card:#171c23;--line:#242c36;--fg:#e8edf3;--dim:#8b97a6;--acc:#2f81f7}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);font-family:system-ui,-apple-system,"Segoe UI",Arial,sans-serif;font-size:14px}
.wrap{max-width:720px;margin:0 auto;padding:14px}
h1{font-size:16px;margin:0 0 10px;font-weight:600}
h1 small{color:var(--dim);font-weight:400;font-size:12px;margin-left:6px}
.card{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:12px;margin-bottom:12px}
.lab{color:var(--dim);font-size:12px;margin:0 0 8px}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(64px,1fr));gap:8px}
.btn{border:1px solid var(--line);background:#1e252e;color:var(--fg);border-radius:9px;padding:10px 6px;font-size:13px;cursor:pointer;text-align:center;user-select:none;transition:.15s}
.btn:hover{border-color:#3a4653}
.btn.on{background:var(--acc);border-color:var(--acc);color:#fff;font-weight:600}
.sw{height:34px;border-radius:8px;border:1px solid var(--line);cursor:pointer;position:relative}
.sw.on{outline:2px solid var(--acc);outline-offset:1px}
.row{display:flex;gap:10px;align-items:center;margin-top:10px;flex-wrap:wrap}
.row>div{flex:1;min-width:90px}
input[type=number]{width:100%;background:#0e1216;border:1px solid var(--line);color:var(--fg);border-radius:8px;padding:8px;font-size:14px}
.big{display:flex;gap:10px;margin-top:10px}
.big button{flex:1;padding:14px;font-size:16px;border-radius:10px;border:1px solid var(--line);background:#1e252e;color:var(--fg);cursor:pointer}
.big button:active{background:#2a3542}
.big button.play{background:#1f6f3f;border-color:#1f6f3f;color:#fff}
.big button.play.on{background:#8a2f2f;border-color:#8a2f2f}
.stat{display:grid;grid-template-columns:repeat(3,1fr);gap:8px;text-align:center}
.stat b{display:block;color:var(--dim);font-weight:400;font-size:11px;margin-bottom:3px}
.stat span{font-size:17px;font-variant-numeric:tabular-nums}
.bar{height:6px;background:#0e1216;border-radius:3px;overflow:hidden;margin-top:10px}
.bar>i{display:block;height:100%;background:var(--acc);width:0;transition:width .15s}
</style></head><body>
<div class="wrap">

<h1>坏点检测 <small>96 × 128</small></h1>

<div class="card">
  <p class="lab">亮度 <span id="brVal">25</span> / 255 <span style="color:#8b97a6">（全白时调低，避免供电不足重启）</span></p>
  <input type="range" id="bright" min="1" max="255" value="25" style="width:100%">
</div>

<div class="card">
  <p class="lab">整屏纯色</p>
  <div class="grid" id="colors"></div>
</div>

<div class="card">
  <p class="lab">定位扫描</p>
  <div class="grid" id="modes">
    <div class="btn" data-mode="1">逐行</div>
    <div class="btn" data-mode="2">逐列</div>
    <div class="btn" data-mode="3">逐点</div>
  </div>
  <div class="row">
    <div><p class="lab" style="margin-bottom:4px">起始</p><input type="number" id="from" value="0"></div>
    <div><p class="lab" style="margin-bottom:4px">结束</p><input type="number" id="to" value="0"></div>
    <div><p class="lab" style="margin-bottom:4px">步长</p><input type="number" id="step" value="1" min="1"></div>
  </div>
  <div class="row">
    <div style="flex:1">
      <label style="display:flex;gap:8px;align-items:center;cursor:pointer">
        <input type="checkbox" id="invert"> 反色（白底黑点）
      </label>
    </div>
    <div style="flex:1">
      <label style="display:flex;gap:8px;align-items:center;cursor:pointer">
        <input type="checkbox" id="cross"> 逐点显示十字辅助
      </label>
    </div>
  </div>
</div>

<div class="card">
  <div class="stat">
    <div><b>当前序号</b><span id="sIdx">0</span></div>
    <div><b>坐标 X,Y</b><span id="sPos">-</span></div>
    <div><b>进度</b><span id="sPct">0%</span></div>
  </div>
  <div class="bar"><i id="bar"></i></div>

  <div class="big">
    <button onclick="step(-1)">◀ 上一个</button>
    <button class="play" id="playBtn" onclick="togglePlay()">▶ 自动</button>
    <button onclick="step(1)">下一个 ▶</button>
  </div>

  <div class="row">
    <div><p class="lab" style="margin-bottom:4px">自动延时 (ms)</p><input type="number" id="delay" value="800" min="50" step="50"></div>
    <div><p class="lab" style="margin-bottom:4px">跳到序号</p>
      <div style="display:flex;gap:6px">
        <input type="number" id="goto" value="0">
        <button class="btn" style="padding:8px 14px" onclick="doGoto()">跳</button>
      </div>
    </div>
  </div>
  <p class="lab" style="margin:10px 0 0">键盘：← → 切换，空格 播放/暂停</p>
</div>

</div>
<script>
var S={}, COLORS=[
 ["红","#ff0000"],["绿","#00ff00"],["蓝","#0000ff"],["白","#ffffff"],
 ["黑","#000000"],["青","#00ffff"],["品红","#ff00ff"],["黄","#ffff00"],
 ["灰25","#404040"],["灰50","#808080"],["灰75","#c0c0c0"]
];

function api(p,d){
  return fetch(p,{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify(d||{})}).then(r=>r.json()).then(apply);
}
function post(p,d){return fetch(p,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(d||{})}).then(r=>r.json()).then(apply)}

function buildColors(){
  var box=document.getElementById('colors');
  COLORS.forEach(function(c,i){
    var d=document.createElement('div');
    d.className='sw'; d.style.background=c[1]; d.title=c[0]; d.dataset.i=i;
    d.onclick=function(){post('/api/set',{mode:0,color:i})};
    box.appendChild(d);
  });
}

function apply(s){
  if(!s) return;
  S=s;
  // 颜色高亮
  document.querySelectorAll('#colors .sw').forEach(function(e){
    e.classList.toggle('on', s.mode===0 && +e.dataset.i===s.color);
  });
  document.querySelectorAll('#modes .btn').forEach(function(e){
    e.classList.toggle('on', +e.dataset.mode===s.mode);
  });
  document.getElementById('from').value=s.from;
  document.getElementById('to').value=s.to;
  document.getElementById('step').value=s.step;
  document.getElementById('delay').value=s.delay;
  document.getElementById('invert').checked=s.invert;
  document.getElementById('cross').checked=s.crosshair;
  document.getElementById('bright').value=s.bright;
  document.getElementById('brVal').textContent=s.bright;
  document.getElementById('playBtn').classList.toggle('on', s.running);
  document.getElementById('playBtn').textContent = s.running ? '⏸ 暂停' : '▶ 自动';

  document.getElementById('sIdx').textContent=s.idx;
  document.getElementById('sPos').textContent = (s.px>=0? s.px+','+s.py : '-');
  var total=Math.max(1,s.to-s.from+1);
  var cur=Math.max(0,Math.min(total,s.idx-s.from+1));
  var pct=Math.round(cur*100/total);
  document.getElementById('sPct').textContent=pct+'%';
  document.getElementById('bar').style.width=pct+'%';
}

function step(d){post('/api/step',{delta:d})}
function doGoto(){var v=+document.getElementById('goto').value;post('/api/goto',{idx:v})}
function togglePlay(){post('/api/set',{running:!S.running})}

document.getElementById('modes').onclick=function(e){
  var t=e.target.closest('.btn'); if(!t) return;
  post('/api/set',{mode:+t.dataset.mode});
};
['from','to','step','delay'].forEach(function(id){
  document.getElementById(id).onchange=function(){
    var o={}; o[id==='delay'?'delay':id]=+this.value; post('/api/set',o);
  };
});
var brEl=document.getElementById('bright');
brEl.oninput=function(){document.getElementById('brVal').textContent=this.value};
brEl.onchange=function(){post('/api/set',{bright:+this.value})};
document.getElementById('invert').onchange=function(){post('/api/set',{invert:this.checked})};
document.getElementById('cross').onchange=function(){post('/api/set',{crosshair:this.checked})};

document.addEventListener('keydown',function(e){
  if(e.key==='ArrowLeft'){step(-1);e.preventDefault()}
  else if(e.key==='ArrowRight'){step(1);e.preventDefault()}
  else if(e.code==='Space'){togglePlay();e.preventDefault()}
});

function poll(){fetch('/api/state').then(r=>r.json()).then(apply).catch(function(){})}

buildColors();
poll();
setInterval(poll,800);
</script></body></html>)HTMLPAGE";
