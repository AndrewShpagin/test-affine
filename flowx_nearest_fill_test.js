'use strict';
const assert=require('node:assert/strict');
const {fillMissingPixels,RestartAssembler}=require('./flowx_restart_browser.js');
function sample(width,height,mask) {
  const pixels=new Uint8Array(width*height*4),columns=width/16;
  for(let y=0;y<height;y++)for(let x=0;x<width;x++) {
    const b=(y>>3)*columns+(x>>4),valid=mask[2*b]||mask[2*b+1],p=(y*width+x)*4;
    pixels.set(valid?[x,y,(x*7+y*11)%254+1,255]:[239,1,239,255],p);
  }
  return pixels;
}
// Independent brute-force reference, including deterministic left/top ties.
function brute(pixels,received,width,height) {
  const result=pixels.slice(),sources=[],columns=width/16;
  const valid=(x,y)=>{const b=(y>>3)*columns+(x>>4);return received[2*b]||received[2*b+1];};
  for(let x=0;x<width;x++)for(let y=0;y<height;y++)if(valid(x,y))sources.push([x,y]);
  if(!sources.length)return result;
  for(let y=0;y<height;y++)for(let x=0;x<width;x++) {
    if(valid(x,y))continue;
    let distance=Infinity,source=0;
    for(const [sx,sy] of sources) {
      const d=(sx-x)**2+(sy-y)**2;
      if(d<distance){distance=d;source=(sy*width+sx)*4;}
    }
    result.set(pixels.subarray(source,source+4),(y*width+x)*4);
  }
  return result;
}
let seed=871;
const random=()=>{seed=(Math.imul(seed,1664525)+1013904223)>>>0;return seed;};
for(let trial=0;trial<16;trial++) {
  const width=64,height=32,received=new Uint8Array((width/16)*(height/8)*2);
  for(let b=0;b<received.length;b++)received[b]=Number(random()%5===0);
  received[0]=1;received[received.length-1]=0;received[received.length-2]=0;
  const original=sample(width,height,received),actual=original.slice(),mask=received.slice();
  const expected=brute(original,received,width,height);
  assert.equal(fillMissingPixels(actual,received,width,height),true);
  assert.deepEqual(actual,expected,'nearest color differs from Euclidean brute force');
  assert.deepEqual(received,mask,'estimated pixels changed actual receipt mask');
  // Repeat with existing estimates in the holes; they must not become sources.
  assert.equal(fillMissingPixels(actual,received,width,height),true);
  assert.deepEqual(actual,expected,'fill propagated an estimate instead of a valid pixel');
}
// Genuine black is usable source data, not another hole.
const black=new Uint8Array(32*16*4),one=new Uint8Array(8);one[0]=1;
for(let p=3;p<black.length;p+=4)black[p]=255;
assert.equal(fillMissingPixels(black,one,32,16),true);
for(let p=0;p<black.length;p++)assert.equal(black[p],p%4===3?255:0);
// No sources: preserve the buffer. Fully usable pairs: no fill needed.
const empty=new Uint8Array(8),untouched=sample(32,16,empty),copy=untouched.slice();
assert.equal(fillMissingPixels(untouched,empty,32,16),false);assert.deepEqual(untouched,copy);
const full=Uint8Array.from([1,0,0,1,1,1,0,1]),complete=sample(32,16,full),frozen=complete.slice();
assert.equal(fillMissingPixels(complete,full,32,16),false);assert.deepEqual(complete,frozen);
assert.equal(new RestartAssembler({},()=>{}).fillMissing(),false);
console.log('PASS: exact nearest-pixel colors, edges/diagonals/ties, valid black, receipt masks, repeated fill, empty/full frames');
