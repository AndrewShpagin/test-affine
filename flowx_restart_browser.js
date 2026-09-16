// Shared, testable JPEG region assembly. The WebGL view owns presentation;
// accepting late key data updates this reference without replaying old frames.
(function(root) {
  'use strict';
  const newer = (a,b) => ((a-b)|0)>0;
  function parseRegion(u) {
    if(u.length<=36 || u.length>1300) throw new Error('bad JPEG region size');
    const v=new DataView(u.buffer,u.byteOffset,u.byteLength),get=p=>v.getUint16(p,true);
    if(get(0)!==0x5846 || u[2]!==0x44 || u[3] || !v.getUint32(4,true)) throw new Error('bad JPEG region header');
    const r={frameId:v.getUint32(8,true),ow:get(20),oh:get(22),lw:get(24),lh:get(26),x:get(28),y:get(30),width:get(32),profile:u[34],layout:u[35],entropy:u.slice(36)};
    if(r.layout>1 || r.ow<16 || (r.ow&1) || r.oh<8 || r.ow*r.oh>16*1024*1024 ||
       r.lw<8 || r.lh<8 || r.lw%8 || r.lh%8 || 2*r.lw>r.ow || r.lh>r.oh ||
       r.width<8 || r.width%8 || ((r.x>>1)%8) || r.y%8 ||
       (r.x>>1)>=r.lw || r.y+8>r.lh ||
       (r.y/8)*(r.lw/8)+(r.x>>4)+r.width/8>(r.lw/8)*(r.lh/8) ||
       (r.profile!==1 && r.profile!==2))
      throw new Error('bad JPEG region geometry/profile');
    for(let p=0;p<r.entropy.length;p++)
      if(r.entropy[p]===255 && (++p===r.entropy.length || r.entropy[p]!==0)) throw new Error('unexpected JPEG marker');
    return r;
  }
  function tilePermutation(width,height) {
    if(!Number.isInteger(width)||!Number.isInteger(height)||width<8||height<8||
       width>32760||height>65528||width%8||height%8||2*width*height>16*1024*1024)
      throw new Error('bad shuffle dimensions');
    const map=new Uint32Array((width/8)*(height/8));
    for(let i=0;i<map.length;i++)map[i]=i;
    let state=(0x46584a31^width^(height<<16))>>>0;
    for(let i=map.length-1;i>0;i--) {
      state^=state<<13;state^=state>>>17;state^=state<<5;state>>>=0;
      const j=state%(i+1),v=map[i];map[i]=map[j];map[j]=v;
    }
    return map;
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
  const smoothUnit=x=>{x=Math.max(0,Math.min(1,x));return x*x*(3-2*x);};
  function fillFaces(valid,columns,rows) {
    // Distances to actual usable blocks along each axis. Looking beyond the
    // current tile avoids suppressing the filter at internal lost-tile seams.
    const faces=new Float32Array(valid.length*4);faces.fill(Infinity);
    for(let y=0;y<rows;y++) {
      let left=-1,right=-1;
      for(let x=0;x<columns;x++) {
        const b=y*columns+x;if(valid[b])left=x;
        if(left>=0)faces[4*b]=(x-left-1)*16+1;
      }
      for(let x=columns-1;x>=0;x--) {
        const b=y*columns+x;if(valid[b])right=x;
        if(right>=0)faces[4*b+1]=(right-x)*16;
      }
    }
    for(let x=0;x<columns;x++) {
      let top=-1,bottom=-1;
      for(let y=0;y<rows;y++) {
        const b=y*columns+x;if(valid[b])top=y;
        if(top>=0)faces[4*b+2]=(y-top-1)*8+1;
      }
      for(let y=rows-1;y>=0;y--) {
        const b=y*columns+x;if(valid[b])bottom=y;
        if(bottom>=0)faces[4*b+3]=(bottom-y)*8;
      }
    }
    return faces;
  }
  function fillMissingPixels(pixels,received,width,height,smoothing=null) {
    const columns=width/16,rows=height/8;
    if(!Number.isInteger(columns)||!Number.isInteger(rows)||columns<1||rows<1||
       pixels.length!==width*height*4||received.length!==columns*rows*2)
      throw new Error('bad nearest-fill dimensions');
    if(smoothing&&smoothing.length!==width*height*2)throw new Error('bad smoothing dimensions');
    if(smoothing)smoothing.fill(0);
    // A received half-block makes both interleaved columns usable through the
    // existing counterpart copy. Neither that copy nor this fill changes the
    // received mask. Pixel color, including genuine black, is not a validity flag.
    const valid=new Uint8Array(columns*rows);
    let known=0;
    for(let b=0;b<valid.length;b++) {
      valid[b]=Number(Boolean(received[2*b]||received[2*b+1]));known+=valid[b];
    }
    if(known===0||known===valid.length)return false;
    const faces=smoothing?fillFaces(valid,columns,rows):null;

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
        if(smoothing) {
          // One-pixel border is untouched; strength reaches 1 at distance 4.
          const t=smoothUnit((Math.hypot(x-sx,y-sy)-1)/3);
          const b=((y>>3)*columns+(x>>4))*4,u=x&15,v=y&7;
          let first=Infinity,second=Infinity;
          for(let side=0;side<4;side++) {
            const d=faces[b+side]+(side===0?u:side===1?-u:side===2?v:-v);
            if(d<first){second=first;first=d;}else if(d<second)second=d;
          }
          const seam=Number.isFinite(second)?1-smoothUnit((second-first-1)/1.5):1;
          // RG8 stores radius/3 and blend strength for the single GPU pass.
          smoothing[(to>>2)*2]=Math.round(255*t*(0.5+0.5*seam));
          smoothing[(to>>2)*2+1]=Math.round(255*t);
        }
      }
    }
    return true;
  }
  class RestartAssembler {
    constructor(headers,decode,{fillGaps=true,smoothFills=true}={}) {
      this.headers=headers;this.decode=decode;this.fillGaps=fillGaps;this.frameId=null;this.fillDirty=false;
      this.smoothFills=smoothFills;this.smoothing=null;
    }
    matching(r) { return this.ow===r.ow && this.oh===r.oh && this.lw===r.lw && this.lh===r.lh && this.profile===r.profile && this.layout===r.layout; }
    spatial(index) { return this.tileMap?this.tileMap[index]:index; }
    fillMissing() {
      if(!this.fillGaps||this.frameId===null||!this.fillDirty)return false;
      if(this.smoothFills&&!this.smoothing)this.smoothing=new Uint8Array(this.width*this.height*2);
      const changed=fillMissingPixels(this.pixels,this.received,this.width,this.height,this.smoothing);
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
          for(let i=0;i<count;i++) missing=missing || !this.received[2*this.spatial(first+i)+parity];
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
        this.layout=r.layout;this.tileMap=r.layout===1?tilePermutation(r.lw,r.lh):null;
        this.width=r.lw*2;this.height=r.lh;
        this.smoothing=null;
        this.received=new Uint8Array(2*columns*(r.lh/8));
        this.receivedCount=0;
        this.pixels=new Uint8Array(this.width*this.height*4);
        for(let p=3;p<this.pixels.length;p+=4) this.pixels[p]=255;
      } else if(!this.matching(r)) throw new Error('JPEG region metadata changed during decode');
      let changed=false;
      for(let b=0;b<count;b++) {
        const target=this.spatial(first+b),own=2*target+parity;
        const tx=(target%columns)*16+parity,ty=Math.floor(target/columns)*8;
        if(this.received[own]) continue;
        const fill=this.fillGaps&&!this.received[own^1];
        for(let y=0;y<8;y++) for(let x=0;x<8;x++) {
          const sx=8*b+x,dx=tx+2*x,src=(y*r.width+sx)*4,dst=((ty+y)*this.width+dx)*4;
          for(let c=0;c<4;c++) {
            this.pixels[dst+c]=rgba[src+c];
            if(fill) this.pixels[((ty+y)*this.width+(dx^1))*4+c]=rgba[src+c];
          }
        }
        this.received[own]=1;this.receivedCount++;changed=true;
      }
      if(!changed) return null;
      this.fillDirty=true;
      // Shuffled and cross-row regions are not single horizontal rectangles.
      // Batch their GPU upload until rendering instead of uploading per tile.
      if(this.layout===1||(r.x>>1)+r.width>r.lw)return {newKeyframe,fullUpload:true};
      const x=r.x&~1,width=2*r.width,patch=new Uint8Array(width*8*4);
      for(let y=0;y<8;y++) {
        const begin=((r.y+y)*this.width+x)*4;
        patch.set(this.pixels.subarray(begin,begin+width*4),y*width*4);
      }
      return {newKeyframe,x,y:r.y,width,height:8,pixels:patch};
    }
  }
  root.FlowXRestartAssembler=RestartAssembler;
  if(typeof module!=='undefined' && module.exports) module.exports={RestartAssembler,parseRegion,makeJpeg,fillMissingPixels,tilePermutation};
})(globalThis);
