'use strict';
// Optional integration test with Playwright + Chromium. First export fixtures:
// build/jpeg_restart_test --export build/restart-fixtures.json
// node flowx_restart_webgl_test.js build/restart-fixtures.json [chromium-path]
const fs=require('node:fs'),assert=require('node:assert/strict');
const {chromium}=require('playwright');
const path=process.argv[2],fixture=JSON.parse(fs.readFileSync(path,'utf8'));
(async()=>{
  const browser=await chromium.launch({
    ...(process.argv[3]?{executablePath:process.argv[3]}:{}),
    args:['--use-angle=swiftshader','--enable-unsafe-swiftshader']
  });
  try {
    const page=await browser.newPage();
    await page.setContent(fs.readFileSync(path+'.html','utf8').replace(/<script[^>]*src[^>]*><\/script>/g,''));
    // Expose closure state in this test copy; production assets stay unchanged.
    const original=fs.readFileSync(path+'.js','utf8');
    assert.ok(original.includes('\nreconnectLoop();\n'));
    const script=original.replace('\nreconnectLoop();\n',`
globalThis.flowxTest={processDatagram,stats,
  reference:()=>Array.from(restartAssembly.pixels),
  state:()=>({frame:lastRenderedFrame,shown:lastPresentedFrame,key:keyFrameId,stream:streamId}),
  snapshots:()=>playout.frames.map(frame=>{
    const slot=frame.payload,pixels=new Uint8Array(slot.width*slot.height*4);
    gl.bindFramebuffer(gl.FRAMEBUFFER,keyFbo);
    gl.framebufferTexture2D(gl.FRAMEBUFFER,gl.COLOR_ATTACHMENT0,gl.TEXTURE_2D,slot.texture,0);
    gl.readPixels(0,0,slot.width,slot.height,gl.RGBA,gl.UNSIGNED_BYTE,pixels);
    if(gl.getError()!==gl.NO_ERROR)throw new Error('snapshot read failed');
    return {frame:frame.frameId,pixels:Array.from(pixels)};
  }),
  pixels:(reference)=>{
    const w=reference?keyW:outW,h=reference?keyH:outH;
    gl.bindFramebuffer(gl.FRAMEBUFFER,reference?keyFbo:fbo[current]);
    if(reference) gl.framebufferTexture2D(gl.FRAMEBUFFER,gl.COLOR_ATTACHMENT0,gl.TEXTURE_2D,keyTex,0);
    const pixels=new Uint8Array(w*h*4);
    gl.readPixels(0,0,w,h,gl.RGBA,gl.UNSIGNED_BYTE,pixels);
    if(gl.getError()!==gl.NO_ERROR) throw new Error('WebGL read failed');
    return Array.from(pixels);
  }
};
`);
    await page.addScriptTag({content:script});
    const result=await page.evaluate(async f=>{
      const t=globalThis.flowxTest,check=(v,m)=>{if(!v)throw new Error(m);};
      const send=bytes=>t.processDatagram(Uint8Array.from(bytes));
      const patch=(frame,key=100)=>{
        const u=new Uint8Array(51),v=new DataView(u.buffer);
        v.setUint16(0,0x5846,true);u[2]=0x42;v.setUint32(4,7,true);v.setUint32(8,frame,true);
        v.setUint16(20,frame-key,true);v.setUint16(22,f.width,true);v.setUint16(24,f.height,true);
        v.setFloat32(27,1,true);v.setFloat32(43,1,true);return u;
      };
      const equal=(a,b)=>a.length===b.length&&a.every((x,i)=>x===b[i]);
      const order=f.regions.map((_,i)=>i);let seed=87;
      for(let i=order.length-1;i>0;i--) {
        seed=(Math.imul(seed,1664525)+1013904223)>>>0;
        const j=seed%(i+1);[order[i],order[j]]=[order[j],order[i]];
      }
      const kept=Math.floor(order.length*.65);
      for(const i of order.slice(0,kept)) await send(f.regions[i].wire);
      check(t.stats.keys===0,'incomplete first key was shown before burst ended');
      await send(patch(101));
      check(t.state().frame===101,'PATCH not rendered');
      check(equal(t.reference(),t.pixels(true)),'nearest-filled reference was not uploaded intact');
      const displayed=t.pixels(false),renders=t.stats.renders;
      const queued=t.snapshots().find(s=>s.frame===101);
      check(queued&&equal(queued.pixels,displayed),'queued GPU snapshot differs from rendered PATCH');
      for(const i of order.slice(kept)) await send(f.regions[i].wire);
      check(t.stats.renders===renders&&equal(displayed,t.pixels(false)),'late key rewound display');
      // Shuffled late tiles are uploaded together just before the next render.
      await send(patch(102));check(!equal(displayed,t.pixels(false)),'next PATCH did not use restored key');
      const key=t.pixels(true);
      let maxError=0;for(let i=0;i<key.length;i++) maxError=Math.max(maxError,Math.abs(key[i]-f.complete[i]));
      check(maxError<=3,'browser JPEG/RGBA differs from native: '+maxError);
      const completed=t.pixels(false);
      await send(patch(101));await send(f.regions[0].wire);
      check(t.state().frame===102&&equal(completed,t.pixels(false)),'duplicate/stale display changed');
      const rerender=t.stats.renders;
      for(const i of order.slice(0,kept)) {
        const u=Uint8Array.from(f.regions[i].wire);new DataView(u.buffer).setUint32(8,110,true);
        await send(u);
        check(t.stats.renders===rerender,'partial replacement key flashed on arrival');
      }
      check(equal(completed,t.pixels(false)),'pending key cleared the displayed image');
      await send(patch(111,110));
      check(t.stats.renders===rerender+2&&t.state().frame===111,'key/PATCH capture slots were not both rendered');
      const fresh=Uint8Array.from(f.regions[0].wire),v=new DataView(fresh.buffer);
      v.setUint32(4,8,true);v.setUint32(8,1,true);await send(fresh);
      await send(f.regions[1].wire);
      check(t.state().stream===8&&t.state().frame===null,'retired stream or first-region flash');
      for(const r of f.regions) {
        const u=Uint8Array.from(r.wire),v=new DataView(u.buffer);
        v.setUint32(4,8,true);v.setUint32(8,1,true);await send(u);
      }
      check(t.state().frame===1,'complete new-stream key was not displayed');
      return {regions:order.length,maxJpegChannelError:maxError,renders:t.stats.renders};
    },fixture);
    console.log('PASS: Chromium JPEG decode + WebGL, 35% loss, late reference updates, no display rewind, stream reset',result);
  } finally { await browser.close(); }
})().catch(e=>{console.error(e);process.exitCode=1;});
