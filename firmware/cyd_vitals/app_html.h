// app_html.h - the page served by cyd_vitals in export mode, held in flash (PROGMEM).
//
// It lives in the firmware rather than on the SD card on purpose: the whole point of export mode
// is never having to take the card out, so the page cannot depend on a file someone put there.
//
// The board only ever ships bytes. All parsing, aggregation and drawing happens in the phone's
// browser, which is why the charts can be interactive without costing the ESP32 anything. Charts
// are hand-rolled SVG for the same reason there is no CDN link anywhere in here: while a phone is
// joined to the board's access point it has no route to the internet.
#pragma once

static const char APP_HTML[] PROGMEM = R"rawliteral(<!doctype html>
<html lang="en"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Baby Vitals Report</title>
<style>
  :root{
    --bg:#fff; --fg:#16181d; --mut:#6b7280; --line:#e5e7eb; --card:#f9fafb;
    --hr:#c0324b; --ox:#2563a6; --warn:#b45309; --ok:#15803d;
  }
  *{box-sizing:border-box}
  body{margin:0;font:15px/1.5 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif;
       background:var(--bg);color:var(--fg);-webkit-text-size-adjust:100%}
  header{position:sticky;top:0;background:var(--bg);border-bottom:1px solid var(--line);
         padding:12px 14px;z-index:5}
  h1{margin:0 0 2px;font-size:17px;letter-spacing:-.01em}
  .sub{color:var(--mut);font-size:12px}
  .bar{display:flex;gap:6px;flex-wrap:wrap;margin-top:10px}
  button{font:inherit;font-size:13px;padding:7px 12px;border:1px solid var(--line);
         background:var(--card);color:var(--fg);border-radius:7px;cursor:pointer}
  button.on{background:var(--fg);color:var(--bg);border-color:var(--fg)}
  button:disabled{opacity:.45;cursor:default}
  main{padding:14px;max-width:900px;margin:0 auto}
  section{margin:0 0 22px}
  h2{font-size:13px;text-transform:uppercase;letter-spacing:.06em;color:var(--mut);
     margin:0 0 8px;font-weight:600}
  .metric-title{font-size:12px;color:var(--mut);font-weight:600;margin:10px 0 4px}
  .cards{display:grid;grid-template-columns:repeat(auto-fit,minmax(130px,1fr));gap:8px}
  .card{background:var(--card);border:1px solid var(--line);border-radius:9px;padding:10px 12px}
  .card .k{font-size:11px;color:var(--mut);text-transform:uppercase;letter-spacing:.04em}
  .card .v{font-size:21px;font-variant-numeric:tabular-nums;margin-top:2px;
           display:flex;align-items:baseline;gap:5px;flex-wrap:wrap}
  .card .u{font-size:12px;color:var(--mut);font-variant-numeric:normal}
  .wrap{overflow-x:auto;-webkit-overflow-scrolling:touch}
  svg{display:block;touch-action:pan-y}
  table{border-collapse:collapse;width:100%;font-size:13px;font-variant-numeric:tabular-nums}
  th,td{text-align:right;padding:5px 7px;border-bottom:1px solid var(--line);white-space:nowrap}
  th:first-child,td:first-child{text-align:left}
  th{color:var(--mut);font-weight:600;font-size:11px;text-transform:uppercase;letter-spacing:.04em}
  tr.sel td{background:#eef2ff}
  tfoot td{font-weight:600;border-top:2px solid var(--line);border-bottom:none}
  .secline{display:flex;align-items:baseline;justify-content:space-between;gap:8px;flex-wrap:wrap}
  .res{display:flex;gap:4px}
  .res button{padding:4px 9px;font-size:12px;border-radius:6px}
  .lo{color:var(--warn);font-weight:600}
  .note{font-size:12px;color:var(--mut);border-left:3px solid var(--line);padding:8px 12px;
        background:var(--card);border-radius:0 7px 7px 0}
  .tip{position:fixed;pointer-events:none;background:var(--fg);color:var(--bg);font-size:12px;
       padding:6px 9px;border-radius:6px;opacity:0;transition:opacity .1s;z-index:9;
       font-variant-numeric:tabular-nums;white-space:pre}
  #msg{padding:30px 14px;text-align:center;color:var(--mut)}
  .prog{height:3px;background:var(--line);border-radius:2px;overflow:hidden;margin-top:8px}
  .prog i{display:block;height:100%;background:var(--fg);width:0;transition:width .2s}
  @media print{
    header{position:static} .bar,.res,#saveBtn{display:none}
    body{font-size:11px} .card{break-inside:avoid} section{break-inside:avoid}
  }
</style>
</head><body>
<header>
  <h1>Baby Vitals Report</h1>
  <div class="sub" id="sub">Loading&hellip;</div>
  <div class="bar" id="bar"></div>
  <div class="prog" id="prog" hidden><i></i></div>
</header>
<main><div id="msg">Contacting monitor&hellip;</div><div id="app" hidden></div></main>
<div class="tip" id="tip"></div>
<script>
"use strict";
// Sampling assumptions, kept next to each other so the arithmetic below is auditable.
var SEC_PER_SAMPLE=20;              // the wristband emits a new heart rate roughly every 20 s
var EXPECTED_PER_DAY=86400/SEC_PER_SAMPLE;
var SPO2_LOW=90, HR_LOW=90, HR_HIGH=180;   // mirrors the thresholds in cyd_vitals.ino
// Bin widths offered for the single-day charts: seconds, button label, wording for the title.
// 10 s is finer than the wristband samples, so it shows the readings themselves rather than a
// summary of them.
var RES=[[10,"10s","10-second"],[60,"1m","1-minute"],[300,"5m","5-minute"],[900,"15m","15-minute"]];

var $=function(s){return document.querySelector(s)};
var days=[], sel=null, binSec=900,
    embedded=(typeof EMBEDDED_DATA!=="undefined")?EMBEDDED_DATA:null;

function fmt(n,d){ return (n===null||n===undefined||isNaN(n))?"–":n.toFixed(d===undefined?0:d) }
function pct(n){ return (n*100).toFixed(0)+"%" }
function r1(v){ return Math.round(v*10)/10 }          // keeps the SVG text small
function hhmm(sec,withSec){ var p=function(v){ return (v<10?"0":"")+v };
  return p(Math.floor(sec/3600))+":"+p(Math.floor(sec/60)%60)+(withSec?":"+p(Math.floor(sec)%60):"") }
function esc(s){ return String(s).replace(/[&<>"]/g,function(c){
  return {"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;"}[c] }) }

// ---------- data ----------
function quant(sorted,q){
  if(!sorted.length) return null;
  var i=(sorted.length-1)*q, lo=Math.floor(i), hi=Math.ceil(i);
  return lo===hi?sorted[lo]:sorted[lo]+(sorted[hi]-sorted[lo])*(i-lo);
}
// One day of CSV -> the summary the report is built from. Rows with a 0/blank value are dropped:
// the firmware writes 0 when it has no reading, and averaging those in would invent healthy data.
//
// The day keeps its readings (raw.t/h/o, one entry per row, time as seconds past midnight) rather
// than a fixed set of bins, because the single-day charts let the reader choose the bin width.
// Three integer arrays per day cost far less than they look: a full day is ~4300 samples.
function summarise(name,text){
  var hr=[],ox=[],sk=[],rt=[],rh=[],ro=[],i;
  var lines=text.split("\n"), lowOx=0, loHr=0, hiHr=0;
  for(i=0;i<lines.length;i++){
    var ln=lines[i]; if(!ln||ln[0]==="t") continue;
    var p=ln.split(",");  if(p.length<3) continue;
    var t=p[0], h=+p[1], o=+p[2], s=p[3]===""||p[3]===undefined?NaN:+p[3];
    var hh=+t.substr(11,2), mm=+t.substr(14,2), sec=+t.substr(17,2);
    if(isNaN(hh)||isNaN(mm)) continue;
    if(isNaN(sec)) sec=0;
    var sod=hh*3600+mm*60+sec; if(sod<0||sod>=86400) continue;
    rt.push(sod); rh.push(h>0?h:0); ro.push(o>0?o:0);
    // Low and high heart rate are counted apart, because the report shows them apart.
    if(h>0){ hr.push(h); if(h<HR_LOW) loHr++; else if(h>=HR_HIGH) hiHr++; }
    if(o>0){ ox.push(o); if(o<SPO2_LOW) lowOx++; }
    if(!isNaN(s)&&s>0) sk.push(s);
  }
  var sh=hr.slice().sort(function(a,b){return a-b}), so=ox.slice().sort(function(a,b){return a-b});
  var ss=sk.slice().sort(function(a,b){return a-b});
  return {
    date:name.replace(/^\/?vitals_/,"").replace(/\.csv$/,""),
    n:hr.length, cover:Math.min(1,hr.length/EXPECTED_PER_DAY),
    hr:{min:sh[0]||null,med:quant(sh,.5),max:sh[sh.length-1]||null},
    ox:{min:so[0]||null,med:quant(so,.5),max:so[so.length-1]||null},
    skin:ss.length?quant(ss,.5):null,
    lowOxMin:lowOx*SEC_PER_SAMPLE/60,
    loHrMin:loHr*SEC_PER_SAMPLE/60, hiHrMin:hiHr*SEC_PER_SAMPLE/60,
    raw:{t:rt,h:rh,o:ro}
  };
}

// One day's readings -> bins of the requested width, each with min/median/max and a count.
// Binning happens here rather than in summarise() because the width is a viewing choice, and the
// last result is cached: the two day charts and every resize ask for the same bins.
var profCache={date:null,binSec:0,bins:null};
function profile(d,binSec){
  if(!d) return [];
  if(!d.raw) return d.prof||[];        // a report saved before the resolution buttons existed
  if(profCache.date===d.date&&profCache.binSec===binSec) return profCache.bins;
  var n=Math.round(86400/binSec), bins=[], i;
  for(i=0;i<n;i++) bins.push(null);
  var t=d.raw.t, h=d.raw.h, o=d.raw.o;
  for(i=0;i<t.length;i++){
    var b=Math.floor(t[i]/binSec); if(b<0||b>=n) continue;
    if(!bins[b]) bins[b]={h:[],o:[]};
    if(h[i]>0) bins[b].h.push(h[i]);
    if(o[i]>0) bins[b].o.push(o[i]);
  }
  var f=function(a){ if(!a.length) return null;
    var s=a.slice().sort(function(x,y){return x-y});
    return {min:s[0],med:quant(s,.5),max:s[s.length-1],n:s.length} };
  for(i=0;i<n;i++){ var b2=bins[i];
    bins[i]=(!b2||(!b2.h.length&&!b2.o.length))?null:{h:f(b2.h),o:f(b2.o)} }
  profCache={date:d.date,binSec:binSec,bins:bins};
  return bins;
}
function resLabel(){ for(var i=0;i<RES.length;i++) if(RES[i][0]===binSec) return RES[i][2];
  return "15-minute" }

function setProg(f){ var p=$("#prog"); if(f===null){p.hidden=true;return} p.hidden=false;
  p.firstElementChild.style.width=(f*100).toFixed(0)+"%" }

async function load(n){
  if(embedded){ days=embedded.days; render(); return }
  $("#msg").hidden=false; $("#app").hidden=true; $("#msg").textContent="Reading index…";
  var idx;
  try{ idx=await (await fetch("/days")).json() }
  catch(e){ $("#msg").textContent="Could not reach the monitor. Is the phone still on the "+
    "BabyVitals network?"; return }
  idx.sort(function(a,b){ return a.f<b.f?1:-1 });          // newest first
  var pick=(n==="all")?idx:idx.slice(0,n);
  pick.reverse();                                          // oldest -> newest for plotting
  days=[];
  for(var i=0;i<pick.length;i++){
    $("#msg").textContent="Downloading "+(i+1)+" of "+pick.length+"…";
    setProg(i/pick.length);
    try{ var txt=await (await fetch(pick[i].f)).text(); days.push(summarise(pick[i].f,txt)) }
    catch(e){ /* skip a day we cannot read rather than abort the whole report */ }
  }
  setProg(null); render();
}

// ---------- charts ----------
var tip=null;
function showTip(x,y,txt){ var t=$("#tip"); t.textContent=txt; t.style.opacity=1;
  var w=t.offsetWidth,h=t.offsetHeight;
  t.style.left=Math.max(4,Math.min(innerWidth-w-4,x-w/2))+"px";
  t.style.top=Math.max(4,y-h-10)+"px" }
function hideTip(){ $("#tip").style.opacity=0 }

// Per-day min-max band with a median line. One column per day, tappable.
// Width comes from the caller: it fills the container, and only grows past it (turning the
// container into a horizontal scroller) once there are too many days to stay legible.
function bandChart(data,key,color,yLo,yHi,refs,w){
  var h=190, pad={l:40,r:16,t:10,b:26};
  var iw=w-pad.l-pad.r, ih=h-pad.t-pad.b;
  var Y=function(v){ return pad.t+ih-(Math.max(yLo,Math.min(yHi,v))-yLo)/(yHi-yLo)*ih };
  var X=function(i){ return pad.l+(data.length===1?iw/2:i*(iw/(data.length-1||1))) };
  if(data.length===1) X=function(){ return pad.l+iw/2 };
  var s='<svg viewBox="0 0 '+w+' '+h+'" width="'+w+'" height="'+h+'" role="img">';
  var g,i;
  for(g=0;g<=4;g++){ var v=yLo+(yHi-yLo)*g/4, y=Y(v);
    s+='<line x1="'+pad.l+'" y1="'+y+'" x2="'+(w-pad.r)+'" y2="'+y+'" stroke="#e5e7eb"/>';
    s+='<text x="'+(pad.l-6)+'" y="'+(y+4)+'" text-anchor="end" font-size="10" fill="#6b7280">'+
       Math.round(v)+'</text>' }
  for(i=0;i<refs.length;i++) s+='<line x1="'+pad.l+'" y1="'+Y(refs[i])+'" x2="'+(w-pad.r)+
    '" y2="'+Y(refs[i])+'" stroke="#b45309" stroke-dasharray="4 3" stroke-width="1"/>';
  // Bar thickness follows the spacing, so a week looks generous and a month stays readable
  // instead of merging into a solid block.
  var gap=data.length>1?iw/(data.length-1):iw;
  var bw=Math.max(3,Math.min(10,gap*0.45));
  var band="",line="";
  for(i=0;i<data.length;i++){ var d=data[i][key];
    if(!d||d.min===null) continue;
    var x=X(i);
    band+='<line x1="'+x+'" y1="'+Y(d.min)+'" x2="'+x+'" y2="'+Y(d.max)+
          '" stroke="'+color+'" stroke-opacity=".28" stroke-width="'+bw.toFixed(1)+
          '" stroke-linecap="round"/>';
    line+=(line?"L":"M")+x+" "+Y(d.med)+" ";
  }
  s+=band+'<path d="'+line+'" fill="none" stroke="'+color+'" stroke-width="2"/>';
  for(i=0;i<data.length;i++){ var d2=data[i][key]; if(!d2||d2.med===null) continue;
    s+='<circle cx="'+X(i)+'" cy="'+Y(d2.med)+'" r="3" fill="'+color+'"/>' }
  // Thin the date labels to whatever the current width can actually fit.
  var every=Math.max(1,Math.ceil(data.length/Math.max(2,Math.floor(iw/54))));
  var hitW=data.length>1?Math.max(10,iw/(data.length-1)):iw;
  var lastI=data.length-1;
  for(i=0;i<data.length;i++){
    // Every Nth day, plus the final one - but drop a regular tick that would collide with it.
    if(i===lastI || (i%every===0 && X(lastI)-X(i)>46)){
      var a=(i===0&&data.length>1)?"start":(i===lastI&&data.length>1?"end":"middle");
      s+='<text x="'+X(i)+'" y="'+(h-8)+'" text-anchor="'+a+'" font-size="9" fill="#6b7280">'+
         esc(data[i].date.slice(5))+'</text>';
    }
    s+='<rect data-i="'+i+'" x="'+(X(i)-hitW/2)+'" y="'+pad.t+'" width="'+hitW+'" height="'+ih+
       '" fill="transparent" style="cursor:pointer"/>';
  }
  return s+'</svg>';
}

// A single metric from one day, already binned by profile(), midnight to midnight.
// Wide bins are drawn as range bars, as before. Once a bin is thinner than a couple of pixels -
// which 10-second bins always are - the bars would merge into a smear, so the min-max range is
// drawn as one filled envelope instead and only the median stays a line.
function dayChart(bins,key,color,yLo,yHi,refs,w){
  var n=bins.length||96;
  var h=170, pad={l:40,r:16,t:10,b:24};
  var iw=w-pad.l-pad.r, ih=h-pad.t-pad.b;
  var Y=function(v){ return r1(pad.t+ih-(Math.max(yLo,Math.min(yHi,v))-yLo)/(yHi-yLo)*ih) };
  var step=iw/n, X=function(i){ return r1(pad.l+(i+.5)*step) };
  var s='<svg viewBox="0 0 '+w+' '+h+'" width="'+w+'" height="'+h+'">',i;
  for(i=0;i<=4;i++){ var v=yLo+(yHi-yLo)*i/4,y=Y(v);
    s+='<line x1="'+pad.l+'" y1="'+y+'" x2="'+(w-pad.r)+'" y2="'+y+'" stroke="#e5e7eb"/>'+
       '<text x="'+(pad.l-6)+'" y="'+(y+4)+'" text-anchor="end" font-size="10" fill="#6b7280">'+
       Math.round(v)+'</text>' }
  for(i=0;i<refs.length;i++) s+='<line x1="'+pad.l+'" y1="'+Y(refs[i])+'" x2="'+(w-pad.r)+
    '" y2="'+Y(refs[i])+'" stroke="#b45309" stroke-dasharray="4 3" stroke-width="1"/>';
  for(i=0;i<=24;i+=3){ var x=pad.l+i/24*iw;
    s+='<line x1="'+x+'" y1="'+pad.t+'" x2="'+x+'" y2="'+(pad.t+ih)+'" stroke="#f3f4f6"/>'+
       '<text x="'+x+'" y="'+(h-6)+'" text-anchor="middle" font-size="9" fill="#6b7280">'+i+'</text>' }
  var bars=step>=2.5, bw=Math.max(2,Math.min(6,step*.8)).toFixed(1);
  // Split into runs so a real dropout stays a gap on the chart. The break has to be measured in
  // time, not in empty bins: at 10 seconds the band only speaks every second bin, and treating
  // those as gaps would leave a chart of disconnected dots. Anything up to three sample
  // intervals - or one and a half bins, whichever is longer - is bridged; a longer silence is a
  // dropout and is drawn as one.
  var secPerBin=86400/n, gapSec=Math.max(3*SEC_PER_SAMPLE,1.5*secPerBin);
  var segs=[],cur=null,prev=-1;
  for(i=0;i<n;i++){ var m=bins[i]&&bins[i][key];
    if(!m||m.min===null||m.min===undefined) continue;
    if(!cur||(i-prev)*secPerBin>gapSec){ cur=[]; segs.push(cur) }
    cur.push({x:X(i),m:m}); prev=i }
  var band="",line="";
  for(var g=0;g<segs.length;g++){ var seg=segs[g],top="",bot="";
    for(i=0;i<seg.length;i++){ var x2=seg[i].x, m2=seg[i].m;
      if(bars) band+='<line x1="'+x2+'" y1="'+Y(m2.min)+'" x2="'+x2+'" y2="'+Y(m2.max)+
        '" stroke="'+color+'" stroke-opacity=".25" stroke-width="'+bw+'"/>';
      else top+=(i?"L":"M")+x2+" "+Y(m2.max)+" ";
      line+=(i?"L":"M")+x2+" "+Y(m2.med)+" " }
    if(!bars){
      for(i=seg.length-1;i>=0;i--) bot+="L"+seg[i].x+" "+Y(seg[i].m.min)+" ";
      // Stroked as well as filled, so a run of one bin - which encloses no area - still shows.
      band+='<path d="'+top+bot+'Z" fill="'+color+'" fill-opacity=".22" stroke="'+color+
            '" stroke-opacity=".22" stroke-width="1"/>' }
  }
  s+=band+'<path d="'+line+'" fill="none" stroke="'+color+'" stroke-width="1.6"/>';
  // Crosshair, marker and hit area for the pointer readout. The plot geometry rides along on the
  // hit rect so bindDayChart() never has to recompute any of it.
  s+='<line class="cx" x1="0" y1="'+pad.t+'" x2="0" y2="'+(pad.t+ih)+'" stroke="#6b7280" '+
     'stroke-width="1" opacity="0"/><circle class="cd" r="3.5" fill="'+color+'" opacity="0"/>'+
     '<rect class="hit" x="'+pad.l+'" y="'+pad.t+'" width="'+iw+'" height="'+ih+
     '" fill="transparent" data-l="'+pad.l+'" data-t="'+pad.t+'" data-iw="'+iw+'" data-ih="'+ih+
     '" data-n="'+n+'" data-ylo="'+yLo+'" data-yhi="'+yHi+'"/>';
  return s+'</svg>';
}

// Pointer readout for a day chart: crosshair, dot on the median, and the numbers for that bin.
function bindDayChart(el,bins,key,unit){
  var svg=el.querySelector("svg"); if(!svg) return;
  var hit=svg.querySelector("rect.hit"), cx=svg.querySelector("line.cx"),
      cd=svg.querySelector("circle.cd");
  if(!hit) return;
  var A=function(k){ return +hit.getAttribute("data-"+k) };
  var l=A("l"), t=A("t"), iw=A("iw"), ih=A("ih"), n=A("n"), yLo=A("ylo"), yHi=A("yhi");
  var vw=+svg.getAttribute("width"), step=iw/n, secPerBin=86400/n;
  var Y=function(v){ return t+ih-(Math.max(yLo,Math.min(yHi,v))-yLo)/(yHi-yLo)*ih };
  function clear(){ hideTip(); cx.setAttribute("opacity",0); cd.setAttribute("opacity",0) }
  el.addEventListener("pointermove",function(ev){
    // The SVG may be laid out narrower than its own width attribute, so map back through the
    // rendered box rather than trusting pixel-for-pixel.
    var box=svg.getBoundingClientRect(), sc=(box.width/vw)||1;
    var x=(ev.clientX-box.left)/sc;
    if(x<l-4||x>l+iw+4){ clear(); return }
    var i=Math.floor((x-l)/step);
    // Snap to the nearest filled bin within about four pixels: at fine resolutions most bins sit
    // between two readings, and an empty one under the finger should not blank the readout.
    var rad=Math.max(1,Math.ceil(4/step)), j=-1, k;
    for(k=0;k<=rad&&j<0;k++){
      if(i-k>=0&&bins[i-k]&&bins[i-k][key]) j=i-k;
      else if(i+k<n&&bins[i+k]&&bins[i+k][key]) j=i+k;
    }
    if(j<0){ clear(); return }
    var m=bins[j][key], bx=l+(j+.5)*step, by=Y(m.med);
    cx.setAttribute("x1",bx); cx.setAttribute("x2",bx); cx.setAttribute("opacity",".35");
    cd.setAttribute("cx",bx); cd.setAttribute("cy",by); cd.setAttribute("opacity","1");
    showTip(ev.clientX, box.top+by*sc,
      hhmm(j*secPerBin,secPerBin<60)+"\nmed "+fmt(m.med)+unit+
      "\n"+fmt(m.min)+"–"+fmt(m.max)+unit+
      (m.n?"\n"+m.n+" reading"+(m.n>1?"s":""):""));
  });
  el.addEventListener("pointerleave",clear);
  el.addEventListener("pointercancel",clear);
}

function bindChart(el,data,key,unit){
  function at(ev){
    var t=ev.target.getAttribute&&ev.target.getAttribute("data-i");
    if(t===null||t===undefined) return null;
    return +t;
  }
  el.addEventListener("pointermove",function(ev){
    var i=at(ev); if(i===null){hideTip();return}
    var d=data[i], v=d[key];
    if(!v||v.min===null){hideTip();return}
    showTip(ev.clientX,ev.clientY,d.date+"\nmed "+fmt(v.med)+unit+"   "+fmt(v.min)+"–"+
      fmt(v.max)+unit+"\ncoverage "+pct(d.cover));
  });
  el.addEventListener("pointerleave",hideTip);
  el.addEventListener("click",function(ev){
    var i=at(ev); if(i===null) return;
    sel=data[i].date; render();
    var dd=document.getElementById("daydetail"); if(dd) dd.scrollIntoView({behavior:"smooth",block:"center"});
  });
}

// ---------- render ----------
function render(){
  $("#msg").hidden=true; $("#app").hidden=false;
  if(!days.length){ $("#msg").hidden=false; $("#msg").textContent="No log files on the card yet."; return }
  var totN=0,totLowOx=0,totLoHr=0,totHiHr=0,allHr=[],allOx=[],allSk=[],i;
  var minHr=null,maxHr=null,minOx=null;
  for(i=0;i<days.length;i++){ var d=days[i]; totN+=d.n; totLowOx+=d.lowOxMin;
    totLoHr+=d.loHrMin||0; totHiHr+=d.hiHrMin||0;
    if(d.hr.med!==null) allHr.push(d.hr.med); if(d.ox.med!==null) allOx.push(d.ox.med);
    if(d.skin!==null&&d.skin!==undefined) allSk.push(d.skin);
    if(d.hr.min!==null&&(minHr===null||d.hr.min<minHr)) minHr=d.hr.min;
    if(d.hr.max!==null&&(maxHr===null||d.hr.max>maxHr)) maxHr=d.hr.max;
    if(d.ox.min!==null&&(minOx===null||d.ox.min<minOx)) minOx=d.ox.min }
  var cover=totN/(days.length*EXPECTED_PER_DAY);
  var srt=function(a){ return a.slice().sort(function(x,y){return x-y}) };
  var mhr=quant(srt(allHr),.5), mox=quant(srt(allOx),.5), msk=quant(srt(allSk),.5);
  var perDay=function(v){ return v/days.length };
  $("#sub").textContent=days[0].date+" → "+days[days.length-1].date+
    "  ·  "+days.length+" day"+(days.length>1?"s":"")+"  ·  "+
    totN.toLocaleString()+" readings";

  var html="";
  html+='<section><h2>Summary</h2><div class="cards">'+
    card("Median heart rate",fmt(mhr),"bpm")+
    card("Median SpO₂",fmt(mox)+"%","")+       // percent sits tight against the number
    card("Data coverage",pct(cover),"of the period")+
    card("SpO₂ below "+SPO2_LOW+"%",fmt(totLowOx),"min total")+
    card("HR below "+HR_LOW,fmt(perDay(totLoHr),1),"min/day avg")+
    card("HR above "+(HR_HIGH-1),fmt(perDay(totHiHr),1),"min/day avg")+
    '</div></section>';

  html+='<section><h2>Heart rate by day — median, with daily range</h2>'+
    '<div class="wrap" id="c1"></div></section>';
  html+='<section><h2>Oxygen saturation by day — median, with daily range</h2>'+
    '<div class="wrap" id="c2"></div></section>';

  html+='<section id="daydetail"><div class="secline"><h2>Single day'+
    (sel?" — "+esc(sel):"")+'</h2>'+(sel?'<div class="res" id="res"></div>':'')+'</div>'+
    (sel?'<div class="metric-title">Heart rate — '+resLabel()+' median and range</div>'+
     '<div class="wrap" id="c3"></div>'+
     '<div class="metric-title">Oxygen saturation — '+resLabel()+' median and range</div>'+
     '<div class="wrap" id="c4"></div>':'<div class="note">Tap any point above to see that '+
     'day hour by hour.</div>')+'</section>';

  html+='<section><h2>Daily detail</h2><div class="wrap"><table><thead><tr>'+
    '<th>Date</th><th>Cover</th><th>HR med</th><th>HR min–max</th>'+
    '<th>HR &lt;'+HR_LOW+'</th><th>HR &gt;'+(HR_HIGH-1)+'</th>'+
    '<th>SpO₂ med</th><th>SpO₂ min</th><th>Low SpO₂</th><th>Skin</th></tr></thead><tbody>';
  for(i=days.length-1;i>=0;i--){ var r=days[i];
    html+='<tr class="'+(sel===r.date?"sel":"")+'"><td>'+esc(r.date)+'</td>'+
      '<td'+(r.cover<.5?' class="lo"':'')+'>'+pct(r.cover)+'</td>'+
      '<td>'+fmt(r.hr.med)+'</td><td>'+fmt(r.hr.min)+'–'+fmt(r.hr.max)+'</td>'+
      '<td'+(r.loHrMin?' class="lo"':'')+'>'+fmt(r.loHrMin)+' min</td>'+
      '<td'+(r.hiHrMin?' class="lo"':'')+'>'+fmt(r.hiHrMin)+' min</td>'+
      '<td>'+fmt(r.ox.med)+'</td><td'+(r.ox.min!==null&&r.ox.min<SPO2_LOW?' class="lo"':'')+'>'+
      fmt(r.ox.min)+'</td><td>'+fmt(r.lowOxMin)+' min</td>'+
      '<td>'+(r.skin===null?"–":fmt(r.skin,1)+"°")+'</td></tr>';
  }
  // Footer row: the per-day averages, plus the medians and extremes for the whole period, so the
  // two heart-rate columns can be read as "a typical day" without adding them up by hand.
  html+='</tbody><tfoot><tr><td>Average / day</td><td>'+pct(cover)+'</td>'+
    '<td>'+fmt(mhr)+'</td><td>'+fmt(minHr)+'–'+fmt(maxHr)+'</td>'+
    '<td>'+fmt(perDay(totLoHr),1)+' min</td><td>'+fmt(perDay(totHiHr),1)+' min</td>'+
    '<td>'+fmt(mox)+'</td><td>'+fmt(minOx)+'</td>'+
    '<td>'+fmt(perDay(totLowOx),1)+' min</td>'+
    '<td>'+(msk===null?"–":fmt(msk,1)+"°")+'</td></tr></tfoot>';
  html+='</table></div></section>';

  html+='<section><div class="note"><strong>How to read this.</strong> These figures come from a '+
    'home-built receiver listening to a consumer baby monitor, not a medical device. The alert '+
    'thresholds are arbitrary. A wrist-worn sensor drops out when the baby moves, so low readings '+
    'are often motion artefacts rather than real events — always check the coverage column '+
    'before reading anything into a day. Useful as trend context; not a measurement.</div></section>';

  $("#app").innerHTML=html;
  buildRes();
  drawCharts();
}
// Bin-width picker, sitting at the top right of the single-day section. Re-rendering is cheap
// and keeps the chart titles honest about what is being shown.
function buildRes(){
  var r=document.getElementById("res"); if(!r) return;
  r.innerHTML="";
  RES.forEach(function(o){
    var b=document.createElement("button"); b.textContent=o[1];
    if(binSec===o[0]) b.classList.add("on");
    b.onclick=function(){ if(binSec===o[0]) return; binSec=o[0]; render();
      var dd=document.getElementById("daydetail");
      if(dd) dd.scrollIntoView({behavior:"instant",block:"start"}) };
    r.appendChild(b);
  });
}
// Kept separate from render() so a rotate or window resize only recomputes geometry.
function drawCharts(){
  var c1=document.getElementById("c1"); if(!c1||!days.length) return;
  var avail=Math.max(280,c1.clientWidth);
  // Fill the container and fit every day inside it. Only past ~100 days, where columns would be
  // narrower than a finger, does it grow wider and let the container scroll.
  var w=Math.max(avail,days.length*9);
  c1.innerHTML=bandChart(days,"hr","#c0324b",40,220,[HR_LOW,HR_HIGH],w);
  bindChart(c1,days,"hr"," bpm");
  var c2=document.getElementById("c2");
  c2.innerHTML=bandChart(days,"ox","#2563a6",80,100,[SPO2_LOW],w);
  bindChart(c2,days,"ox","%");
  var c3=document.getElementById("c3"), c4=document.getElementById("c4"), selectedDay=null;
  if((c3||c4)&&sel) for(var i=0;i<days.length;i++)
    if(days[i].date===sel){ selectedDay=days[i]; break }
  var dayWidth=Math.max(avail,460);
  if(selectedDay){
    var bins=profile(selectedDay,binSec);
    if(c3){ c3.innerHTML=dayChart(bins,"h","#c0324b",40,200,[HR_LOW,HR_HIGH],dayWidth);
            bindDayChart(c3,bins,"h"," bpm") }
    if(c4){ c4.innerHTML=dayChart(bins,"o","#2563a6",80,100,[SPO2_LOW],dayWidth);
            bindDayChart(c4,bins,"o","%") }
  }
}
var rzT=null;
window.addEventListener("resize",function(){ clearTimeout(rzT); rzT=setTimeout(drawCharts,150) });
function card(k,v,u){ return '<div class="card"><div class="k">'+k+'</div><div class="v">'+v+
  (u?'<span class="u">'+u+'</span>':'')+'</div></div>' }

// ---------- controls ----------
function buildBar(){
  var b=$("#bar"); if(embedded){ b.innerHTML='<span class="sub">saved report</span>'; return }
  var opts=[[7,"Last 7 days"],[14,"Last 14 days"],[30,"Last 30 days"],["all","Everything"]];
  b.innerHTML="";
  opts.forEach(function(o){
    var el=document.createElement("button"); el.textContent=o[1];
    el.onclick=function(){ [].forEach.call(b.children,function(c){c.classList.remove("on")});
      el.classList.add("on"); load(o[0]) };
    b.appendChild(el);
  });
  var save=document.createElement("button"); save.id="saveBtn"; save.textContent="Save to phone";
  save.onclick=saveReport; b.appendChild(save);
  b.children[0].classList.add("on");
}
// Bake the loaded data into a copy of this page so it opens with no board and no network - the
// version you actually show at an appointment.
function saveReport(){
  var doc=document.documentElement.outerHTML
    .replace(/<script>/, '<script>var EMBEDDED_DATA='+JSON.stringify({days:days})+';<\/script><script>');
  var blob=new Blob(["<!doctype html>\n"+doc],{type:"text/html"});
  var a=document.createElement("a");
  a.href=URL.createObjectURL(blob);
  a.download="baby-vitals_"+days[0].date+"_to_"+days[days.length-1].date+".html";
  document.body.appendChild(a); a.click(); a.remove();
  setTimeout(function(){ URL.revokeObjectURL(a.href) },4000);
}

buildBar();
if(embedded){ days=embedded.days; render() } else { load(7) }
</script>
</body></html>)rawliteral";
