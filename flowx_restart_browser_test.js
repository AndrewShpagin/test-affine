'use strict';
// Run against fixtures exported by jpeg_restart_test: tests the actual native
// entropy/header bytes and expected RGBA, without requiring a DOM in CTest.
const assert=require('node:assert/strict'),fs=require('node:fs');
const {RestartAssembler,parseRegion,makeJpeg,tilePermutation}=require('./flowx_restart_browser.js');
const {smoothReference}=require('./flowx_smooth_fill_test.js');
const f=JSON.parse(fs.readFileSync(process.argv[2],'utf8'));
const packets=f.regions.map(r=>Uint8Array.from(r.wire));
const decoded=new Map(f.regions.map(r=>[Buffer.from(r.jpeg).toString('base64'),Uint8Array.from(r.rgba)]));
function decode(jpeg,w,h) {
  const pixels=decoded.get(Buffer.from(jpeg).toString('base64'));
  assert.ok(pixels,'JS reconstructed different JPEG bytes from C++');
  assert.equal(pixels.length,w*h*4);return Promise.resolve(pixels);
}
function expected(indices,fillGaps=true) {
  const pixels=new Uint8Array(f.width*f.height*4),mask=new Uint8Array(f.width*f.height);
  for(let p=3;p<pixels.length;p+=4) pixels[p]=255;
  for(const i of indices) {
    const r=parseRegion(packets[i]),source=f.regions[i].rgba;
    for(let y=0;y<8;y++) for(let x=0;x<r.width;x++) {
      const columns=r.lw/8,index=(r.y/8)*columns+(r.x>>4)+(x>>3);
      const target=r.layout===1?f.tile_map[index]:index;
      const dest=(Math.floor(target/columns)*8+y)*f.width+(target%columns)*16+2*(x%8)+(r.x&1),src=(y*r.width+x)*4;
      pixels.set(source.slice(src,src+4),dest*4);mask[dest]=1;
    }
  }
  if(fillGaps)for(let y=0;y<f.height;y++) for(let x=0;x<f.width;x++) {
    const own=y*f.width+x,other=y*f.width+(x^1);
    if(!mask[own]&&mask[other]) pixels.set(pixels.slice(other*4,other*4+4),own*4);
  }
  return pixels;
}
function frame(packet,id) {
  const u=packet.slice();new DataView(u.buffer).setUint32(8,id,true);return u;
}
(async()=>{
  assert.ok(packets.some(p=>p.length>1300),'fixture never exceeded the soft target');
  const tooBig=new Uint8Array(65508);tooBig.set(packets[0]);
  assert.throws(()=>parseRegion(tooBig),/size/,'absolute UDP limit ignored');
  // Apply the actual upload instructions to a simulated texture. Cross-row
  // packets must never be emitted as rectangles extending beyond its width.
  const uploaded=new Uint8Array(f.width*f.height*4);
  const uploadAssembler=new RestartAssembler(f.headers,decode,{fillGaps:false});
  let crossed=false,partial=false;
  for(let i=0;i<packets.length;i++) {
    const r=parseRegion(packets[i]),update=await uploadAssembler.accept(packets[i]);
    crossed=crossed||((r.x>>1)+r.width>r.lw);
    partial=partial||r.width<parseRegion(packets[0]).width;
    if(update.newKeyframe||update.fullUpload)uploaded.set(uploadAssembler.pixels);
    else {
      assert.ok(update.x>=0&&update.x+update.width<=f.width&&update.y+update.height<=f.height,
        'GPU rectangle exceeds reference bounds');
      for(let y=0;y<update.height;y++)uploaded.set(update.pixels.subarray(y*update.width*4,(y+1)*update.width*4),
        ((update.y+y)*f.width+update.x)*4);
    }
    assert.deepEqual(uploaded,uploadAssembler.pixels,'GPU update omitted wrapped blocks');
  }
  assert.deepEqual(uploaded,Uint8Array.from(f.complete));
  if(f.width===80) {
    assert.ok(crossed,'narrow fixture never crossed a row');
    assert.ok(partial,'narrow fixture never tested the final short segment');
  }
  for(const [x,y,width] of [[f.width,0,8],[f.width-16,f.height-8,16]]) {
    const bad=packets[0].slice(),v=new DataView(bad.buffer);
    v.setUint16(28,x,true);v.setUint16(30,y,true);v.setUint16(32,width,true);
    assert.throws(()=>parseRegion(bad),/geometry/,'out-of-raster block range accepted');
  }
  for(const sample of f.concealment) {
    const a=new RestartAssembler(f.headers,decode,{fillGaps:sample.mode!=='raw',smoothFills:sample.mode==='smooth'});
    for(const i of sample.indices)await a.accept(packets[i]);
    a.fillMissing();
    const output=sample.mode==='smooth'&&a.smoothing?smoothReference(a.pixels,a.smoothing,a.width,a.height):a.pixels;
    assert.equal(output.length,sample.rgba.length);
    for(let i=0;i<output.length;i++)assert.ok(Math.abs(output[i]-sample.rgba[i])<=1,'native decoder differs from browser '+sample.mode);
  }
  assert.deepEqual(Array.from(tilePermutation(32,16)),[0,6,5,7,2,1,3,4],'shuffle v1 wire vector changed');
  assert.deepEqual(Array.from(tilePermutation(8,8)),[0]);
  if(f.tile_map.length)assert.deepEqual(Array.from(tilePermutation(f.width/2,f.height)),f.tile_map,'C++/JS shuffle mismatch');
  for(let seed=1;seed<=12;seed++) {
    let state=seed;
    const rand=()=>{state=(Math.imul(state,1664525)+1013904223)>>>0;return state;};
    const order=packets.map((_,i)=>i);
    for(let i=order.length-1;i>0;i--) {const j=rand()%(i+1);[order[i],order[j]]=[order[j],order[i]];}
    const a=new RestartAssembler(f.headers,decode),kept=Math.floor(order.length*.65);
    let events=0;
    for(const i of order.slice(0,kept)) {
      const result=await a.accept(packets[i]);events+=Number(result.newKeyframe);
      assert.equal(await a.accept(packets[i]),null,'duplicate accepted');
    }
    assert.equal(events,1);
    assert.equal(a.receivedCount,a.received.reduce((sum,n)=>sum+n,0),'duplicate counted twice');
    assert.deepEqual(a.pixels,expected(order.slice(0,kept)),'35% loss reconstruction');
    const beforeFill=a.pixels.slice(),received=a.received.slice(),receivedCount=a.receivedCount;
    a.fillMissing();
    assert.deepEqual(a.received,received,'concealment claimed lost blocks');
    assert.equal(a.receivedCount,receivedCount);
    assert.equal(a.fillMissing(),false,'clean key was filled again');
    for(let y=0;y<a.height;y++)for(let x=0;x<a.width;x++) {
      const block=(y>>3)*(a.lw/8)+(x>>4);
      if(received[2*block]||received[2*block+1]) {
        const p=(y*a.width+x)*4;
        assert.deepEqual(a.pixels.subarray(p,p+4),beforeFill.subarray(p,p+4),'known/counterpart pixel changed');
      }
    }
    const displayed=a.pixels.slice(),frozen=displayed.slice();
    for(const i of order.slice(kept)) {
      assert.equal((await a.accept(packets[i])).newKeyframe,false);
      a.fillMissing();
    }
    assert.deepEqual(displayed,frozen,'late region changed display snapshot');
    assert.deepEqual(a.pixels,Uint8Array.from(f.complete),'late restore differs from native');
    assert.equal(a.receivedCount,a.received.length,'completion count missed late blocks');
    assert.equal(await a.accept(frame(packets[0],99)),null);
    assert.equal((await a.accept(frame(packets[0],110))).newKeyframe,true);
    assert.deepEqual(a.pixels,expected([0]),'new frame retained old mask');
    assert.equal(a.receivedCount,parseRegion(packets[0]).width/8,'new frame retained old count');
    assert.equal(await a.accept(packets[1]),null);
    const bad=frame(packets[1],111);bad[34]=99;
    await assert.rejects(a.accept(bad),/geometry/);assert.equal(a.frameId,110);
    const mixed=frame(packets[1],110);mixed[35]^=1;
    await assert.rejects(a.accept(mixed),/metadata/);
    const unknown=frame(packets[1],111);unknown[35]=2;
    await assert.rejects(a.accept(unknown),/geometry/);assert.equal(a.frameId,110);
  }
  // Either parity can arrive first. With concealment off, only its actual
  // samples exist; absent columns and regions stay opaque black even after fill.
  for(const parity of [0,1]) {
    const indices=packets.map((_,i)=>i).filter(i=>(parseRegion(packets[i]).x&1)===parity);
    const raw=new RestartAssembler(f.headers,decode,{fillGaps:false});
    await raw.accept(packets[indices[0]]);
    assert.equal(raw.fillMissing(),false);
    assert.deepEqual(raw.pixels,expected(indices.slice(0,1),false),'raw gaps were filled');
    for(const i of indices.slice(1))await raw.accept(packets[i]);
    const mask=raw.received.slice();
    assert.equal(raw.fillMissing(),false);
    assert.deepEqual(raw.pixels,expected(indices,false),'missing parity did not stay black');
    assert.deepEqual(raw.received,mask);
    assert.equal(raw.receivedCount,raw.received.length/2);
    for(let i=0;i<packets.length;i++)await raw.accept(packets[i]);
    assert.deepEqual(raw.pixels,Uint8Array.from(f.complete),'late parity failed to restore raw view');
    await raw.accept(frame(packets[indices[0]],110));
    assert.deepEqual(raw.pixels,expected(indices.slice(0,1),false),'new raw key retained old pixels');
  }
  const wrap=new RestartAssembler(f.headers,decode);
  await wrap.accept(frame(packets[0],0xfffffffe));await wrap.accept(frame(packets[1],1));
  assert.equal(wrap.frameId,1);assert.equal(await wrap.accept(frame(packets[0],0xfffffffe)),null);
  // Simulate asynchronous JPEG decodes completing in reverse frame order.
  const pending=[];
  const racing=new RestartAssembler(f.headers,(bytes)=>new Promise(resolve=>pending.push(
    ()=>resolve(decoded.get(Buffer.from(bytes).toString('base64'))))));
  const old=racing.accept(packets[0]),fresh=racing.accept(frame(packets[1],101));
  pending[1]();await fresh;pending[0]();assert.equal(await old,null);assert.equal(racing.frameId,101);
  for(const i of [0,Math.floor(packets.length/2),packets.length-1])
    assert.deepEqual(Array.from(makeJpeg(parseRegion(packets[i]),f.headers)),f.regions[i].jpeg);
  console.log('PASS: browser/native JPEG bytes and tile layout, 35% loss (12 trials), fills on/off, black odd/even columns, duplicates, late restore, frame wrap, async reorder');
})().catch(e=>{console.error(e);process.exitCode=1;});
