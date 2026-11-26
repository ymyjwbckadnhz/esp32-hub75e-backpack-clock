#pragma once
// ============================================================
//  web_page.h —— 坏点检测控制页（含画布框选）
//  单独放头文件：Arduino 的 .ino 预处理器会破坏 raw string 字面量
// ============================================================

const char INDEX_HTML[] PROGMEM = R"HTMLPAGE(<!DOCTYPE html>
<html lang="zh"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no">
<title>坏点检测</title>
<style>
:root{--bg:#0f1216;--card:#171c23;--line:#242c36;--fg:#e8edf3;--dim:#8b97a6;--acc:#2f81f7}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);font-family:system-ui,-apple-system,"Segoe UI",Arial,sans-serif;font-size:14px}
.wrap{max-width:760px;margin:0 auto;padding:14px}
h1{font-size:16px;margin:0 0 10px;font-weight:600}
h1 small{color:var(--dim);font-weight:400;font-size:12px;margin-left:6px}
.card{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:12px;margin-bottom:12px}
.lab{color:var(--dim);font-size:12px;margin:0 0 8px}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(64px,1fr));gap:8px}
.btn{border:1px solid var(--line);background:#1e252e;color:var(--fg);border-radius:9px;padding:10px 6px;font-size:13px;cursor:pointer;text-align:center;user-select:none;transition:.15s}
.btn:hover{border-color:#3a4653}
.btn.on{background:var(--acc);border-color:var(--acc);color:#fff;font-weight:600}
.sw{height:34px;border-radius:8px;border:1px solid var(--line);cursor:pointer}
.sw.on{outline:2px solid var(--acc);outline-offset:1px}
.row{display:flex;gap:10px;align-items:center;margin-top:10px;flex-wrap:wrap}
.row>div{flex:1;min-width:78px}
input[type=number]{width:100%;background:#0e1216;border:1px solid var(--line);color:var(--fg);border-radius:8px;padding:8px;font-size:14px}
.big{display:flex;gap:10px;margin-top:10px}
.big button{flex:1;padding:14px;font-size:15px;border-radius:10px;border:1px solid var(--line);background:#1e252e;color:var(--fg);cursor:pointer}
.big button:active{background:#2a3542}
.big button.play{background:#1f6f3f;border-color:#1f6f3f;color:#fff}
.big button.play.on{background:#8a2f2f;border-color:#8a2f2f}
.stat{display:grid;grid-template-columns:repeat(4,1fr);gap:8px;text-align:center}
.stat b{display:block;color:var(--dim);font-weight:400;font-size:11px;margin-bottom:3px}
.stat span{font-size:16px;font-variant-numeric:tabular-nums}
.bar{height:6px;background:#0e1216;border-radius:3px;overflow:hidden;margin-top:10px}
.bar>i{display:block;height:100%;background:var(--acc);width:0;transition:width .15s}
#cv{width:100%;height:auto;display:block;border-radius:8px;background:#000;touch-action:none;cursor:crosshair}
.hint{color:var(--dim);font-size:12px;margin:8px 0 0}
</style></head><body>
<div class="wrap">

<h1>坏点检测 <small>96 × 128</small></h1>

<div class="card">
  <p class="lab">框选区域 <span style="color:#8b97a6">— 在图上拖动选择，之后所有扫描都只在这个范围内</span></p>
  <canvas id="cv"></canvas>
  <div class="row">
    <div><p class="lab" style="margin-bottom:4px">X0</p><input type="number" id="rx0" min="0" max="127" value="0"></div>
    <div><p class="lab" style="margin-bottom:4px">Y0</p><input type="number" id="ry0" min="0" max="95" value="0"></div>
    <div><p class="lab" style="margin-bottom:4px">X1</p><input type="number" id="rx1" min="0" max="127" value="127"></div>
    <div><p class="lab" style="margin-bottom:4px">Y1</p><input type="number" id="ry1" min="0" max="95" value="95"></div>
  </div>
  <div class="row">
    <div style="flex:1"><button class="btn" style="width:100%" onclick="setRect(0,0,127,95)">全屏</button></div>
    <div style="flex:1"><button class="btn" style="width:100%" onclick="setRect(0,0,63,47)">左上 ¼</button></div>
    <div style="flex:1"><button class="btn" style="width:100%" onclick="setRect(64,48,127,95)">右下 ¼</button></div>
    <div style="flex:1"><button class="btn" style="width:100%" onclick="setRect(32,24,95,71)">居中 64×48</button></div>
  </div>
</div>

<div class="card">
  <p class="lab">显示模式</p>
  <div class="grid" id="modes">
    <div class="btn" data-mode="1">逐行</div>
    <div class="btn" data-mode="2">逐列</div>
    <div class="btn" data-mode="3">逐点</div>
    <div class="btn" data-mode="4">区块点亮</div>
  </div>
</div>

<div class="card">
  <p class="lab">整屏纯色</p>
  <div class="grid" id="colors"></div>
</div>

<div class="card">
  <div class="stat">
    <div><b>序号</b><span id="sIdx">0</span></div>
    <div><b>坐标</b><span id="sPos">-</span></div>
    <div><b>进度</b><span id="sPct">0%</span></div>
    <div><b>总数</b><span id="sTot">0</span></div>
  </div>
  <div class="bar"><i id="bar"></i></div>

  <div class="big">
    <button onclick="step(-1)">◀ 上一个</button>
    <button class="play" id="playBtn" onclick="togglePlay()">▶ 自动</button>
    <button onclick="step(1)">下一个 ▶</button>
  </div>

  <div class="row">
    <div><p class="lab" style="margin-bottom:4px">区间起</p><input type="number" id="from" value="0"></div>
    <div><p class="lab" style="margin-bottom:4px">区间止</p><input type="number" id="to" value="0"></div>
    <div><p class="lab" style="margin-bottom:4px">步长</p><input type="number" id="stepv" value="1" min="1"></div>
    <div><p class="lab" style="margin-bottom:4px">延时 ms</p><input type="number" id="delay" value="800" min="50" step="50"></div>
  </div>

  <div class="row">
    <div><p class="lab" style="margin-bottom:4px">跳到序号</p>
      <div style="display:flex;gap:6px">
        <input type="number" id="goto" value="0">
        <button class="btn" style="padding:8px 14px" onclick="doGoto()">跳</button>
      </div>
    </div>
    <div><p class="lab" style="margin-bottom:4px">亮度 <span id="brVal">25</span></p>
      <input type="range" id="bright" min="1" max="255" value="25" style="width:100%">
    </div>
  </div>

  <div class="row">
    <label style="display:flex;gap:8px;align-items:center;cursor:pointer">
      <input type="checkbox" id="invert"> 反色（白底黑点，查常亮）
    </label>
    <label style="display:flex;gap:8px;align-items:center;cursor:pointer">
      <input type="checkbox" id="cross"> 逐点显示十字
    </label>
  </div>
  <p class="hint">键盘：← → 切换，空格 播放/暂停</p>
</div>

</div>
<script>
var S={}, DRAG=null;
var SW=128, SH=96, SC=4;                 // 逻辑尺寸 × 放大倍数
var COLORS=[
 ["红","#ff0000"],["绿","#00ff00"],["蓝","#0000ff"],["白","#ffffff"],
 ["黑","#000000"],["青","#00ffff"],["品红","#ff00ff"],["黄","#ffff00"],
 ["灰25","#404040"],["灰50","#808080"],["灰75","#c0c0c0"]
];

var cv=document.getElementById('cv'), ctx=cv.getContext('2d');
cv.width=SW*SC; cv.height=SH*SC;
ctx.imageSmoothingEnabled=false;

function post(p,d){
  return fetch(p,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(d||{})})
    .then(r=>r.json()).then(apply);
}
function step(d){post('/api/step',{delta:d})}
function doGoto(){post('/api/goto',{idx:+document.getElementById('goto').value})}
function togglePlay(){post('/api/set',{running:!S.running})}
function setRect(x0,y0,x1,y1){post('/api/set',{rx0:x0,ry0:y0,rx1:x1,ry1:y1})}

function buildColors(){
  var box=document.getElementById('colors');
  COLORS.forEach(function(c,i){
    var d=document.createElement('div');
    d.className='sw'; d.style.background=c[1]; d.title=c[0]; d.dataset.i=i;
    d.onclick=function(){post('/api/set',{mode:0,color:i})};
    box.appendChild(d);
  });
}

// ---------- 画布 ----------
function draw(){
  ctx.fillStyle='#000'; ctx.fillRect(0,0,cv.width,cv.height);

  var r=S.rx0!==undefined?S:null;
  if(!r) return;

  var x0=S.rx0, y0=S.ry0, x1=S.rx1, y1=S.ry1;

  // 框选区域底色
  ctx.fillStyle='#1b2027';
  ctx.fillRect(x0*SC,y0*SC,(x1-x0+1)*SC,(y1-y0+1)*SC);

  // 当前元素
  ctx.fillStyle='#ffffff';
  if(S.mode===0){
    var c=COLORS[S.color]||COLORS[0];
    ctx.fillStyle=c[1]; ctx.fillRect(0,0,cv.width,cv.height);
  } else if(S.mode===4){
    ctx.fillRect(x0*SC,y0*SC,(x1-x0+1)*SC,(y1-y0+1)*SC);
  } else if(S.mode===1){
    ctx.fillRect(x0*SC,(y0+S.idx)*SC,(x1-x0+1)*SC,SC);
  } else if(S.mode===2){
    ctx.fillRect((x0+S.idx)*SC,y0*SC,SC,(y1-y0+1)*SC);
  } else if(S.mode===3 && S.px>=0){
    if(S.crosshair){
      ctx.fillStyle='rgba(60,120,220,.55)';
      ctx.fillRect(x0*SC,S.py*SC,(x1-x0+1)*SC,SC);
      ctx.fillRect(S.px*SC,y0*SC,SC,(y1-y0+1)*SC);
      ctx.fillStyle='#fff';
    }
    ctx.fillRect(S.px*SC,S.py*SC,SC,SC);
  }

  // 框选描边
  ctx.strokeStyle='#2f81f7'; ctx.lineWidth=1.5;
  ctx.strokeRect(x0*SC+0.75,y0*SC+0.75,(x1-x0+1)*SC-1.5,(y1-y0+1)*SC-1.5);

  // 拖动预览
  if(DRAG){
    ctx.strokeStyle='#ffd166'; ctx.setLineDash([4,3]);
    ctx.strokeRect(DRAG.x0*SC+0.75,DRAG.y0*SC+0.75,(DRAG.x1-DRAG.x0+1)*SC-1.5,(DRAG.y1-DRAG.y0+1)*SC-1.5);
    ctx.setLineDash([]);
  }
}

function cell(ev){
  var b=cv.getBoundingClientRect();
  var x=Math.floor((ev.clientX-b.left)/b.width*SW);
  var y=Math.floor((ev.clientY-b.top)/b.height*SH);
  return [Math.max(0,Math.min(SW-1,x)), Math.max(0,Math.min(SH-1,y))];
}

cv.addEventListener('pointerdown',function(ev){
  ev.preventDefault(); cv.setPointerCapture(ev.pointerId);
  var c=cell(ev); DRAG={ax:c[0], ay:c[1], x0:c[0], y0:c[1], x1:c[0], y1:c[1]};
  draw();
});
cv.addEventListener('pointermove',function(ev){
  if(!DRAG) return;
  var c=cell(ev);
  DRAG.x0=Math.min(DRAG.ax,c[0]); DRAG.x1=Math.max(DRAG.ax,c[0]);
  DRAG.y0=Math.min(DRAG.ay,c[1]); DRAG.y1=Math.max(DRAG.ay,c[1]);
  draw();
});
cv.addEventListener('pointerup',function(ev){
  if(!DRAG) return;
  var d=DRAG; DRAG=null;
  setRect(d.x0,d.y0,d.x1,d.y1);
});
cv.addEventListener('pointercancel',function(){DRAG=null;draw()});

// ---------- 状态同步 ----------
function apply(s){
  if(!s) return;
  S=s;
  draw();

  document.querySelectorAll('#colors .sw').forEach(function(e){
    e.classList.toggle('on', s.mode===0 && +e.dataset.i===s.color);
  });
  document.querySelectorAll('#modes .btn').forEach(function(e){
    e.classList.toggle('on', +e.dataset.mode===s.mode);
  });

  if(!DRAG){
    document.getElementById('rx0').value=s.rx0;
    document.getElementById('ry0').value=s.ry0;
    document.getElementById('rx1').value=s.rx1;
    document.getElementById('ry1').value=s.ry1;
  }
  document.getElementById('from').value=s.from;
  document.getElementById('to').value=s.to;
  document.getElementById('stepv').value=s.step;
  document.getElementById('delay').value=s.delay;
  document.getElementById('bright').value=s.bright;
  document.getElementById('brVal').textContent=s.bright;
  document.getElementById('invert').checked=s.invert;
  document.getElementById('cross').checked=s.crosshair;
  var pb=document.getElementById('playBtn');
  pb.classList.toggle('on', s.running);
  pb.textContent = s.running ? '⏸ 暂停' : '▶ 自动';

  document.getElementById('sIdx').textContent=s.idx;
  document.getElementById('sPos').textContent = (s.px>=0? s.px+','+s.py : '-');
  document.getElementById('sTot').textContent=s.count;
  var total=Math.max(1,s.to-s.from+1);
  var cur=Math.max(0,Math.min(total,s.idx-s.from+1));
  var pct=Math.round(cur*100/total);
  document.getElementById('sPct').textContent=pct+'%';
  document.getElementById('bar').style.width=pct+'%';
}

document.getElementById('modes').onclick=function(e){
  var t=e.target.closest('.btn'); if(!t) return;
  post('/api/set',{mode:+t.dataset.mode});
};

['rx0','ry0','rx1','ry1'].forEach(function(id){
  document.getElementById(id).onchange=function(){
    var o={};
    o.rx0=+document.getElementById('rx0').value;
    o.ry0=+document.getElementById('ry0').value;
    o.rx1=+document.getElementById('rx1').value;
    o.ry1=+document.getElementById('ry1').value;
    post('/api/set',o);
  };
});
['from','to','stepv','delay'].forEach(function(id){
  document.getElementById(id).onchange=function(){
    var map={from:'from',to:'to',stepv:'step',delay:'delay'};
    var o={}; o[map[id]]=+this.value; post('/api/set',o);
  };
});
var brEl=document.getElementById('bright');
brEl.oninput=function(){document.getElementById('brVal').textContent=this.value};
brEl.onchange=function(){post('/api/set',{bright:+this.value})};
document.getElementById('invert').onchange=function(){post('/api/set',{invert:this.checked})};
document.getElementById('cross').onchange=function(){post('/api/set',{crosshair:this.checked})};

document.addEventListener('keydown',function(e){
  if(e.target.tagName==='INPUT') return;
  if(e.key==='ArrowLeft'){step(-1);e.preventDefault()}
  else if(e.key==='ArrowRight'){step(1);e.preventDefault()}
  else if(e.code==='Space'){togglePlay();e.preventDefault()}
});

function poll(){fetch('/api/state').then(r=>r.json()).then(apply).catch(function(){})}
buildColors();
poll();
setInterval(function(){ if(!DRAG) poll(); }, 700);
</script></body></html>)HTMLPAGE";
