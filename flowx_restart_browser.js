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
  function fillMissingPixels(pixels,received,width,height) {
    const columns=width/16,rows=height/8;
    if(!Number.isInteger(columns)||!Number.isInteger(rows)||columns<1||rows<1||
       pixels.length!==width*height*4||received.length!==columns*rows*2)
      throw new Error('bad nearest-fill dimensions');
    // A received half-block makes both interleaved columns usable through the
    // existing counterpart copy. Neither that copy nor this fill changes the
    // received mask. Pixel color, including genuine black, is not a validity flag.
    const valid=new Uint8Array(columns*rows);
    let known=0;
    for(let b=0;b<valid.length;b++) {
      valid[b]=Number(Boolean(received[2*b]||received[2*b+1]));known+=valid[b];
    }
    if(known===0||known===valid.length)return false;

    // Vertical nearest-source coordinates are identical for all 16 columns
    // in a paired block. This needs 1/16 of a full per-pixel source map.
    const nearestY=new Int32Array(columns*height);
    for(let bx=0;bx<columns;bx++) {
      let above=-1;
      for(let y=0;y<height;y++) {
        if(valid[(y>>3)*columns+bx])above=y;
        nearestY[y*columns+bx]=above;
      }
      let below=-1;
      for(let y=height-1;y>=0;y--) {
        if(valid[(y>>3)*columns+bx])below=y;
        const i=y*columns+bx,prior=nearestY[i];
        if(below>=0&&(prior<0||below-y<y-prior))nearestY[i]=below;
      }
    }
    // For each row, minimize (x-sourceX)^2 + (y-sourceY)^2 using the
    // lower envelope of parabolas. Exact Euclidean nearest pixel, O(W*H).
    const sites=new Int32Array(width),edges=new Float64Array(width+1),cost=new Float64Array(width);
    for(let y=0;y<height;y++) {
      let last=-1;
      for(let x=0;x<width;x++) {
        const sy=nearestY[y*columns+(x>>4)];
        if(sy<0)continue;
        cost[x]=(y-sy)*(y-sy);
        let boundary=-Infinity;
        while(last>=0) {
          const p=sites[last];
          boundary=(cost[x]+x*x-cost[p]-p*p)/(2*(x-p));
          if(boundary>edges[last])break;
          last--;
        }
        last++;sites[last]=x;edges[last]=last===0?-Infinity:boundary;edges[last+1]=Infinity;
      }
      let site=0;
      for(let x=0;x<width;x++) {
        if(valid[(y>>3)*columns+(x>>4)])continue;
        // Strict comparison resolves equal-distance ties toward the left.
        while(site<last&&edges[site+1]<x)site++;
        const sx=sites[site],sy=nearestY[y*columns+(sx>>4)];
        const from=(sy*width+sx)*4,to=(y*width+x)*4;
        for(let c=0;c<4;c++)pixels[to+c]=pixels[from+c];
      }
    }
    return true;
  }
  class RestartAssembler {
    constructor(headers,decode) { this.headers=headers;this.decode=decode;this.frameId=null;this.fillDirty=false; }
    matching(r) { return this.ow===r.ow && this.oh===r.oh && this.lw===r.lw && this.lh===r.lh && this.profile===r.profile; }
    fillMissing() {
      if(this.frameId===null||!this.fillDirty)return false;
      const changed=fillMissingPixels(this.pixels,this.received,this.width,this.height);
      this.fillDirty=false;return changed;
    }
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
      this.fillDirty=true;
      const x=r.x&~1,width=2*r.width,patch=new Uint8Array(width*8*4);
      for(let y=0;y<8;y++) {
        const begin=((r.y+y)*this.width+x)*4;
        patch.set(this.pixels.subarray(begin,begin+width*4),y*width*4);
      }
      return {newKeyframe,x,y:r.y,width,height:8,pixels:patch};
    }
  }
  root.FlowXRestartAssembler=RestartAssembler;
  if(typeof module!=='undefined' && module.exports) module.exports={RestartAssembler,parseRegion,makeJpeg,fillMissingPixels};
})(globalThis);
