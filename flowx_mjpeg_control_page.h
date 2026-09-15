#pragma once

namespace flowx {
inline constexpr char kMjpegControlPage[] = R"FXMJPEG(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>FlowX MJPEG controls</title>
<style>
body{margin:0;background:#14171c;color:#eee;font:16px system-ui,sans-serif}
main{max-width:1000px;margin:auto;padding:20px}h1{font-size:24px;margin-top:0}
.controls{display:flex;gap:24px;flex-wrap:wrap;padding:16px;background:#232832;border-radius:8px}
label{display:flex;gap:8px;align-items:center}input{width:18px;height:18px}
small,p{color:#bbc3d0}a{color:#83c5ff}#state{min-height:1.5em}
#preview{display:block;max-width:100%;margin:16px auto;background:#000}
.error{color:#ff9292}
</style></head><body><main>
<h1>MJPEG controls</h1>
<div class="controls">
<label><input id="fill" type="checkbox" disabled>Fill missing pixels</label>
<label><input id="smooth" type="checkbox" disabled>Smooth filled pixels</label>
</div>
<p id="state" role="status" aria-live="polite">Connecting...</p>
<p>Changes apply to every MJPEG viewer and the JPEG snapshot. Restarting the receiver restores the configuration file settings.</p>
<p><a id="snapshot" target="_blank" rel="noopener">JPEG snapshot</a> &middot;
<a id="stream" target="_blank" rel="noopener">MJPEG stream</a> &middot;
<a href="/flowx.html">Direct browser decoder</a></p>
<img id="preview" alt="Waiting for received video">
<small>Disabling filling leaves missing pixels black, including missing odd/even columns. Smoothing is used only while filling is enabled.</small>
</main><script>
'use strict';
const fill=document.getElementById('fill'),smooth=document.getElementById('smooth');
const state=document.getElementById('state'),preview=document.getElementById('preview');
let busy=false,loaded=false;
function show(s){
 fill.checked=s.fill_gaps;smooth.checked=s.smooth_fill;
 fill.disabled=busy;smooth.disabled=busy||!s.fill_gaps;
 state.className='';
 state.textContent=s.revision!==s.applied_revision?'Applying...':
  (!s.fill_gaps?'Filling off':(s.smooth_fill?'Filling and smoothing on':'Filling on; smoothing off'));
 document.getElementById('snapshot').href=s.frame_endpoint;
 document.getElementById('stream').href=s.stream_endpoint;
 if(!loaded){preview.src=s.stream_endpoint;loaded=true;}
}
async function refresh(patch){
 if(busy)return;busy=true;fill.disabled=smooth.disabled=true;
 try{
  const options=patch?{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(patch)}:{cache:'no-store'};
  const r=await fetch('/decoder.json',options),s=await r.json();
  if(!r.ok)throw Error(s.error||'Receiver request failed');
  busy=false;show(s);
 }catch(e){busy=false;state.className='error';state.textContent=e.message;}
}
fill.addEventListener('change',()=>refresh({fill_gaps:fill.checked}));
smooth.addEventListener('change',()=>refresh({smooth_fill:smooth.checked}));
refresh();setInterval(()=>refresh(),1000);
</script></body></html>)FXMJPEG";
} // namespace flowx
