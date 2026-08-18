#include "flowx_browser_assets.h"

namespace flowx {

std::string_view browserHtml() {
    static constexpr char kHtml[] = R"FLOWXHTML(<!doctype html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>FlowX browser decoder</title>
<style>
body{margin:0;background:#111;color:#ddd;font:14px system-ui,sans-serif}header{padding:10px 14px;background:#1b1b1b;display:flex;gap:18px;flex-wrap:wrap}canvas{display:block;max-width:100vw;max-height:calc(100vh - 70px);margin:auto;background:#000}.ok{color:#7ee787}.bad{color:#ff7b72}code{color:#9ecbff}</style>
</head>
<body>
<header>
  <strong>FlowX browser decoder</strong>
  <span id="state">connecting…</span>
  <span>stream <code id="stream">-</code></span>
  <span>frame <code id="frame">-</code></span>
  <span>records <code id="records">0</code></span>
  <span>packets <code id="packets">0</code></span>
  <span>keyframes <code id="keys">0</code></span>
  <span>patches seen <code id="patches">0</code></span>
  <span>errors <code id="errors">0</code></span>
</header>
<canvas id="view"></canvas>
<script src="/flowx.js"></script>
</body>
</html>)FLOWXHTML";
    return kHtml;
}

std::string_view browserJs() {
    static constexpr char kJs[] = R"FLOWXJS((() => {
'use strict';
const $ = id => document.getElementById(id);
const canvas = $('view');
const ctx = canvas.getContext('2d', {alpha:false});
const stats = {records:0, packets:0, keys:0, patches:0, errors:0};
const setState = (text, ok=true) => { $('state').textContent=text; $('state').className=ok?'ok':'bad'; };
const putStats = () => { for (const k of ['records','packets','keys','patches','errors']) $(k).textContent=stats[k]; };
const u16 = (v,o) => v.getUint16(o,true), u32 = (v,o) => v.getUint32(o,true);
const ascii4 = (u,o) => String.fromCharCode(u[o],u[o+1],u[o+2],u[o+3]);
let streamId = 0;
let key = null;

function resetKey(frameId, width, height, kind, layerCount=1) {
  key = {frameId,width,height,kind,layerCount,layers:new Map()};
}
function getLayer(index, bytes, count, jw, jh) {
  let l=key.layers.get(index);
  if(!l){ l={bytes:new Uint8Array(bytes), got:new Uint8Array(count), gotCount:0, count,jw,jh}; key.layers.set(index,l); }
  return l;
}
function addChunk(layer, index, offset, payload) {
  if(index>=layer.count || layer.got[index]) return;
  if(offset+payload.length>layer.bytes.length) throw new Error('chunk outside layer');
  layer.bytes.set(payload,offset); layer.got[index]=1; layer.gotCount++;
}
function complete(l){ return l && l.gotCount===l.count; }
async function bitmap(bytes) {
  const blob=new Blob([bytes],{type:'image/jpeg'});
  return await createImageBitmap(blob);
}
async function showClassic() {
  const l=key.layers.get(0); if(!complete(l)) return;
  const b=await bitmap(l.bytes);
  canvas.width=key.width; canvas.height=key.height;
  ctx.drawImage(b,0,0,key.width,key.height); b.close();
  stats.keys++; $('frame').textContent=key.frameId; putStats(); key=null;
}
async function showStrips() {
  const a=key.layers.get(0), b=key.layers.get(1); if(!complete(a)||!complete(b)) return;
  const [ia,ib]=await Promise.all([bitmap(a.bytes),bitmap(b.bytes)]);
  const w=ia.width*2,h=ia.height;
  const off=document.createElement('canvas'); off.width=w; off.height=h;
  const oc=off.getContext('2d',{alpha:false});
  for(let x=0;x<ia.width;x++) {
    oc.drawImage(ia,x,0,1,ia.height,2*x,0,1,h);
    oc.drawImage(ib,x,0,1,ib.height,2*x+1,0,1,h);
  }
  canvas.width=key.width; canvas.height=key.height;
  ctx.drawImage(off,0,0,key.width,key.height); ia.close(); ib.close();
  stats.keys++; $('frame').textContent=key.frameId; putStats(); key=null;
}
async function processCodec(codec) {
  const u=new Uint8Array(codec.buffer,codec.byteOffset,codec.byteLength), v=new DataView(codec.buffer,codec.byteOffset,codec.byteLength);
  if(codec.byteLength<20 || ascii4(u,0)!=='AFC1' || u[4]!==2) throw new Error('bad AFC1 packet');
  const type=u[5], hb=u16(v,6), frame=u32(v,8), kf=u32(v,12), ow=u16(v,16), oh=u16(v,18);
  if(type===2){ stats.patches++; return; }
  if(type===1) {
    if(hb!==40 || codec.byteLength<40) throw new Error('bad classic key header');
    if(!key || key.frameId!==frame || key.kind!=='classic') resetKey(frame,ow,oh,'classic');
    const jw=u16(v,20), jh=u16(v,22), ci=u16(v,24), cc=u16(v,26), total=u32(v,28), off=u32(v,32), n=u16(v,36);
    if(hb+n!==codec.byteLength) throw new Error('bad classic chunk length');
    addChunk(getLayer(0,total,cc,jw,jh),ci,off,u.slice(hb)); await showClassic(); return;
  }
  if(type===3) {
    if(hb!==44 || codec.byteLength<44) throw new Error('bad layered key header');
    const li=u[20], lc=u[21];
    if(lc!==2) return; // PR #44 deliberately supports STRIPS only; MOSAIC is not needed here.
    if(!key || key.frameId!==frame || key.kind!=='strips') resetKey(frame,ow,oh,'strips',lc);
    const jw=u16(v,24), jh=u16(v,26), ci=u16(v,28), cc=u16(v,30), total=u32(v,32), off=u32(v,36), n=u16(v,40);
    if(hb+n!==codec.byteLength) throw new Error('bad layered chunk length');
    addChunk(getLayer(li,total,cc,jw,jh),ci,off,u.slice(hb)); await showStrips(); return;
  }
  if(type===4) return;
  if(frame!==kf) return;
}
async function processDatagram(d) {
  const u=new Uint8Array(d.buffer,d.byteOffset,d.byteLength), v=new DataView(d.buffer,d.byteOffset,d.byteLength);
  if(d.byteLength<32 || ascii4(u,0)!=='FXV3' || u[4]!==3 || u16(v,6)!==32) throw new Error('bad FXV3 packet');
  const sid=u32(v,8), payload=u16(v,28);
  if(32+payload!==d.byteLength) throw new Error('bad FXV3 payload length');
  if(streamId!==sid){ streamId=sid; key=null; $('stream').textContent=sid; }
  stats.packets++; await processCodec(d.slice(32));
}
async function processRecord(r) {
  const u=new Uint8Array(r.buffer,r.byteOffset,r.byteLength), v=new DataView(r.buffer,r.byteOffset,r.byteLength);
  if(r.byteLength<28 || ascii4(u,0)!=='FXB1' || u16(v,4)!==1 || u16(v,6)!==28) throw new Error('bad FXB1 record');
  const recordBytes=u32(v,8), count=u16(v,24); if(recordBytes!==r.byteLength) throw new Error('bad record size');
  let p=28;
  for(let i=0;i<count;i++){ if(p+2>r.length) throw new Error('truncated packet length'); const n=u16(v,p); p+=2; if(p+n>r.length) throw new Error('truncated packet'); await processDatagram(r.slice(p,p+n)); p+=n; }
  if(p!==r.length) throw new Error('record trailing bytes'); stats.records++; putStats();
}
function concat(a,b){ const c=new Uint8Array(a.length+b.length); c.set(a); c.set(b,a.length); return c; }
async function run() {
  setState('connecting…');
  const res=await fetch('/flowx.bin',{cache:'no-store'}); if(!res.ok||!res.body) throw new Error('HTTP '+res.status);
  setState('connected'); const rd=res.body.getReader(); let buf=new Uint8Array(0);
  while(true){ const {value,done}=await rd.read(); if(done) throw new Error('stream ended'); buf=concat(buf,value); while(buf.length>=12){ const v=new DataView(buf.buffer,buf.byteOffset,buf.byteLength); if(ascii4(buf,0)!=='FXB1'){ buf=buf.slice(1); stats.errors++; putStats(); continue; } const n=u32(v,8); if(n<28||n>16*1024*1024){ buf=buf.slice(4); stats.errors++; putStats(); continue; } if(buf.length<n) break; const rec=buf.slice(0,n); buf=buf.slice(n); try{ await processRecord(rec); }catch(e){ stats.errors++; putStats(); console.error(e); } } }
}
async function reconnectLoop(){ for(;;){ try{ await run(); }catch(e){ setState(String(e),false); stats.errors++; putStats(); await new Promise(r=>setTimeout(r,1000)); } } }
reconnectLoop();
})();)FLOWXJS";
    return kJs;
}

} // namespace flowx
