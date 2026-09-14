'use strict';
// Run against fixtures exported by jpeg_restart_test: tests the actual native
// entropy/header bytes and expected RGBA, without requiring a DOM in CTest.
const assert=require('node:assert/strict'),fs=require('node:fs');
const {RestartAssembler,parseRegion,makeJpeg}=require('./flowx_restart_browser.js');
const f=JSON.parse(fs.readFileSync(process.argv[2],'utf8'));
const packets=f.regions.map(r=>Uint8Array.from(r.wire));
const decoded=new Map(f.regions.map(r=>[Buffer.from(r.jpeg).toString('base64'),Uint8Array.from(r.rgba)]));
function decode(jpeg,w,h) {
  const pixels=decoded.get(Buffer.from(jpeg).toString('base64'));
  assert.ok(pixels,'JS reconstructed different JPEG bytes from C++');
  assert.equal(pixels.length,w*h*4);return Promise.resolve(pixels);
}
function expected(indices) {
  const pixels=new Uint8Array(f.width*f.height*4),mask=new Uint8Array(f.width*f.height);
  for(let p=3;p<pixels.length;p+=4) pixels[p]=255;
  for(const i of indices) {
    const r=parseRegion(packets[i]),source=f.regions[i].rgba;
    for(let y=0;y<8;y++) for(let x=0;x<r.width;x++) {
      const dest=(r.y+y)*f.width+r.x+2*x,src=(y*r.width+x)*4;
      pixels.set(source.slice(src,src+4),dest*4);mask[dest]=1;
    }
  }
  for(let y=0;y<f.height;y++) for(let x=0;x<f.width;x++) {
    const own=y*f.width+x,other=y*f.width+(x^1);
    if(!mask[own]&&mask[other]) pixels.set(pixels.slice(other*4,other*4+4),own*4);
  }
  return pixels;
}
function frame(packet,id) {
  const u=packet.slice();new DataView(u.buffer).setUint32(8,id,true);return u;
}
(async()=>{
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
  console.log('PASS: browser/native JPEG bytes, 35% loss (12 trials), fills, duplicates, late restore, frame wrap, async reorder');
})().catch(e=>{console.error(e);process.exitCode=1;});
