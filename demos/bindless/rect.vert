#version 450
// Rect corners come from gl_VertexIndex (4-vertex strip), everything else from the instance SSBO

struct Rect {
   vec4 xywh;   // center and size in pixels
   vec4 uv;     // u0 v0 u1 v1
   uint img;
   uint tint;   // RGBA8
   float t_offset;
   float speed; // negative spins backwards
};

layout(std430, set = 0, binding = 0) readonly buffer Rects { Rect rects[]; };
// One time per swapchain image: its command buffer is recorded once and resubmitted
layout(std430, set = 0, binding = 2) readonly buffer Times { float times[]; };
layout(push_constant) uniform PC { vec2 px_to_ndc; uint slot; } pc;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_tint;
layout(location = 2) flat out uint v_img;

void main()
{
   Rect r = rects[gl_InstanceIndex];
   vec2 corner = vec2(gl_VertexIndex & 1, gl_VertexIndex >> 1);
   float phase = times[pc.slot] * r.speed + r.t_offset;
   vec2 p = (corner - 0.5) * r.xywh.zw * (0.75 + 0.25 * sin(2.0 * phase));
   float c = cos(phase), s = sin(phase);
   p = mat2(c, s, -s, c) * p + r.xywh.xy;
   gl_Position = vec4(p * pc.px_to_ndc - 1.0, 0.0, 1.0);
   v_uv = mix(r.uv.xy, r.uv.zw, corner);
   v_tint = unpackUnorm4x8(r.tint);
   v_img = r.img;
}
