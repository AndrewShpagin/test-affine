// Bounded presentation of decoded frames on the sender's capture timeline.
// Rendering/decoding can run ahead; only presentation follows the display clock.
(function(root) {
  'use strict';
  const newer=(a,b)=>((a-b)|0)>0;
  class FramePlayout {
    constructor({now,requestFrame,cancelFrame,present,release,onDrop=()=>{},delayMs=60,maxFrames=4}) {
      Object.assign(this,{now,requestFrame,cancelFrame,present,release,onDrop,maxFrames});
      this.frames=[];this.request=null;this.offset=null;this.observed=null;this.lastQueued=null;
      this.setDelay(delayMs);
    }
    setDelay(ms) { this.delayMs=Number.isFinite(ms)?Math.max(0,Math.min(200,ms)):60; }
    discard(frame) { this.release(frame.payload);this.onDrop(); }
    reset() {
      if(this.request!==null)this.cancelFrame(this.request);
      this.request=null;
      for(const frame of this.frames)this.discard(frame);
      this.frames=[];this.offset=null;this.observed=null;this.lastQueued=null;
    }
    observe(frameId,captureMs,arrivalMs=this.now()) {
      if(!Number.isFinite(captureMs)||captureMs<=0)return;
      if(this.observed && !newer(frameId,this.observed.frameId))return;
      if(this.observed) {
        const elapsedCapture=captureMs-this.observed.captureMs;
        const elapsedArrival=arrivalMs-this.observed.arrivalMs;
        // Clock changes or a long transport stall must not create a permanent
        // backlog or schedule frames far into the future.
        if(elapsedCapture<=0||Math.abs(elapsedCapture-elapsedArrival)>1000)this.reset();
      }
      if(this.offset===null)this.offset=arrivalMs-captureMs;
      this.observed={frameId,captureMs,arrivalMs};
    }
    enqueue(frameId,captureMs,payload) {
      if(this.lastQueued!==null&&!newer(frameId,this.lastQueued)) {
        this.discard({payload});return;
      }
      this.lastQueued=frameId;
      this.frames.push({frameId,captureMs,payload,queuedAt:this.now()});
      if(this.frames.length>this.maxFrames) {
        while(this.frames.length>this.maxFrames)this.discard(this.frames.shift());
        // If the requested delay needs more storage than the bounded queue,
        // shorten buffering under pressure. Otherwise high-FPS input could
        // perpetually evict every frame before its deadline and show nothing.
        this.frames[0].urgent=true;
      }
      this.wake();
    }
    deadline(frame) {
      if(this.delayMs===0||frame.urgent)return frame.queuedAt;
      return this.offset!==null && Number.isFinite(frame.captureMs) && frame.captureMs>0
        ? frame.captureMs+this.offset+this.delayMs : frame.queuedAt+this.delayMs;
    }
    wake() {
      if(this.request===null && this.frames.length)
        this.request=this.requestFrame(()=>this.tick());
    }
    tick() {
      this.request=null;
      const now=this.now();
      let due=-1;
      for(let i=0;i<this.frames.length;i++) {
        if(this.deadline(this.frames[i])>now)break;
        due=i;
      }
      if(due>=0) {
        // At most one presentation per refresh. After a stall, skip stale work
        // instead of rapidly replaying every missed frame.
        for(let i=0;i<due;i++)this.discard(this.frames.shift());
        const frame=this.frames.shift();
        try { this.present(frame); } finally { this.release(frame.payload); }
      }
      this.wake();
    }
  }
  root.FlowXFramePlayout=FramePlayout;
  if(typeof module!=='undefined'&&module.exports)module.exports={FramePlayout};
})(globalThis);
