#pragma once

static const char index_html[] = R"rawhtml(<!DOCTYPE html>
<html lang="ko">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1.0">
<title>FlowMeter CAM</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
:root{
  --bg:#f5f6f8;
  --white:#ffffff;
  --border:#e4e6eb;
  --text:#1a1d23;
  --text-sub:#6b7280;
  --text-dim:#9ca3af;
  --accent:#2563eb;
  --accent-light:#eff6ff;
  --green:#16a34a;
  --green-light:#f0fdf4;
  --orange:#d97706;
  --orange-light:#fffbeb;
  --red:#dc2626;
  --red-light:#fef2f2;
  --shadow:0 1px 3px rgba(0,0,0,.06),0 1px 2px rgba(0,0,0,.04);
  --shadow-md:0 4px 12px rgba(0,0,0,.08),0 2px 4px rgba(0,0,0,.04);
}
body{
  background:var(--bg);
  color:var(--text);
  font-family:'Malgun Gothic','Segoe UI',system-ui,sans-serif;
  font-size:13px;
  min-height:100vh;
}
/* ── Header ── */
.header{
  background:var(--white);
  border-bottom:1px solid var(--border);
  padding:0 24px;
  height:48px;
  display:flex;
  align-items:center;
  gap:10px;
  position:sticky;
  top:0;
  z-index:100;
}
.header-title{
  font-size:.95rem;
  font-weight:700;
  color:var(--text);
  letter-spacing:-.01em;
}
.header-dot{
  width:7px;height:7px;
  border-radius:50%;
  background:var(--green);
  flex-shrink:0;
}
.header-right{
  margin-left:auto;
  display:flex;align-items:center;gap:16px;
  font-size:.75rem;
  color:var(--text-sub);
}
.live-chip{
  display:flex;align-items:center;gap:5px;
  background:var(--red-light);
  border:1px solid rgba(220,38,38,.15);
  border-radius:20px;
  padding:3px 10px;
  font-size:.7rem;
  font-weight:700;
  color:var(--red);
  letter-spacing:.5px;
}
.live-chip-dot{
  width:6px;height:6px;
  border-radius:50%;
  background:var(--red);
  animation:blink .9s infinite;
}
@keyframes blink{0%,100%{opacity:1}50%{opacity:.25}}
@keyframes spin{to{transform:rotate(360deg)}}
/* ── Layout ── */
.page{
  max-width:1280px;
  margin:0 auto;
  padding:20px 20px;
  display:flex;
  flex-direction:column;
  gap:16px;
}
/* ── KPI Row ── */
.kpi-row{
  display:grid;
  grid-template-columns:repeat(4,1fr);
  gap:12px;
}
@media(max-width:900px){.kpi-row{grid-template-columns:repeat(2,1fr)}}
@media(max-width:500px){.kpi-row{grid-template-columns:1fr}}
.kpi-card{
  background:var(--white);
  border:1px solid var(--border);
  border-radius:10px;
  padding:14px 16px;
  box-shadow:var(--shadow);
}
.kpi-label{
  font-size:.7rem;
  color:var(--text-sub);
  margin-bottom:4px;
}
.kpi-value{
  font-size:1.55rem;
  font-weight:700;
  line-height:1.1;
  color:var(--text);
  display:flex;align-items:baseline;gap:3px;
}
.kpi-unit{font-size:.75rem;font-weight:400;color:var(--text-sub)}
.kpi-status{
  display:flex;align-items:center;gap:5px;
  margin-top:6px;
  font-size:.7rem;
}
.status-dot{
  width:6px;height:6px;
  border-radius:50%;
  flex-shrink:0;
}
.dot-green{background:var(--green)}
.dot-orange{background:var(--orange)}
.dot-red{background:var(--red)}
.dot-blue{background:var(--accent)}
.text-green{color:var(--green)}
.text-orange{color:var(--orange)}
.text-red{color:var(--red)}
.text-blue{color:var(--accent)}
/* ── Main Content ── */
.content-row{
  display:grid;
  grid-template-columns:1fr 280px;
  gap:16px;
  align-items:start;
}
@media(max-width:900px){.content-row{grid-template-columns:1fr}}
/* ── Camera Card ── */
.cam-card{
  background:var(--white);
  border:1px solid var(--border);
  border-radius:10px;
  box-shadow:var(--shadow);
  overflow:hidden;
}
.card-header{
  padding:12px 16px;
  border-bottom:1px solid var(--border);
  display:flex;
  align-items:center;
  gap:8px;
}
.card-title{
  font-size:.78rem;
  font-weight:700;
  color:var(--text);
}
.card-title-bar{
  width:3px;height:14px;
  background:var(--accent);
  border-radius:2px;
  flex-shrink:0;
}
.card-header-right{
  margin-left:auto;
  display:flex;align-items:center;gap:8px;
}
.cam-wrap{
  position:relative;
  background:#111;
  min-height:200px;
  display:flex;
  align-items:center;
  justify-content:center;
}
#cam{
  width:100%;
  display:block;
  transition:opacity .12s;
}
#cam.updating{opacity:.8}
.cam-hud{
  position:absolute;
  inset:0;
  pointer-events:none;
}
.cam-hud-tl{
  position:absolute;
  top:10px;left:12px;
  display:flex;align-items:center;gap:6px;
  font-size:.7rem;
  font-weight:700;
  letter-spacing:1.5px;
  color:#fff;
  text-shadow:0 1px 3px rgba(0,0,0,.6);
}
.cam-hud-tr{
  position:absolute;
  top:10px;right:12px;
  font-size:.7rem;
  color:rgba(255,255,255,.8);
  text-shadow:0 1px 3px rgba(0,0,0,.6);
}
.cam-hud-br{
  position:absolute;
  bottom:10px;right:12px;
  font-size:.68rem;
  color:rgba(255,255,255,.6);
}
.corner{
  position:absolute;
  width:14px;height:14px;
  border-color:rgba(255,255,255,.4);
  border-style:solid;
}
.corner-tl{top:8px;left:8px;border-width:2px 0 0 2px}
.corner-tr{top:8px;right:8px;border-width:2px 2px 0 0}
.corner-bl{bottom:8px;left:8px;border-width:0 0 2px 2px}
.corner-br{bottom:8px;right:8px;border-width:0 2px 2px 0}
.cam-footer{
  padding:10px 16px;
  display:flex;
  align-items:center;
  gap:12px;
  border-top:1px solid var(--border);
  background:#fafafa;
}
.progress-bar{
  flex:1;
  height:3px;
  background:var(--border);
  border-radius:2px;
  overflow:hidden;
}
.progress-fill{
  height:100%;
  background:var(--accent);
  border-radius:2px;
  transition:width linear;
}
/* ── Sidebar ── */
.sidebar{display:flex;flex-direction:column;gap:12px}
.panel{
  background:var(--white);
  border:1px solid var(--border);
  border-radius:10px;
  box-shadow:var(--shadow);
  overflow:hidden;
}
.panel-body{padding:14px 16px;display:flex;flex-direction:column;gap:10px}
/* ── Controls ── */
.ctrl-row{display:flex;flex-direction:column;gap:4px}
.ctrl-label{
  font-size:.68rem;
  color:var(--text-sub);
  display:flex;justify-content:space-between;
}
.ctrl-label b{color:var(--text);font-weight:600}
select{
  width:100%;
  background:var(--white);
  border:1px solid var(--border);
  border-radius:6px;
  color:var(--text);
  padding:6px 10px;
  font-size:.78rem;
  cursor:pointer;
  outline:none;
  transition:border-color .15s;
  font-family:inherit;
}
select:hover,select:focus{border-color:#93c5fd}
input[type=range]{
  width:100%;
  accent-color:var(--accent);
  cursor:pointer;
}
/* ── Alert List ── */
.alert-item{
  display:flex;
  gap:10px;
  padding:9px 0;
  border-bottom:1px solid var(--border);
}
.alert-item:last-child{border-bottom:none}
.alert-icon{
  width:20px;height:20px;
  border-radius:50%;
  flex-shrink:0;
  display:flex;align-items:center;justify-content:center;
  font-size:.7rem;
  margin-top:1px;
}
.alert-icon.ok{background:var(--green-light);color:var(--green)}
.alert-icon.warn{background:var(--orange-light);color:var(--orange)}
.alert-icon.info{background:var(--accent-light);color:var(--accent)}
.alert-text{flex:1;line-height:1.4}
.alert-title{font-size:.76rem;font-weight:600;color:var(--text)}
.alert-sub{font-size:.68rem;color:var(--text-sub);margin-top:1px}
.alert-badge{
  font-size:.62rem;
  padding:1px 6px;
  border-radius:10px;
  font-weight:600;
  margin-top:2px;
  display:inline-block;
}
.badge-ok{background:var(--green-light);color:var(--green)}
.badge-warn{background:var(--orange-light);color:var(--orange)}
.badge-info{background:var(--accent-light);color:var(--accent)}
/* ── Buttons ── */
.btn-row{display:flex;gap:8px}
.btn{
  flex:1;
  padding:8px 10px;
  border-radius:7px;
  border:1px solid var(--border);
  cursor:pointer;
  font-size:.75rem;
  font-weight:600;
  font-family:inherit;
  transition:all .15s;
  background:var(--white);
  color:var(--text);
  display:flex;align-items:center;justify-content:center;gap:4px;
}
.btn:hover{background:#f1f5f9;border-color:#cbd5e1}
.btn-primary{
  background:var(--accent);
  border-color:var(--accent);
  color:#fff;
}
.btn-primary:hover{background:#1d4ed8;border-color:#1d4ed8}
/* ── Toggle ── */
.toggle-row{display:flex;align-items:center;justify-content:space-between}
.toggle{
  position:relative;
  width:36px;height:20px;
}
.toggle input{opacity:0;width:0;height:0}
.toggle-slider{
  position:absolute;inset:0;
  background:#d1d5db;
  border-radius:20px;
  cursor:pointer;
  transition:.2s;
}
.toggle-slider:before{
  content:'';
  position:absolute;
  width:14px;height:14px;
  border-radius:50%;
  background:#fff;
  left:3px;top:3px;
  transition:.2s;
  box-shadow:0 1px 3px rgba(0,0,0,.2);
}
.toggle input:checked + .toggle-slider{background:var(--accent)}
.toggle input:checked + .toggle-slider:before{transform:translateX(16px)}
/* ── Divider ── */
.divider{height:1px;background:var(--border);margin:2px 0}
/* ── Spinner ── */
.spinner{
  width:14px;height:14px;
  border:2px solid var(--border);
  border-top-color:var(--accent);
  border-radius:50%;
  animation:spin .6s linear infinite;
  display:none;
}
.spinner.show{display:block}
/* ── Timestamp ── */
#clock-display{
  font-size:.72rem;
  color:var(--text-sub);
  font-variant-numeric:tabular-nums;
  min-width:60px;
  text-align:right;
}
</style>
</head>
<body>
<!-- Header -->
<div class="header">
  <div class="header-dot"></div>
  <div class="header-title">FlowMeter CAM &mdash; 카메라 모니터링</div>
  <div class="header-right">
    <div class="live-chip"><div class="live-chip-dot"></div>LIVE</div>
    <span>&#9679; 실시간 &nbsp;<span id="clock-display">--:--:--</span></span>
  </div>
</div>

<div class="page">
  <!-- KPI Row -->
  <div class="kpi-row">
    <div class="kpi-card">
      <div class="kpi-label">갱신 주기</div>
      <div class="kpi-value"><span id="kpi-interval">1.0</span><span class="kpi-unit">초</span></div>
      <div class="kpi-status">
        <div class="status-dot dot-green"></div>
        <span class="text-green" id="kpi-status-text">자동 갱신 중</span>
      </div>
    </div>
    <div class="kpi-card">
      <div class="kpi-label">캡처 횟수</div>
      <div class="kpi-value"><span id="kpi-count">0</span><span class="kpi-unit">회</span></div>
      <div class="kpi-status">
        <div class="status-dot dot-blue"></div>
        <span class="text-blue">누적 캡처</span>
      </div>
    </div>
    <div class="kpi-card">
      <div class="kpi-label">설정 해상도</div>
      <div class="kpi-value"><span id="kpi-res-name" style="font-size:.88rem;letter-spacing:-.01em">-</span></div>
      <div class="kpi-status">
        <div class="status-dot dot-green" id="kpi-res-dot"></div>
        <span id="kpi-res-label" class="text-green">실측: 대기 중</span>
      </div>
    </div>
    <div class="kpi-card">
      <div class="kpi-label">연결 상태</div>
      <div class="kpi-value"><span id="kpi-conn" style="font-size:1.1rem">정상</span></div>
      <div class="kpi-status">
        <div class="status-dot dot-green" id="kpi-conn-dot"></div>
        <span class="text-green" id="kpi-conn-text">ESP32-S3 연결됨</span>
      </div>
    </div>
  </div>

  <!-- Main Content -->
  <div class="content-row">
    <!-- Camera Feed -->
    <div class="cam-card">
      <div class="card-header">
        <div class="card-title-bar"></div>
        <div class="card-title">카메라 영상 (실시간)</div>
        <div class="card-header-right">
          <div class="spinner" id="spinner"></div>
          <span style="font-size:.7rem;color:var(--text-dim)" id="last-update">마지막 갱신: -</span>
        </div>
      </div>
      <div class="cam-wrap">
        <img id="cam" src="/capture" alt="Camera Feed">
        <div class="cam-hud">
          <div class="corner corner-tl"></div>
          <div class="corner corner-tr"></div>
          <div class="corner corner-bl"></div>
          <div class="corner corner-br"></div>
          <div class="cam-hud-tl">
            <div style="width:7px;height:7px;border-radius:50%;background:#ef4444;animation:blink .9s infinite"></div>
            REC
          </div>
          <div class="cam-hud-tr" id="hud-time">--:--:--</div>
          <div class="cam-hud-br" id="hud-res">-</div>
        </div>
      </div>
      <div class="cam-footer">
        <span style="font-size:.7rem;color:var(--text-sub)">다음 갱신</span>
        <div class="progress-bar">
          <div class="progress-fill" id="progress" style="width:0%"></div>
        </div>
        <span style="font-size:.7rem;color:var(--text-dim)" id="countdown">-</span>
      </div>
    </div>

    <!-- Sidebar -->
    <div class="sidebar">
      <!-- Status Alerts -->
      <div class="panel">
        <div class="card-header">
          <div class="card-title-bar"></div>
          <div class="card-title">시스템 상태</div>
        </div>
        <div class="panel-body" style="padding:0 16px">
          <div class="alert-item">
            <div class="alert-icon ok">&#10003;</div>
            <div class="alert-text">
              <div class="alert-title">카메라 정상 동작</div>
              <div class="alert-sub" id="alert-cam-sub">연결 확인 중...</div>
              <span class="alert-badge badge-ok">정상</span>
            </div>
          </div>
          <div class="alert-item">
            <div class="alert-icon info">&#8635;</div>
            <div class="alert-text">
              <div class="alert-title">자동 갱신</div>
              <div class="alert-sub" id="alert-refresh-sub">1초 주기 갱신 활성</div>
              <span class="alert-badge badge-info" id="alert-refresh-badge">동작 중</span>
            </div>
          </div>
          <div class="alert-item">
            <div class="alert-icon warn">&#9432;</div>
            <div class="alert-text">
              <div class="alert-title">OV26xx 시리즈</div>
              <div class="alert-sub">ESP32-S3 카메라 모듈</div>
              <span class="alert-badge badge-warn">활성</span>
            </div>
          </div>
        </div>
      </div>

      <!-- Camera Controls -->
      <div class="panel">
        <div class="card-header">
          <div class="card-title-bar"></div>
          <div class="card-title">카메라 설정</div>
        </div>
        <div class="panel-body">
          <div class="ctrl-row">
            <div class="ctrl-label"><span>프레임 크기</span></div>
            <select id="framesize" onchange="onFramesizeChange(this)">
              <option value="1">QQVGA 160x120</option>
              <option value="5">QVGA 320x240</option>
              <option value="8">VGA 640x480</option>
              <option value="9">SVGA 800x600</option>
              <option value="10">XGA 1024x768</option>
              <option value="11" selected>HD 1280x720</option>
              <option value="12">SXGA 1280x1024</option>
              <option value="13">UXGA 1600x1200</option>
              <option value="21">WQXGA 2560x1600 (Max)</option>
            </select>
          </div>
          <div class="ctrl-row">
            <div class="ctrl-label"><span>화질 (Quality)</span><b id="qval">12</b></div>
            <input type="range" id="quality" min="10" max="63" value="12"
              oninput="document.getElementById('qval').textContent=this.value"
              onchange="setCtrl('quality',this.value)">
          </div>
          <div class="ctrl-row">
            <div class="ctrl-label"><span>밝기</span><b id="bval">0</b></div>
            <input type="range" id="brightness" min="-2" max="2" value="0"
              oninput="document.getElementById('bval').textContent=this.value"
              onchange="setCtrl('brightness',this.value)">
          </div>
          <div class="ctrl-row">
            <div class="ctrl-label"><span>대비</span><b id="cval">0</b></div>
            <input type="range" id="contrast" min="-2" max="2" value="0"
              oninput="document.getElementById('cval').textContent=this.value"
              onchange="setCtrl('contrast',this.value)">
          </div>
          <div class="divider"></div>
          <div class="toggle-row">
            <span style="font-size:.75rem">좌우 반전</span>
            <label class="toggle"><input type="checkbox" id="hmirror" onchange="setCtrl('hmirror',this.checked?1:0)"><span class="toggle-slider"></span></label>
          </div>
          <div class="toggle-row">
            <span style="font-size:.75rem">상하 반전</span>
            <label class="toggle"><input type="checkbox" id="vflip" onchange="setCtrl('vflip',this.checked?1:0)"><span class="toggle-slider"></span></label>
          </div>
        </div>
      </div>

      <!-- Interval Control -->
      <div class="panel">
        <div class="card-header">
          <div class="card-title-bar"></div>
          <div class="card-title">갱신 주기 설정</div>
        </div>
        <div class="panel-body">
          <div class="ctrl-row">
            <div class="ctrl-label"><span>자동 갱신 간격</span></div>
            <select id="interval" onchange="setRefreshInterval(parseInt(this.value))">
              <option value="500">0.5초 (빠름)</option>
              <option value="1000" selected>1초 (기본)</option>
              <option value="2000">2초</option>
              <option value="5000">5초</option>
              <option value="10000">10초 (절전)</option>
            </select>
          </div>
          <div class="btn-row">
            <button class="btn btn-primary" onclick="captureNow()">&#128247; 즉시 캡처</button>
            <button class="btn" onclick="togglePause()" id="pauseBtn">&#9646;&#9646; 일시정지</button>
          </div>
          <div class="btn-row" style="margin-top:6px">
            <button class="btn" id="streamBtn" onclick="toggleStream()">&#128225; 스트리밍 시작</button>
          </div>
        </div>
      </div>
    </div>
  </div>
</div>

<script>
var refreshMs=1000, timer=null, paused=false, count=0;
var progressTimer=null, progressStart=0;
var streaming=false;
var STREAM_URL='http://'+window.location.hostname+':81/stream';
var img=document.getElementById('cam');

// 프레임사이즈 인덱스 → 실제 픽셀 크기 매핑 (Arduino ESP32 SDK 3.x enum 기준 OV5640 지원 해상도)
var FRAMESIZE_WH = {
  1:[160,120], 5:[320,240], 8:[640,480], 9:[800,600],
  10:[1024,768], 11:[1280,720], 12:[1280,1024], 13:[1600,1200],
  21:[2560,1600]
};
var currentFramesizeIdx = -1;

function pad(n){return n.toString().padStart(2,'0')}

function updateClock(){
  var now=new Date();
  var s=pad(now.getHours())+':'+pad(now.getMinutes())+':'+pad(now.getSeconds());
  document.getElementById('clock-display').textContent=s;
  document.getElementById('hud-time').textContent=s;
}
setInterval(updateClock,1000);
updateClock();

function startProgress(){
  if(progressTimer)clearInterval(progressTimer);
  progressStart=Date.now();
  var fill=document.getElementById('progress');
  var cd=document.getElementById('countdown');
  progressTimer=setInterval(function(){
    var elapsed=Date.now()-progressStart;
    var pct=Math.min(elapsed/refreshMs*100,100);
    fill.style.width=pct+'%';
    var rem=Math.max(0,(refreshMs-elapsed)/1000);
    cd.textContent=rem.toFixed(1)+'s';
  },50);
}

function refresh(){
  if(paused)return;
  var t=Date.now();
  var spinner=document.getElementById('spinner');
  spinner.classList.add('show');
  img.classList.add('updating');
  var url='/capture?t='+t;
  var tmp=new Image();
  tmp.onload=function(){
    img.src=url;
    img.classList.remove('updating');
    spinner.classList.remove('show');
    count++;
    document.getElementById('kpi-count').textContent=count;
    var w=tmp.naturalWidth,h=tmp.naturalHeight;
    var actualStr=(w&&h)?(w+'x'+h):'-';
    document.getElementById('kpi-res-label').textContent='실측: '+actualStr;
    document.getElementById('hud-res').textContent=actualStr;
    // 설정값과 실측값 비교
    var expected=FRAMESIZE_WH[currentFramesizeIdx];
    var match=expected&&w&&h&&(expected[0]===w)&&(expected[1]===h);
    var mismatch=expected&&w&&h&&!match;
    document.getElementById('kpi-res-dot').className='status-dot '+(mismatch?'dot-orange':'dot-green');
    document.getElementById('kpi-res-label').style.color=mismatch?'var(--orange)':'var(--green)';
    if(mismatch){
      document.getElementById('kpi-res-label').textContent='실측: '+actualStr+' ≠ 설정';
    }
    var now=new Date();
    var ts=pad(now.getHours())+':'+pad(now.getMinutes())+':'+pad(now.getSeconds());
    document.getElementById('last-update').textContent='마지막 갱신: '+ts;
    document.getElementById('alert-cam-sub').textContent='오늘 '+ts+' · 자동 갱신';
    setConn(true);
    startProgress();
  };
  tmp.onerror=function(){
    img.classList.remove('updating');
    spinner.classList.remove('show');
    setConn(false);
  };
  tmp.src=url;
}

function setConn(ok){
  var el=document.getElementById('kpi-conn');
  var dot=document.getElementById('kpi-conn-dot');
  var txt=document.getElementById('kpi-conn-text');
  if(ok){
    el.textContent='정상';
    dot.className='status-dot dot-green';
    txt.className='text-green';
    txt.textContent='ESP32-S3 연결됨';
  } else {
    el.textContent='오류';
    dot.className='status-dot dot-red';
    txt.className='text-red';
    txt.textContent='연결 실패 — 재시도 중';
  }
}

function setRefreshInterval(ms){
  refreshMs=ms;
  document.getElementById('kpi-interval').textContent=(ms/1000).toFixed(1);
  document.getElementById('alert-refresh-sub').textContent=(ms/1000).toFixed(1)+'초 주기 갱신 활성';
  if(timer)clearInterval(timer);
  timer=setInterval(refresh,ms);
  startProgress();
}

function togglePause(){
  paused=!paused;
  var btn=document.getElementById('pauseBtn');
  var badge=document.getElementById('alert-refresh-badge');
  var statusTxt=document.getElementById('kpi-status-text');
  if(paused){
    btn.innerHTML='&#9654; 재시작';
    badge.className='alert-badge badge-warn';
    badge.textContent='일시정지';
    statusTxt.className='text-orange';
    statusTxt.textContent='일시정지 됨';
    if(progressTimer)clearInterval(progressTimer);
    document.getElementById('progress').style.width='0%';
  } else {
    btn.innerHTML='&#9646;&#9646; 일시정지';
    badge.className='alert-badge badge-info';
    badge.textContent='동작 중';
    statusTxt.className='text-green';
    statusTxt.textContent='자동 갱신 중';
    refresh();
  }
}

function captureNow(){
  window.open('/capture','_blank');
}

function onFramesizeChange(sel){
  var idx=parseInt(sel.value);
  currentFramesizeIdx=idx;
  var name=sel.options[sel.selectedIndex].text;
  document.getElementById('kpi-res-name').textContent=name;
  document.getElementById('kpi-res-label').textContent='센서 전환 중...';
  document.getElementById('kpi-res-label').style.color='var(--text-sub)';
  document.getElementById('kpi-res-dot').className='status-dot dot-blue';
  if(streaming){
    // 스트리밍 중: 스트림 끊고 해상도 변경 후 재연결
    img.src='';
    fetch('/control?var=framesize&val='+sel.value).then(function(){
      setTimeout(function(){
        img.src=STREAM_URL;
        document.getElementById('kpi-res-label').textContent='스트리밍 중';
        document.getElementById('kpi-res-label').style.color='var(--green)';
        document.getElementById('kpi-res-dot').className='status-dot dot-green';
      },800);
    }).catch(function(){ img.src=STREAM_URL; });
  } else {
    paused=true;
    fetch('/control?var=framesize&val='+sel.value).then(function(){
      setTimeout(function(){
        paused=false;
        refresh();
      },500);
    }).catch(function(){ paused=false; });
  }
}

function startStream(){
  streaming=true;
  paused=true;
  if(timer)clearInterval(timer);
  if(progressTimer)clearInterval(progressTimer);
  img.src=STREAM_URL;
  var btn=document.getElementById('streamBtn');
  btn.innerHTML='&#9646;&#9646; 스트리밍 중지';
  btn.className='btn btn-primary';
  document.getElementById('pauseBtn').disabled=true;
  document.getElementById('progress').style.width='100%';
  document.getElementById('countdown').textContent='LIVE';
  document.getElementById('kpi-status-text').textContent='스트리밍 중';
  document.getElementById('kpi-status-text').className='text-red';
  document.getElementById('alert-refresh-sub').textContent='MJPEG 스트리밍 활성';
  document.getElementById('alert-refresh-badge').textContent='LIVE';
  document.getElementById('alert-refresh-badge').className='alert-badge badge-ok';
  document.getElementById('kpi-res-label').textContent='스트리밍 중';
  document.getElementById('kpi-res-label').style.color='var(--green)';
  document.getElementById('kpi-res-dot').className='status-dot dot-green';
}

function stopStream(){
  streaming=false;
  img.src='';
  var btn=document.getElementById('streamBtn');
  btn.innerHTML='&#128225; 스트리밍 시작';
  btn.className='btn';
  document.getElementById('pauseBtn').disabled=false;
  document.getElementById('countdown').textContent='-';
  document.getElementById('progress').style.width='0%';
  document.getElementById('alert-refresh-sub').textContent=(refreshMs/1000).toFixed(1)+'초 주기 갱신 활성';
  document.getElementById('alert-refresh-badge').textContent='동작 중';
  document.getElementById('alert-refresh-badge').className='alert-badge badge-info';
  paused=false;
  setRefreshInterval(refreshMs);
}

function toggleStream(){
  if(streaming) stopStream(); else startStream();
}

function setFramesizeFromStatus(idx){
  var sel=document.getElementById('framesize');
  sel.value=idx;
  currentFramesizeIdx=idx;
  var name=sel.options[sel.selectedIndex]?sel.options[sel.selectedIndex].text:'-';
  document.getElementById('kpi-res-name').textContent=name;
}

function setCtrl(v,val){
  fetch('/control?var='+v+'&val='+val).catch(function(){});
}

fetch('/status').then(function(r){return r.json()}).then(function(s){
  if(s.framesize!==undefined)setFramesizeFromStatus(s.framesize);
  if(s.quality!==undefined){document.getElementById('quality').value=s.quality;document.getElementById('qval').textContent=s.quality}
  if(s.brightness!==undefined){document.getElementById('brightness').value=s.brightness;document.getElementById('bval').textContent=s.brightness}
  if(s.contrast!==undefined){document.getElementById('contrast').value=s.contrast;document.getElementById('cval').textContent=s.contrast}
  if(s.hmirror!==undefined)document.getElementById('hmirror').checked=!!s.hmirror;
  if(s.vflip!==undefined)document.getElementById('vflip').checked=!!s.vflip;
}).catch(function(){});

setRefreshInterval(1000);
</script>
</body>
</html>)rawhtml";

static const size_t index_html_len = sizeof(index_html) - 1;
