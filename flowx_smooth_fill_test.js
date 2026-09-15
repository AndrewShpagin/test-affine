'use strict';
// CPU reference for the one-pass GPU kernel, plus geometry/mask invariants.
const assert=require('node:assert/strict'),fs=require('node:fs');
const {fillMissingPixels}=require('./flowx_restart_browser.js');
function smoothReference(pixels,params,width,height) {
  const out=pixels.slice(),q=Math.SQRT1_2;
  const taps=[[0,0,4],[-.5,0,2],[.5,0,2],[0,-.5,2],[0,.5,2],
    [-1,0,1],[1,0,1],[0,-1,1],[0,1,1],[-q,-q,1],[-q,q,1],[q,-q,1],[q,q,1]];
  function sample(x,y,c) {
    x=Math.max(0,Math.min(width-1,x));y=Math.max(0,Math.min(height-1,y));
    const x0=Math.floor(x),y0=Math.floor(y),x1=Math.min(x0+1,width-1),y1=Math.min(y0+1,height-1);
    const a=x-x0,b=y-y0;
    return (1-b)*((1-a)*pixels[(y0*width+x0)*4+c]+a*pixels[(y0*width+x1)*4+c])
      +b*((1-a)*pixels[(y1*width+x0)*4+c]+a*pixels[(y1*width+x1)*4+c]);
  }
  for(let y=0;y<height;y++)for(let x=0;x<width;x++) {
    const i=y*width+x,t=params[2*i+1]/255,r=params[2*i]*3/255;
    if(!t)continue;
    for(let c=0;c<3;c++) {
      const blurred=taps.reduce((sum,[dx,dy,w])=>sum+w*sample(x+r*dx,y+r*dy,c),0)/20;
      out[4*i+c]=Math.round(pixels[4*i+c]*(1-t)+blurred*t);
    }
  }
  return out;
}
function fixture(name,holes,width=64,height=32,constant=false) {
  const received=new Uint8Array((width/16)*(height/8)*2);received.fill(1);
  for(const [bx,by] of holes)received.fill(0,2*(by*(width/16)+bx),2*(by*(width/16)+bx)+2);
  const pixels=new Uint8Array(width*height*4),params=new Uint8Array(width*height*2);
  for(let y=0;y<height;y++)for(let x=0;x<width;x++) {
    const b=(y>>3)*(width/16)+(x>>4),i=(y*width+x)*4;
    const color=constant?[63,97,151,255]:x<16?[40,220,30,255]:x>=48?[230,210,40,255]:y<12?[240,30,40,255]:[30,40,240,255];
    pixels.set(received[2*b]?color:[0,0,0,255],i);
  }
  const original=pixels.slice(),mask=received.slice();
  fillMissingPixels(pixels,received,width,height,params);
  const expected=smoothReference(pixels,params,width,height);
  assert.deepEqual(received,mask,'smoothing changed receipt masks');
  for(let y=0;y<height;y++)for(let x=0;x<width;x++) {
    const b=(y>>3)*(width/16)+(x>>4),i=y*width+x;
    if(received[2*b]) {
      assert.equal(params[2*i+1],0,'known pixel has smoothing weight');
      assert.deepEqual(expected.subarray(4*i,4*i+4),original.subarray(4*i,4*i+4));
    }
    assert.equal(expected[4*i+3],255);
    if(!params[2*i+1])assert.deepEqual(expected.subarray(4*i,4*i+4),pixels.subarray(4*i,4*i+4));
  }
  // Recomputing parameters/pixels from real sources must not accumulate blur.
  const repeat=expected.slice(),again=params.slice();
  fillMissingPixels(repeat,received,width,height,again);
  assert.deepEqual(repeat,pixels);assert.deepEqual(again,params);
  return {name,width,height,pixels:Array.from(pixels),params:Array.from(params),expected:Array.from(expected)};
}
const cases=[fixture('isolated',[[1,1]]),fixture('joined_horizontal',[[1,1],[2,1]]),
  fixture('joined_vertical',[[1,1],[1,2]]),fixture('corner',[[0,0],[1,0],[0,1]]),
  fixture('constant',[[1,1],[2,1]],64,32,true),fixture('complete',[])];
const isolated=cases[0],w=isolated.width,at=(f,x,y)=>f.params[2*(y*f.width+x)+1];
for(let y=8;y<16;y++)for(let x=16;x<32;x++) {
  if(x===16||x===31||y===8||y===15)assert.equal(at(isolated,x,y),0,'isolated boundary blurred');
}
assert.equal(at(isolated,23,11),255,'center lacks full blend');
assert.equal(isolated.params[2*(11*w+23)],255,'center radius is not 3 pixels');
const joined=cases[1];
assert.equal(at(joined,31,11),255);assert.equal(at(joined,32,11),255,'internal tile seam disabled smoothing');
const vertical=cases[2];
assert.equal(at(vertical,23,15),255);assert.equal(at(vertical,23,16),255);
assert.deepEqual(cases[4].expected,cases[4].pixels,'constant color changed');
assert.ok(isolated.expected.some((v,i)=>v!==isolated.pixels[i]),'filter had no effect');
// The strong top/bottom NN discontinuity through the center must be softened.
function jump(a,x,y){return Math.abs(a[4*(y*w+x)]-a[4*((y+1)*w+x)]);}
assert.ok(jump(isolated.expected,23,11)<jump(isolated.pixels,23,11)*.7,'NN center seam not reduced');
// No usable pixels: no invented colors or smoothing parameters.
const empty=new Uint8Array(16*8*4),zero=new Uint8Array(2),params=new Uint8Array(16*8*2);params.fill(255);
assert.equal(fillMissingPixels(empty,zero,16,8,params),false);assert.ok(params.every(v=>v===0));
// A late counterpart makes its whole paired tile usable, clearing old weights.
const mask=new Uint8Array(16);mask.fill(1);mask[10]=mask[11]=0;
const late=new Uint8Array(32*32*4),controls=new Uint8Array(32*32*2);
fillMissingPixels(late,mask,32,32,controls);mask[10]=1;
assert.equal(fillMissingPixels(late,mask,32,32,controls),false);assert.ok(controls.every(v=>v===0));
if(process.argv[2]==='--export')fs.writeFileSync(process.argv[3],JSON.stringify(cases));
console.log('PASS: adaptive fill, fixed borders, joined tiles, constant color, seam reduction, late masks, immutable NN source');
module.exports={smoothReference};
