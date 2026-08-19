# FlowX v4 migration notes

FlowX v4 is a breaking UDP wire-format change. Sender and receiver must be updated together.

The stable AFC1 codec representation remains inside the C++ process only. On send, AFC1 is compacted to v4; on receive, v4 is expanded back to AFC1 for the existing native decoder. The browser parses v4 directly.

Main savings:

- one 16-bit `FX` magic instead of outer + inner 32-bit magics;
- no wire `reserved`, `header_bytes`, or `payload_bytes` fields;
- no duplicated absolute `keyframe_id` on patches; v4 sends a 16-bit keyframe age;
- mesh vectors are signed 16-bit fixed point at 1/128 px instead of float32;
- mesh-disabled patches send no mesh payload;
- keyframe chunk offset and chunk byte count are inferred rather than transmitted.

A 6x6 homography patch is 203 bytes on v4 versus about 376 bytes with the previous v3+AFC1 envelope. A homography patch without mesh is 59 bytes.

One 24-byte layered-keyframe end marker is retained for immediate compatibility with the stable native layered-keyframe decoder. It occurs once per STRIPS/MOSAIC keyframe, so its bandwidth cost is negligible compared with the encoded keyframe data; removing it can be considered separately after the v4 path is validated.
