#include "camera.h"
#include "config.h"
#include <Arduino.h>
#include <cstring>
#include <esp_camera.h>

#define PART_BOUNDARY "123456789000000000000987654321"
static const char *STREAM_CONTENT_TYPE =
    "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char *STREAM_PART =
    "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

static const char INDEX_HTML[] = R"GCAM(
<!DOCTYPE html><html><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>GripLink</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{background:#0d0f12;color:#c8d0e0;font-family:'IBM Plex Mono',monospace;font-size:12px;height:100vh;display:flex;flex-direction:column;overflow:hidden}
.hdr{display:flex;align-items:center;gap:12px;padding:7px 14px;border-bottom:1px solid #1e2330;flex-shrink:0}
.hdr h1{font-size:12px;letter-spacing:.15em;text-transform:uppercase;color:#00e5ff}
.mid{display:grid;grid-template-columns:282px 1fr;gap:1px;background:#1e2330;flex-shrink:0}
.btm{display:grid;grid-template-columns:repeat(4,1fr);gap:1px;background:#1e2330;flex:1;min-height:0}
.pnl{background:#0d0f12;padding:10px;overflow:hidden}
.ttl{font-size:10px;letter-spacing:.1em;text-transform:uppercase;color:#4a5568;margin-bottom:7px}
#camwrap{position:relative;display:inline-block}
#cam{display:block;width:280px;height:210px;object-fit:fill}
#ov{position:absolute;top:0;left:0;width:280px;height:210px;pointer-events:none}
#imuC{display:block}
.euler{display:flex;gap:14px;margin-top:5px;flex-wrap:wrap}
.edot{display:inline-block;width:9px;height:3px;border-radius:2px;margin-right:3px;vertical-align:middle}
.eval{color:#fff;font-weight:bold;min-width:52px}
.batV{font-size:22px;color:#fff;margin:5px 0 3px}
.bar{height:8px;background:#1e2330;border-radius:4px;overflow:hidden;margin:3px 0}
.barF{height:100%;border-radius:4px;transition:width .5s,background .5s}
.dim{color:#4a5568;font-size:11px}
.wlist{max-height:110px;overflow-y:auto;margin:3px 0}
.wrow{display:flex;align-items:center;gap:5px;padding:3px 4px;border-radius:3px}
.wrow:hover{background:#1e2330}
.wssid{flex:1;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.wrssi{color:#4a5568;font-size:11px;width:44px;text-align:right}
.wbtn{background:#00e5ff15;color:#00e5ff;border:1px solid #00e5ff30;border-radius:3px;font-size:11px;padding:2px 6px;cursor:pointer;font-family:inherit}
.wbtn:hover{background:#00e5ff30}
.sbtn{width:100%;margin-top:4px;background:#1e2330;color:#c8d0e0;border:1px solid #2e3440;border-radius:3px;padding:3px 0;cursor:pointer;font-size:11px;font-family:inherit}
.sbtn:hover{background:#2e3440}
.wcur{color:#4a5568;font-size:11px;margin-bottom:3px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.oly{display:none;position:fixed;inset:0;background:rgba(0,0,0,.6);z-index:50}
.mdl{display:none;position:fixed;top:50%;left:50%;transform:translate(-50%,-50%);background:#13161c;border:1px solid #2e3440;border-radius:8px;padding:15px;width:252px;z-index:51}
.mdl input{width:100%;background:#1e2330;border:1px solid #2e3440;border-radius:3px;padding:5px 7px;color:#c8d0e0;font-family:inherit;font-size:12px;margin:5px 0}
.mbtns{display:flex;gap:8px;justify-content:flex-end;margin-top:8px}
.mok{background:#00e5ff20;color:#00e5ff;border:1px solid #00e5ff40;border-radius:3px;padding:3px 14px;cursor:pointer;font-family:inherit}
.mcn{background:#1e2330;color:#4a5568;border:1px solid #2e3440;border-radius:3px;padding:3px 14px;cursor:pointer;font-family:inherit}
.crow{display:flex;align-items:center;gap:7px;margin:4px 0}
.clbl{color:#4a5568;width:72px;flex-shrink:0;font-size:11px}
.csel{flex:1;background:#1e2330;border:1px solid #2e3440;border-radius:3px;color:#c8d0e0;font-family:inherit;font-size:11px;padding:2px 4px;min-width:0}
.cslr{flex:1;cursor:pointer;accent-color:#00e5ff;min-width:0}
.cval{color:#fff;width:26px;text-align:right;font-size:11px}
.tog{display:flex;flex:1}
.tbtn{flex:1;background:#1e2330;border:1px solid #2e3440;color:#4a5568;font-family:inherit;font-size:11px;padding:2px 0;cursor:pointer;text-align:center}
.tbtn.on{background:#00e5ff20;color:#00e5ff;border-color:#00e5ff40}
.tbtn:first-child{border-radius:3px 0 0 3px;border-right:none}
.tbtn:last-child{border-radius:0 3px 3px 0}
#csPnl{overflow-y:auto}
#drop{border:2px dashed #2e3440;border-radius:5px;padding:16px 8px;text-align:center;color:#4a5568;cursor:pointer;transition:.2s;font-size:11px}
#drop.dg{border-color:#00e5ff;color:#00e5ff;background:#00e5ff08}
#drop.up{border-color:#44ff88;color:#44ff88}
.obar{height:5px;background:#1e2330;border-radius:3px;overflow:hidden;margin-top:6px;display:none}
.obarF{height:100%;background:#44ff88;width:0;border-radius:3px;transition:width .1s}
#ost{color:#4a5568;font-size:11px;margin-top:4px;min-height:14px}
</style>
</head>
<body>
<div class="hdr">
  <h1>GripLink Wrist</h1>
  <span id="conn" style="color:#4a5568">&#9679; connecting</span>
</div>
<div class="mid">
  <div class="pnl">
    <div class="ttl">Camera</div>
    <div id="camwrap">
      <img id="cam" src="/stream">
      <canvas id="ov" width="280" height="210"></canvas>
    </div>
  </div>
  <div class="pnl">
    <div class="ttl">IMU &#8212; Euler Angles</div>
    <canvas id="imuC" height="145"></canvas>
    <div class="euler">
      <span><span class="edot" style="background:#ff4444"></span>Roll <span class="eval" id="eR">0.0&#176;</span></span>
      <span><span class="edot" style="background:#44ff88"></span>Pitch <span class="eval" id="eP">0.0&#176;</span></span>
      <span><span class="edot" style="background:#4488ff"></span>Yaw <span class="eval" id="eY">0.0&#176;</span></span>
    </div>
  </div>
</div>
<div class="btm">
  <div class="pnl">
    <div class="ttl">Battery</div>
    <div class="batV" id="batV">&#8212;</div>
    <div class="bar"><div class="barF" id="batF" style="width:0%"></div></div>
    <div class="dim" id="batP">&#8212;</div>
  </div>
  <div class="pnl">
    <div class="ttl">WiFi</div>
    <div class="wcur" id="wCur">&#8212;</div>
    <div class="wlist" id="wList"></div>
    <button class="sbtn" onclick="wScan()">Scan networks</button>
  </div>
  <div class="pnl" id="csPnl">
    <div class="ttl">Camera Settings</div>
    <div class="crow">
      <span class="clbl">Resolution</span>
      <select class="csel" id="sFS">
        <option value="1">160&#215;120 QQVGA</option>
        <option value="4">240&#215;240</option>
        <option value="5">320&#215;240 QVGA</option>
        <option value="8">640&#215;480 VGA</option>
        <option value="9" selected>800&#215;600 SVGA</option>
        <option value="10">1024&#215;768 XGA</option>
        <option value="13">1600&#215;1200 UXGA</option>
      </select>
    </div>
    <div class="crow">
      <span class="clbl">Quality</span>
      <input class="cslr" type="range" min="4" max="63" value="12" id="sQ"
        oninput="document.getElementById('vQ').textContent=this.value">
      <span class="cval" id="vQ">12</span>
    </div>
    <div class="crow">
      <span class="clbl">Brightness</span>
      <input class="cslr" type="range" min="-2" max="2" value="0" id="sB"
        oninput="document.getElementById('vB').textContent=this.value">
      <span class="cval" id="vB">0</span>
    </div>
    <div class="crow">
      <span class="clbl">Contrast</span>
      <input class="cslr" type="range" min="-2" max="2" value="0" id="sC"
        oninput="document.getElementById('vC').textContent=this.value">
      <span class="cval" id="vC">0</span>
    </div>
    <div class="crow">
      <span class="clbl">Saturation</span>
      <input class="cslr" type="range" min="-2" max="2" value="0" id="sS"
        oninput="document.getElementById('vS').textContent=this.value">
      <span class="cval" id="vS">0</span>
    </div>
    <div class="crow">
      <span class="clbl">H-Mirror</span>
      <div class="tog">
        <button class="tbtn" id="mOn" onclick="ton('mOn','mOff')">On</button>
        <button class="tbtn on" id="mOff" onclick="ton('mOff','mOn')">Off</button>
      </div>
    </div>
    <div class="crow">
      <span class="clbl">V-Flip</span>
      <div class="tog">
        <button class="tbtn" id="fOn" onclick="ton('fOn','fOff')">On</button>
        <button class="tbtn on" id="fOff" onclick="ton('fOff','fOn')">Off</button>
      </div>
    </div>
    <div class="crow">
      <span class="clbl">AWB</span>
      <div class="tog">
        <button class="tbtn on" id="aOn" onclick="ton('aOn','aOff')">On</button>
        <button class="tbtn" id="aOff" onclick="ton('aOff','aOn')">Off</button>
      </div>
    </div>
    <div class="crow">
      <span class="clbl">AEC</span>
      <div class="tog">
        <button class="tbtn on" id="eOn" onclick="ton('eOn','eOff')">On</button>
        <button class="tbtn" id="eOff" onclick="ton('eOff','eOn')">Off</button>
      </div>
    </div>
    <button class="sbtn" id="applyBtn" onclick="cApply(this)" style="margin-top:6px;background:#00e5ff20;color:#00e5ff;border-color:#00e5ff40">Apply Settings</button>
  </div>
  <div class="pnl">
    <div class="ttl">OTA Update</div>
    <div id="drop">Drop firmware.bin here<br><span style="font-size:10px">or click to browse</span>
      <input type="file" id="otaF" accept=".bin" style="display:none" onchange="otaUp(this.files[0])">
    </div>
    <div class="obar" id="oBar"><div class="obarF" id="oFill"></div></div>
    <div id="ost"></div>
  </div>
</div>
<div class="oly" id="oly" onclick="closeM()"></div>
<div class="mdl" id="mdl">
  <div style="color:#c8d0e0;margin-bottom:6px">Connect to WiFi</div>
  <div id="mSSID" style="color:#00e5ff;font-size:11px;margin-bottom:2px"></div>
  <input type="password" id="mPass" placeholder="Password" onkeydown="if(event.key==='Enter')wConn()">
  <div class="mbtns">
    <button class="mcn" onclick="closeM()">Cancel</button>
    <button class="mok" onclick="wConn()">Connect</button>
  </div>
</div>
<script>
var quat=[1,0,0,0],tags=[],cSSID='';
var HIST=200,rB=new Float32Array(HIST),pB=new Float32Array(HIST),yB=new Float32Array(HIST);

function qMat(w,x,y,z){
  return[[1-2*(y*y+z*z),2*(x*y-w*z),2*(x*z+w*y)],
         [2*(x*y+w*z),1-2*(x*x+z*z),2*(y*z-w*x)],
         [2*(x*z-w*y),2*(y*z+w*x),1-2*(x*x+y*y)]];
}
function qEul(w,x,y,z){
  var r=Math.atan2(2*(w*x+y*z),1-2*(x*x+y*y))*180/Math.PI;
  var s=2*(w*y-z*x);
  var p=Math.abs(s)>=1?Math.sign(s)*90:Math.asin(s)*180/Math.PI;
  var y2=Math.atan2(2*(w*z+x*y),1-2*(y*y+z*z))*180/Math.PI;
  return[r,p,y2];
}

var OV=document.getElementById('ov'),oc=OV.getContext('2d');

function proj(R,v,ox,oy,s){
  var xr=R[0][0]*v[0]+R[0][1]*v[1]+R[0][2]*v[2];
  var yr=R[1][0]*v[0]+R[1][1]*v[1]+R[1][2]*v[2];
  var zr=R[2][0]*v[0]+R[2][1]*v[1]+R[2][2]*v[2];
  var d=2.5+zr;
  return[ox+s*xr/d,oy-s*yr/d];
}
function dAx(R,v,ox,oy,s,col,lbl){
  var pt=proj(R,v,ox,oy,s),tx=pt[0],ty=pt[1];
  oc.strokeStyle=col;oc.fillStyle=col;oc.lineWidth=2;
  oc.beginPath();oc.moveTo(ox,oy);oc.lineTo(tx,ty);oc.stroke();
  var a=Math.atan2(ty-oy,tx-ox),h=7;
  oc.beginPath();oc.moveTo(tx,ty);
  oc.lineTo(tx-h*Math.cos(a-.4),ty-h*Math.sin(a-.4));
  oc.lineTo(tx-h*Math.cos(a+.4),ty-h*Math.sin(a+.4));
  oc.closePath();oc.fill();
  oc.font='bold 10px monospace';oc.fillText(lbl,tx+4,ty+4);
}

var IC=document.getElementById('imuC'),ic=IC.getContext('2d');
function szIC(){IC.width=IC.parentElement.clientWidth-20;}
szIC();window.addEventListener('resize',szIC);

function drawPlot(){
  var W=IC.width,H=IC.height;
  ic.fillStyle='#0d0f12';ic.fillRect(0,0,W,H);
  var grid=[-180,-90,0,90,180];
  for(var gi=0;gi<grid.length;gi++){
    var d=grid[gi],gy=H/2-(d/180)*(H/2-4);
    ic.strokeStyle='#1e2330';ic.lineWidth=1;
    ic.beginPath();ic.moveTo(0,gy);ic.lineTo(W,gy);ic.stroke();
    ic.fillStyle='#2e3440';ic.font='10px monospace';ic.fillText(d+'deg',2,gy-2);
  }
  var lines=[[rB,'#ff4444'],[pB,'#44ff88'],[yB,'#4488ff']];
  for(var li=0;li<lines.length;li++){
    var buf=lines[li][0],lc=lines[li][1];
    ic.strokeStyle=lc;ic.lineWidth=1.5;ic.beginPath();
    for(var i=0;i<HIST;i++){
      var x=i/(HIST-1)*W,y=H/2-buf[i]/180*(H/2-4);
      if(i===0)ic.moveTo(x,y);else ic.lineTo(x,y);
    }
    ic.stroke();
  }
}

function render(){
  var W=OV.width,H=OV.height,sx=W/800,sy=H/600;
  oc.clearRect(0,0,W,H);
  oc.strokeStyle='#00e5ff';oc.fillStyle='#00e5ff';oc.lineWidth=1.5;oc.font='bold 10px monospace';
  for(var ti=0;ti<tags.length;ti++){
    var t=tags[ti],c=t.corners;
    oc.beginPath();
    oc.moveTo(c[0][0]*sx,c[0][1]*sy);oc.lineTo(c[1][0]*sx,c[1][1]*sy);
    oc.lineTo(c[2][0]*sx,c[2][1]*sy);oc.lineTo(c[3][0]*sx,c[3][1]*sy);
    oc.closePath();oc.stroke();
    oc.fillText('ID:'+t.id+' z:'+t.pose.tz.toFixed(2)+'m',c[0][0]*sx+3,c[0][1]*sy-3);
  }
  var ax=48,ay=H-26,as=38;
  oc.fillStyle='rgba(13,15,18,.6)';
  oc.beginPath();oc.arc(ax,ay,as+6,0,2*Math.PI);oc.fill();
  var R=qMat(quat[0],quat[1],quat[2],quat[3]);
  dAx(R,[1,0,0],ax,ay,as,'#ff4444','X');
  dAx(R,[0,1,0],ax,ay,as,'#44ff88','Y');
  dAx(R,[0,0,1],ax,ay,as,'#4488ff','Z');
  var eu=qEul(quat[0],quat[1],quat[2],quat[3]),r=eu[0],p=eu[1],y2=eu[2];
  rB.copyWithin(0,1);rB[HIST-1]=r;
  pB.copyWithin(0,1);pB[HIST-1]=p;
  yB.copyWithin(0,1);yB[HIST-1]=y2;
  document.getElementById('eR').textContent=r.toFixed(1)+'deg';
  document.getElementById('eP').textContent=p.toFixed(1)+'deg';
  document.getElementById('eY').textContent=y2.toFixed(1)+'deg';
  drawPlot();
  requestAnimationFrame(render);
}

async function pollPose(){
  try{
    var d=await(await fetch('/pose')).json();
    quat=d.q;
    var el=document.getElementById('conn');
    el.textContent='online';el.style.color='#44ff88';
  }catch(_){
    var el=document.getElementById('conn');
    el.textContent='offline';el.style.color='#ff4444';
  }
  setTimeout(pollPose,50);
}
async function pollDet(){
  try{tags=(await(await fetch('/detections')).json()).tags;}catch(_){}
  setTimeout(pollDet,200);
}
async function pollBat(){
  try{
    var d=await(await fetch('/battery')).json();
    document.getElementById('batV').textContent=d.voltage.toFixed(2)+'V';
    document.getElementById('batP').textContent=d.percent+'%';
    var f=document.getElementById('batF');
    f.style.width=d.percent+'%';
    f.style.background=d.percent>50?'#44ff88':d.percent>20?'#ffaa00':'#ff4444';
  }catch(_){}
  setTimeout(pollBat,5000);
}

async function wScan(){
  document.getElementById('wList').innerHTML='<div style="padding:5px 0;color:#4a5568">Scanning...</div>';
  try{
    var d=await(await fetch('/wifi/scan')).json();
    document.getElementById('wCur').textContent='STA: '+(d.sta||'--');
    var L=document.getElementById('wList');
    L.innerHTML='';
    var nets=d.networks.slice().sort(function(a,b){return b.rssi-a.rssi;});
    for(var ni=0;ni<nets.length;ni++){
      var n=nets[ni];
      var row=document.createElement('div');row.className='wrow';
      var s=document.createElement('span');s.className='wssid';s.textContent=n.ssid;
      var r2=document.createElement('span');r2.className='wrssi';
      r2.textContent=(n.rssi>-50?'IIII':n.rssi>-70?'III.':'II..')+' '+n.rssi;
      var b=document.createElement('button');b.className='wbtn';b.textContent='->';
      (function(ssid){b.onclick=function(){openM(ssid);};})(n.ssid);
      row.appendChild(s);row.appendChild(r2);row.appendChild(b);
      L.appendChild(row);
    }
  }catch(_){
    document.getElementById('wList').innerHTML='<div style="color:#ff4444;padding:5px 0">Scan failed</div>';
  }
}
function openM(ssid){
  cSSID=ssid;
  document.getElementById('mSSID').textContent=ssid;
  document.getElementById('mPass').value='';
  document.getElementById('mdl').style.display='block';
  document.getElementById('oly').style.display='block';
  document.getElementById('mPass').focus();
}
function closeM(){
  document.getElementById('mdl').style.display='none';
  document.getElementById('oly').style.display='none';
}
async function wConn(){
  var pass=document.getElementById('mPass').value;
  closeM();
  document.getElementById('wCur').textContent='Connecting to '+cSSID+'...';
  try{
    await fetch('/wifi/connect',{method:'POST',
      headers:{'Content-Type':'application/json'},
      body:JSON.stringify({ssid:cSSID,pass:pass})});
  }catch(_){}
}

function cSet(v,val){fetch('/camera/set?var='+v+'&val='+val);}
function cApply(btn){
  cSet('framesize',document.getElementById('sFS').value);
  cSet('quality',document.getElementById('sQ').value);
  cSet('brightness',document.getElementById('sB').value);
  cSet('contrast',document.getElementById('sC').value);
  cSet('saturation',document.getElementById('sS').value);
  cSet('hmirror',document.getElementById('mOn').classList.contains('on')?1:0);
  cSet('vflip',document.getElementById('fOn').classList.contains('on')?1:0);
  cSet('awb',document.getElementById('aOn').classList.contains('on')?1:0);
  cSet('aec',document.getElementById('eOn').classList.contains('on')?1:0);
  var t=btn.textContent;btn.textContent='Applied ✓';
  setTimeout(function(){btn.textContent=t;},1000);
}
function ton(on,off){
  document.getElementById(on).classList.add('on');
  document.getElementById(off).classList.remove('on');
}

var drop=document.getElementById('drop');
drop.addEventListener('click',function(){document.getElementById('otaF').click();});
drop.addEventListener('dragover',function(e){e.preventDefault();drop.classList.add('dg');});
drop.addEventListener('dragleave',function(){drop.classList.remove('dg');});
drop.addEventListener('drop',function(e){e.preventDefault();drop.classList.remove('dg');otaUp(e.dataTransfer.files[0]);});

function otaUp(file){
  if(!file||!file.name.endsWith('.bin')){document.getElementById('ost').textContent='Need a .bin file';return;}
  var bar=document.getElementById('oBar'),fill=document.getElementById('oFill'),st=document.getElementById('ost');
  drop.classList.add('up');
  drop.childNodes[0].textContent='Uploading...';
  bar.style.display='block';
  var xhr=new XMLHttpRequest();
  xhr.open('POST','/ota');
  xhr.setRequestHeader('Content-Type','application/octet-stream');
  xhr.upload.onprogress=function(e){
    if(e.lengthComputable){var p=Math.round(e.loaded/e.total*100);fill.style.width=p+'%';st.textContent=p+'%';}
  };
  xhr.onload=function(){
    if(xhr.status===200){
      st.textContent='Done -- rebooting...';fill.style.width='100%';
    }else{
      st.textContent='Error: '+xhr.responseText;
      drop.classList.remove('up');drop.childNodes[0].textContent='Drop firmware.bin here';
    }
  };
  xhr.onerror=function(){
    st.textContent='Upload failed';
    drop.classList.remove('up');drop.childNodes[0].textContent='Drop firmware.bin here';
  };
  xhr.send(file);
}

pollPose();pollDet();pollBat();requestAnimationFrame(render);
</script>
</body></html>
)GCAM";

bool camera_init() {
  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = CAM_PIN_D0;
  config.pin_d1 = CAM_PIN_D1;
  config.pin_d2 = CAM_PIN_D2;
  config.pin_d3 = CAM_PIN_D3;
  config.pin_d4 = CAM_PIN_D4;
  config.pin_d5 = CAM_PIN_D5;
  config.pin_d6 = CAM_PIN_D6;
  config.pin_d7 = CAM_PIN_D7;
  config.pin_xclk = CAM_PIN_XCLK;
  config.pin_pclk = CAM_PIN_PCLK;
  config.pin_vsync = CAM_PIN_VSYNC;
  config.pin_href = CAM_PIN_HREF;
  config.pin_sccb_sda = CAM_PIN_SIOD;
  config.pin_sccb_scl = CAM_PIN_SIOC;
  config.pin_pwdn = CAM_PIN_PWDN;
  config.pin_reset = CAM_PIN_RESET;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAMESIZE_SVGA;
  config.jpeg_quality = 12;
  config.fb_count = 2;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("[cam] init failed: 0x%x\n", err);
    return false;
  }

  sensor_t *s = esp_camera_sensor_get();
  if (s) {
    s->set_whitebal(s, 1);
    s->set_awb_gain(s, 1);
    s->set_exposure_ctrl(s, 1);
    s->set_aec2(s, 1);
    s->set_gain_ctrl(s, 1);
    s->set_bpc(s, 1);
    s->set_wpc(s, 1);
  }

  Serial.println("[cam] OV2640 ready");
  return true;
}

void camera_set(const char *var, int val) {
  sensor_t *s = esp_camera_sensor_get();
  if (!s) return;
  if      (!strcmp(var, "framesize"))  s->set_framesize(s, (framesize_t)val);
  else if (!strcmp(var, "quality"))    s->set_quality(s, val);
  else if (!strcmp(var, "brightness")) s->set_brightness(s, val);
  else if (!strcmp(var, "contrast"))   s->set_contrast(s, val);
  else if (!strcmp(var, "saturation")) s->set_saturation(s, val);
  else if (!strcmp(var, "hmirror")) {
    s->set_hmirror(s, val);
    s->set_framesize(s, s->status.framesize); // OV2640: DSP reinit picks up mirror
  } else if (!strcmp(var, "vflip")) {
    s->set_vflip(s, val);
    s->set_framesize(s, s->status.framesize); // OV2640: DSP reinit picks up flip
  }
  else if (!strcmp(var, "awb"))        s->set_whitebal(s, val);
  else if (!strcmp(var, "aec"))        s->set_exposure_ctrl(s, val);
  Serial.printf("[cam] set %s=%d\n", var, val);
}

esp_err_t index_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  httpd_resp_send(req, INDEX_HTML, strlen(INDEX_HTML));
  return ESP_OK;
}

esp_err_t stream_handler(httpd_req_t *req) {
  camera_fb_t *fb = nullptr;
  char part_buf[64];
  httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  while (true) {
    fb = esp_camera_fb_get();
    if (!fb) break;
    httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
    size_t hlen = snprintf(part_buf, sizeof(part_buf), STREAM_PART, fb->len);
    httpd_resp_send_chunk(req, part_buf, hlen);
    esp_err_t res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
    esp_camera_fb_return(fb);
    fb = nullptr;
    if (res != ESP_OK) break;
  }
  if (fb) esp_camera_fb_return(fb);
  return ESP_OK;
}
