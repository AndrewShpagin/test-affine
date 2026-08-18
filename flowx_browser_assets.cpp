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
body{margin:0;background:#111;color:#ddd;font:14px system-ui,sans-serif}header{padding:10px 14px;background:#1b1b1b;display:flex;gap:16px;flex-wrap:wrap}canvas{display:block;max-width:100vw;max-height:calc(100vh - 76px);margin:auto;background:#000}.ok{color:#7ee787}.bad{color:#ff7b72}code{color:#9ecbff}
</style>
</head>
<body>
<header>
  <strong>FlowX browser decoder (WebGL2)</strong>
  <span id="state">connecting…</span>
  <span>stream <code id="stream">-</code></span>
  <span>frame <code id="frame">-</code></span>
  <span>records <code id="records">0</code></span>
  <span>packets <code id="packets">0</code></span>
  <span>keyframes <code id="keys">0</code></span>
  <span>patches <code id="patches">0</code></span>
  <span>renders <code id="renders">0</code></span>
  <span>skipped <code id="skipped">0</code></span>
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
const gl = canvas.getContext('webgl2', {alpha:false, antialias:false});
const stats = {records:0, packets:0, keys:0, patches:0, renders:0, skipped:0, errors:0};
const setState = (text, ok=true) => { $('state').textContent=text; $('state').className=ok?'ok':'bad'; };
const putStats = () => { for (const k of Object.keys(stats)) { const e=$(k); if(e) e.textContent=stats[k]; } };
const u16 = (v,o) => v.getUint16(o,true), u32 = (v,o) => v.getUint32(o,true), f32=(v,o)=>v.getFloat32(o,true);
const ascii4 = (u,o) => String.fromCharCode(u[o],u[o+1],u[o+2],u[o+3]);
if(!gl){ setState('WebGL2 unavailable', false); return; }

const vs = `#version 300 es
void main(){
  vec2 p = gl_VertexID==0 ? vec2(-1.0,-1.0) : (gl_VertexID==1 ? vec2(3.0,-1.0) : vec2(-1.0,3.0));
  gl_Position=vec4(p,0.0,1.0);
}`;
const keyFs = `#version 300 es
precision highp float;
uniform sampler2D uKey;
uniform vec2 uOutSize;
uniform vec2 uKeySize;
out vec4 color;
void main(){
  vec2 dst=vec2(gl_FragCoord.x-0.5,uOutSize.y-gl_FragCoord.y-0.5);
  vec2 scale=(uKeySize-vec2(1.0))/max(uOutSize-vec2(1.0),vec2(1.0));
  vec2 s=dst*scale;
  vec2 uv=vec2((s.x+0.5)/uKeySize.x,1.0-(s.y+0.5)/uKeySize.y);
  color=texture(uKey,uv);
}`;
const copyFs = `#version 300 es
precision highp float;
uniform sampler2D uTex;
uniform vec2 uSize;
out vec4 color;
void main(){ color=texture(uTex,gl_FragCoord.xy/uSize); }
`;
const patchFs = `#version 300 es
precision highp float;
uniform sampler2D uKey;
uniform sampler2D uPrev;
uniform vec2 uOutSize;
uniform vec2 uKeySize;
uniform mat3 uInvH;
uniform int uGridX;
uniform int uGridY;
uniform vec2 uMesh[64];
out vec4 color;
float cw(float x){
  const float A=-0.75;
  x=abs(x);
  if(x<=1.0) return (A+2.0)*x*x*x-(A+3.0)*x*x+1.0;
  if(x<2.0) return A*x*x*x-5.0*A*x*x+8.0*A*x-4.0*A;
  return 0.0;
}
vec2 mf(int x,int y){ x=clamp(x,0,uGridX-1); y=clamp(y,0,uGridY-1); return uMesh[y*8+x]; }
vec2 meshAt(vec2 p){
  if(uGridX<2||uGridY<2) return vec2(0.0);
  vec2 q=(p+vec2(0.5))*vec2(float(uGridX),float(uGridY))/uOutSize-vec2(0.5);
  ivec2 b=ivec2(floor(q)); vec2 t=q-vec2(b); vec2 s=vec2(0.0);
  for(int j=-1;j<=2;++j) for(int i=-1;i<=2;++i)
    s += mf(b.x+i,b.y+j)*cw(t.x-float(i))*cw(t.y-float(j));
  return s;
}
void main(){
  vec2 dst=vec2(gl_FragCoord.x-0.5,uOutSize.y-gl_FragCoord.y-0.5);
  vec2 t=dst-meshAt(dst);
  vec3 q=uInvH*vec3(t,1.0);
  bool ok=abs(q.z)>1e-8;
  vec2 src=ok ? q.xy/q.z : vec2(-1.0);
  ok = ok && src.x>=0.0 && src.y>=0.0 && src.x<uOutSize.x-1.0 && src.y<uOutSize.y-1.0;
  if(ok){
    vec2 scale=(uKeySize-vec2(1.0))/max(uOutSize-vec2(1.0),vec2(1.0));
    vec2 s=src*scale;
    vec2 uv=vec2((s.x+0.5)/uKeySize.x,1.0-(s.y+0.5)/uKeySize.y);
    color=texture(uKey,uv);
  }else{
    vec2 uv=vec2((dst.x+0.5)/uOutSize.x,1.0-(dst.y+0.5)/uOutSize.y);
    color=texture(uPrev,uv);
  }
}`;
function sh(type,src){ const s=gl.createShader(type); gl.shaderSource(s,src); gl.compileShader(s); if(!gl.getShaderParameter(s,gl.COMPILE_STATUS)) throw new Error(gl.getShaderInfoLog(s)); return s; }
function prog(fs){ const p=gl.createProgram(); gl.attachShader(p,sh(gl.VERTEX_SHADER,vs)); gl.attachShader(p,sh(gl.FRAGMENT_SHADER,fs)); gl.linkProgram(p); if(!gl.getProgramParameter(p,gl.LINK_STATUS)) throw new Error(gl.getProgramInfoLog(p)); return p; }
const keyProg=prog(keyFs), copyProg=prog(copyFs), patchProg=prog(patchFs);
const vao=gl.createVertexArray(); gl.bindVertexArray(vao);
function tex(){ const t=gl.createTexture(); gl.bindTexture(gl.TEXTURE_2D,t); gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MIN_FILTER,gl.LINEAR); gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_MAG_FILTER,gl.LINEAR); gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_WRAP_S,gl.CLAMP_TO_EDGE); gl.texParameteri(gl.TEXTURE_2D,gl.TEXTURE_WRAP_T,gl.CLAMP_TO_EDGE); return t; }
const keyTex=tex(), frameTex=[tex(),tex()], fbo=[gl.createFramebuffer(),gl.createFramebuffer()];
let outW=0,outH=0,keyW=0,keyH=0,current=0,keyFrameId=null,streamId=0,key=null;
function alloc(w,h){
  if(outW===w&&outH===h) return;
  outW=w; outH=h; canvas.width=w; canvas.height=h;
  for(let i=0;i<2;i++){ gl.bindTexture(gl.TEXTURE_2D,frameTex[i]); gl.texImage2D(gl.TEXTURE_2D,0,gl.RGBA8,w,h,0,gl.RGBA,gl.UNSIGNED_BYTE,null); gl.bindFramebuffer(gl.FRAMEBUFFER,fbo[i]); gl.framebufferTexture2D(gl.FRAMEBUFFER,gl.COLOR_ATTACHMENT0,gl.TEXTURE_2D,frameTex[i],0); if(gl.checkFramebufferStatus(gl.FRAMEBUFFER)!==gl.FRAMEBUFFER_COMPLETE) throw new Error('framebuffer incomplete'); }
  gl.bindFramebuffer(gl.FRAMEBUFFER,null); gl.viewport(0,0,w,h);
}
function draw(){ gl.drawArrays(gl.TRIANGLES,0,3); }
function display(){ gl.bindFramebuffer(gl.FRAMEBUFFER,null); gl.viewport(0,0,outW,outH); gl.useProgram(copyProg); gl.activeTexture(gl.TEXTURE0); gl.bindTexture(gl.TEXTURE_2D,frameTex[current]); gl.uniform1i(gl.getUniformLocation(copyProg,'uTex'),0); gl.uniform2f(gl.getUniformLocation(copyProg,'uSize'),outW,outH); draw(); }
function uploadKey(source,ow,oh,frameId){
  alloc(ow,oh); keyW=source.width; keyH=source.height;
  gl.activeTexture(gl.TEXTURE0); gl.bindTexture(gl.TEXTURE_2D,keyTex); gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL,true); gl.texImage2D(gl.TEXTURE_2D,0,gl.RGBA,gl.RGBA,gl.UNSIGNED_BYTE,source); gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL,false);
  current=0; gl.bindFramebuffer(gl.FRAMEBUFFER,fbo[current]); gl.viewport(0,0,outW,outH); gl.useProgram(keyProg); gl.activeTexture(gl.TEXTURE0); gl.bindTexture(gl.TEXTURE_2D,keyTex); gl.uniform1i(gl.getUniformLocation(keyProg,'uKey'),0); gl.uniform2f(gl.getUniformLocation(keyProg,'uOutSize'),outW,outH); gl.uniform2f(gl.getUniformLocation(keyProg,'uKeySize'),keyW,keyH); draw(); display();
  keyFrameId=frameId; stats.keys++; stats.renders++; $('frame').textContent=frameId; putStats();
}
function invH(a,p){
  const m00=a[0],m01=a[1],m02=a[2],m10=a[3],m11=a[4],m12=a[5],m20=p[0],m21=p[1],m22=1;
  const c00=m11*m22-m12*m21, c01=m02*m21-m01*m22, c02=m01*m12-m02*m11;
  const c10=m12*m20-m10*m22, c11=m00*m22-m02*m20, c12=m02*m10-m00*m12;
  const c20=m10*m21-m11*m20, c21=m01*m20-m00*m21, c22=m00*m11-m01*m10;
  const d=m00*c00+m01*c10+m02*c20; if(Math.abs(d)<1e-12) return null; const k=1/d;
  const r=[c00*k,c01*k,c02*k,c10*k,c11*k,c12*k,c20*k,c21*k,c22*k];
  return new Float32Array([r[0],r[3],r[6],r[1],r[4],r[7],r[2],r[5],r[8]]);
}
function renderPatch(p){
  if(keyFrameId===null||p.keyframeId!==keyFrameId||p.width!==outW||p.height!==outH){ stats.skipped++; putStats(); return; }
  const inv=invH(p.affine,p.perspective); if(!inv){ stats.skipped++; putStats(); return; }
  const next=1-current; gl.bindFramebuffer(gl.FRAMEBUFFER,fbo[next]); gl.viewport(0,0,outW,outH); gl.useProgram(patchProg);
  gl.activeTexture(gl.TEXTURE0); gl.bindTexture(gl.TEXTURE_2D,keyTex); gl.uniform1i(gl.getUniformLocation(patchProg,'uKey'),0);
  gl.activeTexture(gl.TEXTURE1); gl.bindTexture(gl.TEXTURE_2D,frameTex[current]); gl.uniform1i(gl.getUniformLocation(patchProg,'uPrev'),1);
  gl.uniform2f(gl.getUniformLocation(patchProg,'uOutSize'),outW,outH); gl.uniform2f(gl.getUniformLocation(patchProg,'uKeySize'),keyW,keyH); gl.uniformMatrix3fv(gl.getUniformLocation(patchProg,'uInvH'),false,inv); gl.uniform1i(gl.getUniformLocation(patchProg,'uGridX'),p.gridX); gl.uniform1i(gl.getUniformLocation(patchProg,'uGridY'),p.gridY); gl.uniform2fv(gl.getUniformLocation(patchProg,'uMesh[0]'),p.mesh); draw(); current=next; display();
  stats.renders++; $('frame').textContent=p.frameId; putStats();
}
function parsePatch(u,v,hb,frame,kf,ow,oh){
  if(hb!==u.length||hb<48) throw new Error('bad patch size');
  const gx=u[20],gy=u[21],flags=u16(v,22); if(gx<1||gy<1||gx>8||gy>8||(flags&~1)) throw new Error('bad patch grid/flags');
  let pos=24; const a=[]; for(let i=0;i<6;i++,pos+=4) a.push(f32(v,pos)); const persp=(flags&1)?[f32(v,pos),f32(v,pos+4)]:[0,0]; if(flags&1) pos+=8;
  const need=pos+gx*gy*8; if(need!==hb) throw new Error('bad patch mesh size');
  const mesh=new Float32Array(128); for(let y=0;y<gy;y++) for(let x=0;x<gx;x++){ const q=(y*8+x)*2; mesh[q]=f32(v,pos); mesh[q+1]=f32(v,pos+4); pos+=8; }
  return {frameId:frame,keyframeId:kf,width:ow,height:oh,gridX:gx,gridY:gy,affine:a,perspective:persp,mesh};
}
function resetKey(frameId,width,height,kind,layerCount=1){ key={frameId,width,height,kind,layerCount,layers:new Map()}; }
function getLayer(index,bytes,count,jw,jh){ let l=key.layers.get(index); if(!l){ l={bytes:new Uint8Array(bytes),got:new Uint8Array(count),gotCount:0,count,jw,jh}; key.layers.set(index,l); } return l; }
function addChunk(layer,index,offset,payload){ if(index>=layer.count||layer.got[index]) return; if(offset+payload.length>layer.bytes.length) throw new Error('chunk outside layer'); layer.bytes.set(payload,offset); layer.got[index]=1; layer.gotCount++; }
function complete(l){ return l&&l.gotCount===l.count; }
async function bitmap(bytes){ return await createImageBitmap(new Blob([bytes],{type:'image/jpeg'})); }
async function showClassic(){ const l=key.layers.get(0); if(!complete(l)) return; const k=key,b=await bitmap(l.bytes); uploadKey(b,k.width,k.height,k.frameId); b.close(); key=null; }
async function showStrips(){ const a=key.layers.get(0),b=key.layers.get(1); if(!complete(a)||!complete(b)) return; const k=key,[ia,ib]=await Promise.all([bitmap(a.bytes),bitmap(b.bytes)]); if(ia.width!==ib.width||ia.height!==ib.height) throw new Error('strip size mismatch'); const off=document.createElement('canvas'); off.width=ia.width*2; off.height=ia.height; const oc=off.getContext('2d',{alpha:false}); for(let x=0;x<ia.width;x++){ oc.drawImage(ia,x,0,1,ia.height,2*x,0,1,ia.height); oc.drawImage(ib,x,0,1,ib.height,2*x+1,0,1,ib.height); } uploadKey(off,k.width,k.height,k.frameId); ia.close(); ib.close(); key=null; }
async function processCodec(codec){
  const u=new Uint8Array(codec.buffer,codec.byteOffset,codec.byteLength),v=new DataView(codec.buffer,codec.byteOffset,codec.byteLength); if(codec.byteLength<20||ascii4(u,0)!=='AFC1'||u[4]!==2) throw new Error('bad AFC1 packet');
  const type=u[5],hb=u16(v,6),frame=u32(v,8),kf=u32(v,12),ow=u16(v,16),oh=u16(v,18);
  if(type===2){ stats.patches++; renderPatch(parsePatch(u,v,hb,frame,kf,ow,oh)); return; }
  if(type===1){ if(hb!==40||codec.byteLength<40) throw new Error('bad classic key header'); if(!key||key.frameId!==frame||key.kind!=='classic') resetKey(frame,ow,oh,'classic'); const jw=u16(v,20),jh=u16(v,22),ci=u16(v,24),cc=u16(v,26),total=u32(v,28),off=u32(v,32),n=u16(v,36); if(hb+n!==codec.byteLength) throw new Error('bad classic chunk length'); addChunk(getLayer(0,total,cc,jw,jh),ci,off,u.slice(hb)); await showClassic(); return; }
  if(type===3){ if(hb!==44||codec.byteLength<44) throw new Error('bad layered key header'); const li=u[20],lc=u[21]; if(lc!==2) return; if(!key||key.frameId!==frame||key.kind!=='strips') resetKey(frame,ow,oh,'strips',lc); const jw=u16(v,24),jh=u16(v,26),ci=u16(v,28),cc=u16(v,30),total=u32(v,32),off=u32(v,36),n=u16(v,40); if(hb+n!==codec.byteLength) throw new Error('bad layered chunk length'); addChunk(getLayer(li,total,cc,jw,jh),ci,off,u.slice(hb)); await showStrips(); return; }
}
async function processDatagram(d){ const u=new Uint8Array(d.buffer,d.byteOffset,d.byteLength),v=new DataView(d.buffer,d.byteOffset,d.byteLength); if(d.byteLength<32||ascii4(u,0)!=='FXV3'||u[4]!==3||u16(v,6)!==32) throw new Error('bad FXV3 packet'); const sid=u32(v,8),payload=u16(v,28); if(32+payload!==d.byteLength) throw new Error('bad FXV3 payload length'); if(streamId!==sid){ streamId=sid; key=null; keyFrameId=null; $('stream').textContent=sid; } stats.packets++; await processCodec(d.slice(32)); }
async function processRecord(r){ const u=new Uint8Array(r.buffer,r.byteOffset,r.byteLength),v=new DataView(r.buffer,r.byteOffset,r.byteLength); if(r.byteLength<28||ascii4(u,0)!=='FXB1'||u16(v,4)!==1||u16(v,6)!==28) throw new Error('bad FXB1 record'); const recordBytes=u32(v,8),count=u16(v,24); if(recordBytes!==r.byteLength) throw new Error('bad record size'); let p=28; for(let i=0;i<count;i++){ if(p+2>r.length) throw new Error('truncated packet length'); const n=u16(v,p); p+=2; if(p+n>r.length) throw new Error('truncated packet'); await processDatagram(r.slice(p,p+n)); p+=n; } if(p!==r.length) throw new Error('record trailing bytes'); stats.records++; putStats(); }
function concat(a,b){ const c=new Uint8Array(a.length+b.length); c.set(a); c.set(b,a.length); return c; }
async function run(){ setState('connecting…'); const res=await fetch('/flowx.bin',{cache:'no-store'}); if(!res.ok||!res.body) throw new Error('HTTP '+res.status); setState('connected'); const rd=res.body.getReader(); let buf=new Uint8Array(0); while(true){ const {value,done}=await rd.read(); if(done) throw new Error('stream ended'); buf=concat(buf,value); while(buf.length>=12){ const v=new DataView(buf.buffer,buf.byteOffset,buf.byteLength); if(ascii4(buf,0)!=='FXB1'){ buf=buf.slice(1); stats.errors++; putStats(); continue; } const n=u32(v,8); if(n<28||n>16*1024*1024){ buf=buf.slice(4); stats.errors++; putStats(); continue; } if(buf.length<n) break; const rec=buf.slice(0,n); buf=buf.slice(n); try{ await processRecord(rec); }catch(e){ stats.errors++; putStats(); console.error(e); } } } }
async function reconnectLoop(){ for(;;){ try{ await run(); }catch(e){ setState(String(e),false); stats.errors++; putStats(); await new Promise(r=>setTimeout(r,1000)); } } }
reconnectLoop();
})();
)FLOWXJS";
    return kJs;
}

} // namespace flowx
