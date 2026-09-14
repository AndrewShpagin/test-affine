// Shared, testable JPEG region assembly. The WebGL view owns presentation;
// accepting late key data updates this reference without replaying old frames.
(function(root) {
  'use strict';
  const newer = (a,b) => ((a-b)|0)>0;
  function parseRegion(u) {
    if(u.length<=36 || u.length>1300) throw new Error('bad JPEG region size');
    const v=new DataView(u.buffer,u.byteOffset,u.byteLength),get=p=>v.getUint16(p,true);
    if(get(0)!==0x5846 || u[2]!==0x44 || u[3] || !v.getUint32(4,true)) throw new Error('bad JPEG region header');
    const r={frameId:v.getUint32(8,true),ow:get(20),oh:get(22),lw:get(24),lh:get(26),x:get(28),y:get(30),width:get(32),profile:u[34],entropy:u.slice(36)};
    if(u[35] || r.ow<16 || (r.ow&1) || r.oh<8 || r.ow*r.oh>16*1024*1024 ||
       r.lw<8 || r.lh<8 || r.lw%8 || r.lh%8 || 2*r.lw>r.ow || r.lh>r.oh ||
       r.width<8 || r.width%8 || ((r.x>>1)%8) || r.y%8 ||
       (r.x>>1)+r.width>r.lw || r.y+8>r.lh || (r.profile!==1 && r.profile!==2))
      throw new Error('bad JPEG region geometry/profile');
    for(let p=0;p<r.entropy.length;p++)
      if(r.entropy[p]===255 && (++p===r.entropy.length || r.entropy[p]!==0)) throw new Error('unexpected JPEG marker');
    return r;
  }
  function makeJpeg(r,headers) {
    const source=headers[r.profile];
    if(!source) throw new Error('unknown fixed JPEG profile');
    const header=Uint8Array.from(source);
    let found=false;
    for(let p=2;p+4<=header.length;) {
      const n=(header[p+2]<<8)|header[p+3];
      if(header[p]!==255 || n<2 || p+2+n>header.length) throw new Error('invalid local JPEG template');
      if(header[p+1]===0xc0) {
        header[p+5]=0;header[p+6]=8;header[p+7]=r.width>>8;header[p+8]=r.width&255;found=true;break;
      }
      p+=2+n;
    }
    if(!found) throw new Error('local JPEG template lacks SOF0');
    const jpeg=new Uint8Array(header.length+r.entropy.length+2);
    jpeg.set(header);jpeg.set(r.entropy,header.length);jpeg.set([255,217],jpeg.length-2);
    return jpeg;
  }
  class RestartAssembler {
    constructor(headers,decode) { this.headers=headers;this.decode=decode;this.frameId=null; }
    matching(r) { return this.ow===r.ow && this.oh===r.oh && this.lw===r.lw && this.lh===r.lh && this.profile===r.profile; }
    async accept(u) {
      const r=parseRegion(u);
      const columns=r.lw/8,first=(r.y/8)*columns+(r.x>>4),count=r.width/8,parity=r.x&1;
      if(this.frameId!==null) {
        if(r.frameId!==this.frameId && !newer(r.frameId,this.frameId)) return null;
        if(r.frameId===this.frameId) {
          if(!this.matching(r)) throw new Error('JPEG region metadata changed');
          let missing=false;
          for(let i=0;i<count;i++) missing=missing || !this.received[2*(first+i)+parity];
          if(!missing) return null;
        }
      }
      const rgba=await this.decode(makeJpeg(r,this.headers),r.width,8);
      if(!rgba || rgba.length!==r.width*8*4) throw new Error('JPEG region decode size mismatch');
      // A newer key may have completed while an asynchronous decode was pending.
      if(this.frameId!==null && r.frameId!==this.frameId && !newer(r.frameId,this.frameId)) return null;
      const newKeyframe=this.frameId!==r.frameId;
      if(newKeyframe) {
        this.frameId=r.frameId;this.ow=r.ow;this.oh=r.oh;this.lw=r.lw;this.lh=r.lh;this.profile=r.profile;
        this.width=r.lw*2;this.height=r.lh;
        this.received=new Uint8Array(2*columns*(r.lh/8));
        this.receivedCount=0;
        this.pixels=new Uint8Array(this.width*this.height*4);
        for(let p=3;p<this.pixels.length;p+=4) this.pixels[p]=255;
      } else if(!this.matching(r)) throw new Error('JPEG region metadata changed during decode');
      let changed=false;
      for(let b=0;b<count;b++) {
        const own=2*(first+b)+parity;
        if(this.received[own]) continue;
        const fill=!this.received[own^1];
        for(let y=0;y<8;y++) for(let x=0;x<8;x++) {
          const sx=8*b+x,dx=r.x+2*sx,src=(y*r.width+sx)*4,dst=((r.y+y)*this.width+dx)*4;
          for(let c=0;c<4;c++) {
            this.pixels[dst+c]=rgba[src+c];
            if(fill) this.pixels[((r.y+y)*this.width+(dx^1))*4+c]=rgba[src+c];
          }
        }
        this.received[own]=1;this.receivedCount++;changed=true;
      }
      if(!changed) return null;
      const x=r.x&~1,width=2*r.width,patch=new Uint8Array(width*8*4);
      for(let y=0;y<8;y++) {
        const begin=((r.y+y)*this.width+x)*4;
        patch.set(this.pixels.subarray(begin,begin+width*4),y*width*4);
      }
      return {newKeyframe,x,y:r.y,width,height:8,pixels:patch};
    }
  }
  root.FlowXRestartAssembler=RestartAssembler;
  if(typeof module!=='undefined' && module.exports) module.exports={RestartAssembler,parseRegion,makeJpeg};
})(globalThis);
