'use strict';
// Run the shipped browser control flow with native JPEG fixtures, a recording
// canvas/WebGL stub, and a virtual clock. No browser download is needed.
const assert=require('node:assert/strict'),fs=require('node:fs'),vm=require('node:vm');
const path=process.argv[2],fixture=JSON.parse(fs.readFileSync(path,'utf8'));
const source=fs.readFileSync(path+'.js','utf8');
const decoded=new Map(fixture.regions.map(r=>[Buffer.from(r.jpeg).toString('base64'),Uint8Array.from(r.rgba)]));
const {fillMissingPixels}=require('./flowx_restart_browser.js');
function region(i,frame=100,stream=7,scale=1) {
  const u=Uint8Array.from(fixture.regions[i].wire),v=new DataView(u.buffer);
  v.setUint32(8,frame,true);v.setUint32(4,stream,true);
  v.setUint32(12,1000000000+((frame-100)|0)*32000,true);v.setUint32(16,0,true);
  v.setUint16(20,fixture.width*scale,true);v.setUint16(22,fixture.height*scale,true);
  return u;
}
function patch(frame,key,stream=7,scale=1) {
  const u=new Uint8Array(51),v=new DataView(u.buffer);
  v.setUint16(0,0x5846,true);u[2]=0x42;v.setUint32(4,stream,true);v.setUint32(8,frame,true);
  v.setUint32(12,1000000000+((frame-100)|0)*32000,true);
  v.setUint16(20,(frame-key)>>>0,true);v.setUint16(22,fixture.width*scale,true);v.setUint16(24,fixture.height*scale,true);
  v.setFloat32(27,1,true);v.setFloat32(43,1,true);return u;
}
function harness(delayMs=0,extraQuery='') {
  let now=0,nextTimer=0,resizes=0,program=null;
  const timers=new Map(),nodes=new Map(),uploads=[],filters=[],uniforms=[],draws=[];
  const location={search:'?playout_ms='+delayMs+extraQuery};
  const gl=new Proxy({}, {get:(_,name)=>{
    if(/^[A-Z_0-9]+$/.test(name)) return name;
    if(name==='getShaderParameter'||name==='getProgramParameter') return ()=>true;
    if(name==='checkFramebufferStatus') return ()=>'FRAMEBUFFER_COMPLETE';
    if(name.startsWith('create')) return ()=>({});
    if(name==='texSubImage2D')return (...a)=>uploads.push({width:a[4],height:a[5],pixels:Uint8Array.from(a[8])});
    if(name==='texParameteri')return (...a)=>filters.push(a);
    if(name==='getUniformLocation')return (_,name)=>name;
    if(name==='uniform1i')return (name,value)=>uniforms.push({name,value});
    if(name==='useProgram')return p=>{program=p;};
    if(name==='drawArrays')return ()=>draws.push(program);
    return ()=>{};
  }});
  const canvas={getContext:()=>gl,style:{}};
  for(const name of ['width','height']) {
    let value=0;Object.defineProperty(canvas,name,{get:()=>value,set:v=>{value=v;resizes++;}});
  }
  nodes.set('view',canvas);
  const context=vm.createContext({
    console,Uint8Array,DataView,Float32Array,URLSearchParams,location,
    performance:{now:()=>now},
    requestAnimationFrame:fn=>{const id=++nextTimer;timers.set(id,{fn,at:(Math.floor(now/16)+1)*16});return id;},
    cancelAnimationFrame:id=>timers.delete(id),
    document:{getElementById:id=>{
      if(!nodes.has(id))nodes.set(id,{listeners:{},addEventListener(type,fn){this.listeners[type]=fn;}});
      return nodes.get(id);
    }},
    setTimeout:(fn,ms)=>{const id=++nextTimer;timers.set(id,{fn,at:now+ms});return id;},
    clearTimeout:id=>timers.delete(id),
    fixtureDecode:async (jpeg,width,height)=>{
      const rgba=decoded.get(Buffer.from(jpeg).toString('base64'));
      assert.ok(rgba,'unexpected JPEG bytes');assert.equal(rgba.length,width*height*4);return rgba;
    }
  });
  const hook='\nreconnectLoop();\n';
  assert.ok(source.includes(hook));
  vm.runInContext(source.replace(hook,`
decodeRegion=globalThis.fixtureDecode;
globalThis.testBrowser={processDatagram,stats,filterProgram:smoothFillProg,state:()=>({
  frame:lastRenderedFrame,shown:lastPresentedFrame,key:keyFrameId,pending:restartPresentation!==null,stream:streamId,
  queued:playout.frames.length,pool:presentationPool.length,filtered:renderKeyTex===smoothKeyTex
}),reference:()=>restartAssembly?({
  pixels:restartAssembly.pixels.slice(),received:restartAssembly.received.slice(),
  width:restartAssembly.width,height:restartAssembly.height
}):null};
`),context);
  return {
    ...context.testBrowser,canvas,timers,uploads,filters,uniforms,nodes,location,resizes:()=>resizes,
    filterPasses:()=>draws.filter(p=>p===context.testBrowser.filterProgram).length,
    advance(ms) {
      const end=now+ms;
      for(;;) {
        const due=[...timers].filter(([,t])=>t.at<=end).sort((a,b)=>a[1].at-b[1].at)[0];
        if(!due)break;now=due[1].at;timers.delete(due[0]);due[1].fn();
      }
      now=end;
    }
  };
}
(async()=>{
  const count=fixture.regions.length,kept=Math.floor(count*.65);
  // Startup and inactivity: one valid region must not immediately flash black.
  const quiet=harness();
  assert.equal(quiet.nodes.get('fillGaps').checked,true,'filling must remain the default');
  assert.equal(quiet.nodes.get('smoothFills').checked,true,'smoothing must be enabled by default');
  assert.ok(!quiet.filters.some(a=>a[2]==='NEAREST'),'normal rendering lost smooth sampling');
  await quiet.processDatagram(patch(101,100));
  assert.equal(quiet.stats.renders,0);assert.equal(quiet.timers.size,0);
  await quiet.processDatagram(region(0));
  assert.equal(quiet.stats.renders,0);assert.equal(quiet.resizes(),0);
  quiet.advance(99);assert.equal(quiet.stats.renders,0);
  await quiet.processDatagram(region(1));
  const waiting=quiet.reference(),filled=waiting.pixels.slice();
  assert.equal(fillMissingPixels(filled,waiting.received,waiting.width,waiting.height),true);
  quiet.advance(99);assert.equal(quiet.stats.renders,0,'quiet timer was not extended');
  assert.deepEqual(quiet.reference().pixels,waiting.pixels,'holes filled before wait ended');
  assert.equal(quiet.filterPasses(),0,'smoothing ran before the wait ended');
  quiet.advance(1);assert.equal(quiet.stats.renders,1);assert.equal(quiet.state().frame,100);
  assert.equal(quiet.filterPasses(),1);assert.equal(quiet.state().filtered,true);
  assert.deepEqual(quiet.reference().pixels,filled,'timeout did not fill key holes');
  assert.deepEqual(quiet.reference().received,waiting.received);
  assert.deepEqual(quiet.uploads.at(-1).pixels,filled,'filled reference not uploaded to WebGL');
  await quiet.processDatagram(region(2));quiet.advance(1000);
  assert.equal(quiet.stats.renders,1,'late region replayed key');
  assert.equal(quiet.filterPasses(),1,'late region ran the filter immediately');
  const late=quiet.reference(),lateExpected=late.pixels.slice();
  fillMissingPixels(lateExpected,late.received,late.width,late.height);
  await quiet.processDatagram(patch(101,100));
  assert.deepEqual(quiet.reference().pixels,lateExpected,'PATCH did not refresh late neighbour colors');
  assert.equal(quiet.uniforms.findLast(u=>u.name==='uFillGaps').value,1);
  assert.equal(quiet.filterPasses(),2,'late data did not refresh the filtered reference');
  await quiet.processDatagram(patch(102,100));
  assert.equal(quiet.filterPasses(),2,'unchanged PATCH repeated the smoothing pass');
  assert.equal(quiet.state().filtered,true,'unchanged PATCH lost the filtered texture');
  for(let i=3;i<count;i++)await quiet.processDatagram(region(i));
  await quiet.processDatagram(patch(103,100));
  assert.equal(quiet.state().filtered,false,'recovered key retained stale smoothed pixels');
  assert.equal(quiet.filterPasses(),2,'fully recovered key was filtered');
  assert.deepEqual(quiet.reference().pixels,Uint8Array.from(fixture.complete));

  const nn=harness(0,'&smooth_fill=0&keep=example');
  assert.equal(nn.nodes.get('smoothFills').checked,false);assert.equal(nn.filterProgram,null);
  await nn.processDatagram(region(0));nn.advance(100);
  assert.equal(nn.filterPasses(),0);assert.equal(nn.state().filtered,false);
  assert.deepEqual(nn.uploads.at(-1).pixels,nn.reference().pixels,'NN-only output was not uploaded');
  const smooth=nn.nodes.get('smoothFills');smooth.checked=true;smooth.listeners.change();
  const smoothParams=new URLSearchParams(nn.location.search);
  assert.equal(smoothParams.get('smooth_fill'),'1');assert.equal(smoothParams.get('fill_gaps'),'1');
  assert.equal(smoothParams.get('keep'),'example');

  // The shipped URL option must bypass both fill paths, including quiet/PATCH
  // release and new streams. Assert black by the actual per-parity receipt mask.
  const raw=harness(0,'&fill_gaps=0&keep=example');
  assert.equal(raw.nodes.get('fillGaps').checked,false);
  assert.equal(raw.nodes.get('smoothFills').disabled,true);assert.equal(raw.filterProgram,null);
  assert.equal(raw.canvas.style.imageRendering,'pixelated');
  assert.deepEqual(raw.filters.filter(a=>a[2]==='NEAREST').map(a=>a[1]),['TEXTURE_MIN_FILTER','TEXTURE_MAG_FILTER']);
  function checkMissingBlack(ref) {
    let missing=0;
    for(let y=0;y<ref.height;y++)for(let x=0;x<ref.width;x++) {
      const b=(y>>3)*(ref.width/16)+(x>>4);
      if(!ref.received[2*b+(x&1)]) {
        const p=(y*ref.width+x)*4;
        assert.deepEqual(Array.from(ref.pixels.subarray(p,p+4)),[0,0,0,255]);missing++;
      }
    }
    assert.ok(missing>0,'test needs missing pixels');
  }
  await raw.processDatagram(region(0));
  const rawWaiting=raw.reference();checkMissingBlack(rawWaiting);
  raw.advance(100);assert.equal(raw.stats.renders,1);
  assert.deepEqual(raw.reference().pixels,rawWaiting.pixels,'timeout filled debug view');
  assert.equal(raw.uploads.length,0,'timeout uploaded estimates in debug mode');
  await raw.processDatagram(region(1));await raw.processDatagram(patch(101,100));
  checkMissingBlack(raw.reference());
  if(fixture.tile_map.length)assert.deepEqual(raw.uploads.at(-1).pixels,raw.reference().pixels,'raw shuffled PATCH used a stale texture');
  assert.equal(raw.uniforms.findLast(u=>u.name==='uFillGaps').value,0,'PATCH retained border filling');
  await raw.processDatagram(region(0,110));await raw.processDatagram(patch(111,110));
  checkMissingBlack(raw.reference());
  await raw.processDatagram(region(0,1,8));raw.advance(100);checkMissingBlack(raw.reference());
  assert.equal(raw.filterPasses(),0);
  // UI changes reload with a clean reference, preserving other options/delay.
  const delay=raw.nodes.get('playoutDelay');delay.value='80';delay.listeners.change();
  const fill=raw.nodes.get('fillGaps');fill.checked=true;fill.listeners.change();
  const params=new URLSearchParams(raw.location.search);
  assert.equal(params.get('fill_gaps'),'1');assert.equal(params.get('playout_ms'),'80');
  assert.equal(params.get('keep'),'example');
  fill.checked=false;fill.listeners.change();
  assert.equal(new URLSearchParams(raw.location.search).get('fill_gaps'),'0');
  assert.equal(harness(0,'&fill_gaps=1').nodes.get('fillGaps').checked,true);

  const h=harness();
  for(let i=0;i<count;i++)await h.processDatagram(region(i));
  assert.deepEqual(h.reference().pixels,Uint8Array.from(fixture.complete),'complete key not in spatial order');
  if(fixture.tile_map.length)assert.deepEqual(h.uploads.at(-1).pixels,h.reference().pixels,'complete shuffled key used a stale texture');
  assert.equal(h.stats.renders,1,'complete key not rendered immediately');assert.equal(h.state().pending,false);
  assert.equal(h.filterPasses(),0);assert.equal(h.state().filtered,false);
  h.advance(16);assert.equal(h.state().shown,100);
  await h.processDatagram(patch(101,100));
  const before=h.stats.renders,oldResizes=h.resizes();
  // A resolution change must also keep the old canvas until presentation.
  for(let i=0;i<kept;i++)await h.processDatagram(region(i,110,7,2));
  assert.equal(h.state().frame,101);assert.equal(h.stats.renders,before,'partial key flashed');
  assert.equal(h.resizes(),oldResizes,'pending key cleared canvas on resize');
  await h.processDatagram(patch(111,110,7,2));
  assert.equal(h.stats.renders,before+2,'key capture-time slot was lost');
  h.advance(16);
  assert.equal(h.stats.keys,2);assert.equal(h.canvas.width,fixture.width*2);
  for(let i=kept;i<count;i++)await h.processDatagram(region(i,110,7,2));
  h.advance(1000);assert.equal(h.stats.renders,before+2,'stale timeout/late data rewound display');
  assert.equal(h.state().frame,111);
  await h.processDatagram(patch(112,110,7,2));assert.equal(h.state().frame,112);

  // All following PATCH packets may be lost: a next key closes the prior burst.
  for(let i=0;i<Math.max(1,Math.floor(kept/2));i++)await h.processDatagram(region(i,120));
  const oldKey=h.reference(),oldExpected=oldKey.pixels.slice();
  assert.equal(fillMissingPixels(oldExpected,oldKey.received,oldKey.width,oldKey.height),true);
  const beforeBoundary=h.stats.renders;
  // Use a genuinely incomplete old key, rather than a half that covers all
  // paired blocks, to verify it survives the next assembly's creation.
  await h.processDatagram(region(0,130));
  assert.deepEqual(h.uploads.at(-1).pixels,oldExpected,'new key discarded the old key before filling it');
  assert.equal(h.state().frame,120);assert.equal(h.stats.renders,beforeBoundary+1);
  assert.equal(h.state().key,130);assert.equal(h.state().pending,true);
  assert.equal(h.reference().received.reduce((n,v)=>n+v,0),fixture.regions[0].rgba.length/256);
  // Duplicates do not keep a pending key hidden indefinitely.
  h.advance(99);await h.processDatagram(region(0,130));
  h.advance(1);assert.equal(h.state().frame,130);
  // Stream resets cancel old pending timers and preserve the displayed canvas.
  await h.processDatagram(region(0,140));h.advance(50);
  const resetRenders=h.stats.renders,resetResizes=h.resizes();
  await h.processDatagram(region(0,1,8));
  h.advance(50);assert.equal(h.stats.renders,resetRenders,'old stream timeout fired');
  assert.equal(h.resizes(),resetResizes);
  await h.processDatagram(region(1,140));assert.equal(h.state().stream,8);
  h.advance(50);assert.equal(h.state().frame,1);assert.equal(h.stats.renders,resetRenders+1);
  // Wrapped frame IDs still close the correct pending key.
  const wrap=harness();
  await wrap.processDatagram(region(0,0xfffffffe));
  await wrap.processDatagram(patch(1,0xfffffffe));
  wrap.advance(1000);assert.equal(wrap.state().frame,1);assert.equal(wrap.stats.renders,2);
  assert.equal(wrap.stats.shown,1,'zero-delay playback replayed overdue key');
  // Key + PATCH become ready together after 35% loss. They must still occupy
  // distinct 32 ms capture slots on the actual shipped browser control path.
  const timed=harness(40);
  for(let i=0;i<kept;i++)await timed.processDatagram(region(i));
  timed.advance(32);await timed.processDatagram(patch(101,100));
  assert.equal(timed.stats.shown,0);
  timed.advance(16);assert.equal(timed.state().shown,100);
  timed.advance(32);assert.equal(timed.state().shown,101);
  await timed.processDatagram(patch(102,100));await timed.processDatagram(patch(103,100));
  timed.advance(32);assert.equal(timed.state().shown,102,'burst skipped an on-time PATCH');
  timed.advance(32);assert.equal(timed.state().shown,103);
  assert.equal(timed.stats.playoutDrops,0);
  assert.ok(timed.state().queued+timed.state().pool<=5,'unbounded snapshot textures');
  console.log('PASS: no first-region flash, completion/PATCH/burst/quiet presentation, 35% loss, deferred resize, duplicates, late data, stream reset, frame wrap');
})().catch(e=>{console.error(e);process.exitCode=1;});
