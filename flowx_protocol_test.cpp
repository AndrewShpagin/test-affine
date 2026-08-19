#include "flowx_protocol.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {
using flowx::u_char;

void u8(std::vector<u_char>& out, std::uint8_t v) { out.push_back(v); }
void u16(std::vector<u_char>& out, std::uint16_t v) {
    out.push_back(static_cast<u_char>(v & 255));
    out.push_back(static_cast<u_char>((v >> 8) & 255));
}
void u32(std::vector<u_char>& out, std::uint32_t v) {
    for (int s = 0; s < 32; s += 8) out.push_back(static_cast<u_char>((v >> s) & 255));
}
void f32(std::vector<u_char>& out, float v) {
    std::uint32_t b = 0; std::memcpy(&b, &v, 4); u32(out, b);
}
std::uint16_t get16(const std::vector<u_char>& d, std::size_t p) {
    return static_cast<std::uint16_t>(d[p]) | (static_cast<std::uint16_t>(d[p+1]) << 8);
}
std::uint32_t get32(const std::vector<u_char>& d, std::size_t p) {
    return static_cast<std::uint32_t>(d[p]) |
        (static_cast<std::uint32_t>(d[p+1]) << 8) |
        (static_cast<std::uint32_t>(d[p+2]) << 16) |
        (static_cast<std::uint32_t>(d[p+3]) << 24);
}
float getf(const std::vector<u_char>& d, std::size_t p) {
    const std::uint32_t b=get32(d,p); float v=0; std::memcpy(&v,&b,4); return v;
}

void common(std::vector<u_char>& out, std::uint8_t type, std::uint16_t bytes,
            std::uint32_t frame, std::uint32_t key, std::uint16_t w, std::uint16_t h) {
    u32(out,0x31434641u); u8(out,2); u8(out,type); u16(out,bytes);
    u32(out,frame); u32(out,key); u16(out,w); u16(out,h);
}

bool check(bool ok, const char* text) {
    if (!ok) std::cerr << "FAIL: " << text << '\n';
    return ok;
}

std::vector<u_char> makePatch(bool mesh) {
    const int gx=mesh?6:1, gy=mesh?6:1;
    const bool H=true;
    const std::size_t bytes=20+4+24+8+static_cast<std::size_t>(gx)*gy*8;
    std::vector<u_char> p; p.reserve(bytes);
    common(p,2,static_cast<std::uint16_t>(bytes),100,95,900,900);
    u8(p,gx); u8(p,gy); u16(p,H?1:0);
    const float a[6]={1.0f,0.01f,2.25f,-0.02f,1.0f,-3.5f};
    for(float v:a) f32(p,v);
    f32(p,0.00001f); f32(p,-0.00002f);
    for(int i=0;i<gx*gy;i++) {
        if (!mesh) { f32(p,0); f32(p,0); }
        else {
            f32(p, i==0 ? 1.234f : (i==gx*gy-1 ? 300.0f : i*0.05f));
            f32(p, i==0 ? -2.5f : -i*0.03f);
        }
    }
    return p;
}

std::vector<u_char> makeClassicKey() {
    const std::uint16_t n=100;
    std::vector<u_char> p; p.reserve(40+n);
    common(p,1,40,200,200,900,900);
    u16(p,450);u16(p,450);u16(p,0);u16(p,1);u32(p,n);u32(p,0);u16(p,n);u16(p,0);
    for(unsigned i=0;i<n;i++) u8(p,static_cast<std::uint8_t>(i));
    return p;
}

std::vector<u_char> makeLayerKey() {
    const std::uint16_t n=100;
    std::vector<u_char> p; p.reserve(44+n);
    common(p,3,44,300,300,900,900);
    u8(p,0);u8(p,2);u16(p,0);u16(p,225);u16(p,450);u16(p,0);u16(p,1);
    u32(p,n);u32(p,0);u16(p,n);u16(p,0);
    for(unsigned i=0;i<n;i++) u8(p,static_cast<std::uint8_t>(255-i));
    return p;
}

std::vector<u_char> makeLayerEnd() {
    std::vector<u_char> p; p.reserve(24);
    common(p,4,24,300,300,900,900);u8(p,2);u8(p,0);u16(p,0);return p;
}

bool roundTripExact(const std::vector<u_char>& afc, std::size_t expected_wire,
                    std::uint32_t stream, std::uint64_t ts) {
    std::vector<u_char> wire; std::string error;
    if (!check(flowx::wrapCodecPacket(afc,stream,ts,wire,&error),error.c_str())) return false;
    if (!check(wire.size()==expected_wire,"unexpected wire size")) return false;
    if (!check(get16(wire,0)==flowx::kFlowXMagic,"bad v4 magic")) return false;
    flowx::FlowXPacket decoded;
    if (!check(flowx::unwrapCodecPacket(wire,decoded,&error),error.c_str())) return false;
    return check(decoded.metadata.stream_id==stream,"stream id") &&
           check(decoded.metadata.capture_timestamp_us==ts,"timestamp") &&
           check(decoded.codec_packet==afc,"AFC1 round-trip differs");
}

} // namespace

int main() {
    bool ok=true;
    ok &= roundTripExact(makeClassicKey(),134,77,123456789);
    ok &= roundTripExact(makeLayerKey(),134,77,123456790);
    ok &= roundTripExact(makeLayerEnd(),24,77,123456791);

    {
        const auto afc=makePatch(false); std::vector<u_char> wire; std::string error;
        ok &= check(flowx::wrapCodecPacket(afc,77,123456792,wire,&error),error.c_str());
        ok &= check(wire.size()==59,"mesh-off H patch should be 59 bytes");
        flowx::FlowXPacket r; ok &= check(flowx::unwrapCodecPacket(wire,r,&error),error.c_str());
        ok &= check(r.codec_packet==afc,"mesh-off patch round-trip differs");
    }

    {
        const auto afc=makePatch(true); std::vector<u_char> wire; std::string error;
        ok &= check(flowx::wrapCodecPacket(afc,77,123456793,wire,&error),error.c_str());
        ok &= check(wire.size()==203,"6x6 H patch should be 203 bytes");
        flowx::FlowXPacket r; ok &= check(flowx::unwrapCodecPacket(wire,r,&error),error.c_str());
        ok &= check(r.metadata.frame_id==100 && r.metadata.keyframe_id==95,"patch frame ids");
        ok &= check(r.codec_packet.size()==afc.size(),"reconstructed patch size");
        // First mesh point begins at AFC1 offset 56. Quantization step is 1/128 px.
        const float x=getf(r.codec_packet,56), y=getf(r.codec_packet,60);
        ok &= check(std::abs(x-std::round(1.234f*128.0f)/128.0f)<1e-6f,"mesh x quantization");
        ok &= check(std::abs(y-(-2.5f))<1e-6f,"mesh y quantization");
        const std::size_t last=56+(36-1)*8;
        ok &= check(std::abs(getf(r.codec_packet,last)-255.0f)<1e-6f,"mesh clamp to +255");
    }

    if (!ok) return 1;
    std::cout << "FlowX v4 protocol test OK\n";
    return 0;
}
