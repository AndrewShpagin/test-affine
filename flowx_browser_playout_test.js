'use strict';
const assert=require('node:assert/strict');
const {FramePlayout}=require('./flowx_browser_playout.js');
function harness(delayMs=40) {
  let now=0,id=0,drops=0;
  const callbacks=new Map(),shown=[],released=[];
  const q=new FramePlayout({
    delayMs,now:()=>now,
    requestFrame:fn=>{callbacks.set(++id,fn);return id;},cancelFrame:id=>callbacks.delete(id),
    present:f=>shown.push({id:f.frameId,at:now}),release:p=>released.push(p),onDrop:()=>drops++
  });
  return {q,shown,released,callbacks,drops:()=>drops,
    arrive(frame,capture,at) {now=at;q.observe(frame,capture);q.enqueue(frame,capture,frame);},
    tick(at) {now=at;const due=[...callbacks.values()];callbacks.clear();due.forEach(fn=>fn());}
  };
}
// Source: 25 fps. Network delivers two pairs in bursts instead of at 40 ms intervals.
const smooth=harness(),arrivals=[0,70,70,150,155];
let next=0;
for(let t=0;t<=240;t+=5) {
  while(next<arrivals.length&&arrivals[next]<=t) {
    smooth.arrive(next,1000000+40*next,arrivals[next]);next++;
  }
  smooth.tick(t);
}
assert.deepEqual(smooth.shown.map(f=>f.at),[40,80,120,160,200]);
assert.equal(smooth.drops(),0);
// Configured 20 fps: a partial key becomes ready alongside its following PATCH.
// A 60 ms buffer leaves a full 50 ms display slot for that key.
const keyBurst=harness(60);keyBurst.q.observe(0,1000,0);
keyBurst.arrive(0,1000,50);keyBurst.arrive(1,1050,50);
keyBurst.tick(60);keyBurst.tick(100);keyBurst.tick(110);
assert.deepEqual(keyBurst.shown,[{id:0,at:60},{id:1,at:110}]);
// Missing source frames retain their missing time slots; do not speed up motion.
const lost=harness();
lost.arrive(0,1000,0);lost.tick(40);
lost.arrive(1,1040,45);lost.tick(80);
lost.arrive(3,1120,120);lost.tick(160);
assert.deepEqual(lost.shown.map(f=>f.at),[40,80,160]);
// A stalled/hidden tab never accumulates unbounded frames or replays a burst.
const stalled=harness();
for(let i=0;i<20;i++)stalled.arrive(i,1000+20*i,20*i);
assert.equal(stalled.q.frames.length,4);assert.equal(stalled.drops(),16);
stalled.tick(500);
assert.deepEqual(stalled.shown,[{id:19,at:500}]);
assert.equal(stalled.drops(),19);assert.equal(stalled.callbacks.size,0);
assert.equal(new Set(stalled.released).size,20);assert.equal(stalled.released.length,20);
// A large requested delay with high-FPS input must not evict every frame
// before its deadline and starve presentation indefinitely.
const pressure=harness(200);
for(let i=0;i<100;i++) {
  pressure.arrive(i,1000+10*i,10*i);
  if(i%2===0)pressure.tick(10*i);
  assert.ok(pressure.q.frames.length<=4);
}
assert.ok(pressure.shown.length>40,'queue pressure starved playback');
assert.ok(pressure.shown[0].at<=60,'queue pressure failed to shorten buffering');
assert.ok(pressure.drops()>0);
// Runtime latency changes affect queued frames without resetting the timeline.
const low=harness(80);low.arrive(1,1000,0);low.tick(40);assert.equal(low.shown.length,0);
low.q.setDelay(0);low.tick(41);assert.deepEqual(low.shown,[{id:1,at:41}]);
low.q.setDelay(10000);assert.equal(low.q.delayMs,200);
low.q.setDelay(NaN);assert.equal(low.q.delayMs,60);
// Stream resets clear snapshots and pending animation callbacks.
const reset=harness();reset.arrive(100,1000,0);reset.q.reset();
assert.equal(reset.callbacks.size,0);assert.deepEqual(reset.released,[100]);
reset.arrive(1,90000,10);reset.tick(50);assert.deepEqual(reset.shown,[{id:1,at:50}]);
// Wall-clock discontinuities must not postpone playback for seconds/minutes.
const jump=harness();jump.arrive(1,1000,0);jump.arrive(2,1000000,20);
jump.tick(59);assert.equal(jump.shown.length,0);jump.tick(60);
assert.deepEqual(jump.shown,[{id:2,at:60}]);assert.equal(jump.drops(),1);
jump.arrive(3,2000,70);jump.tick(110);assert.equal(jump.shown.at(-1).id,3);
const offset=jump.q.offset;jump.q.observe(1,10000000,111);
assert.equal(jump.q.offset,offset,'late packet reset the clock');
// Missing/invalid timestamps fall back to arrival-time scheduling.
const missing=harness();missing.arrive(1,0,10);missing.tick(49);assert.equal(missing.shown.length,0);
missing.tick(50);assert.equal(missing.shown[0].id,1);
missing.arrive(2,NaN,60);missing.tick(100);assert.equal(missing.shown[1].id,2);
const wrapped=harness();wrapped.arrive(0xfffffffe,1000,0);wrapped.tick(40);
wrapped.arrive(1,1040,40);wrapped.tick(80);wrapped.arrive(0xfffffffe,1000,81);
assert.deepEqual(wrapped.shown.map(f=>f.id),[0xfffffffe,1]);assert.equal(wrapped.q.frames.length,0);
console.log('PASS: capture-time spacing under burst arrivals, lost-frame gaps, bounded backlog, late-frame drops, delay control, stream/clock resets, timestamp fallback, frame wrap');
