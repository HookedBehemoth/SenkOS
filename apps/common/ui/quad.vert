#version 450
// Corners from gl_VertexIndex (4-vertex strip), the rest from the frame's quad list

struct Quad {
   vec4 xywh;   // top left and size in pixels
   vec4 uv;     // u0 v0 u1 v1
   vec4 clip;   // x0 y0 x1 y1
   uint color;  // RGBA8, straight alpha
   uint tex;    // slot; top bit: uv in texels
   float radius;
   float border;
};

layout(std430, set = 0, binding = 0) readonly buffer Quads { Quad quads[]; };
layout(push_constant) uniform PC { vec2 px_to_ndc; } pc;

layout(location = 0) out vec2 v_pos;  // from the centre, in pixels
layout(location = 1) out vec2 v_uv;
layout(location = 2) out vec4 v_color;
layout(location = 3) flat out vec4 v_clip;
layout(location = 4) flat out vec4 v_shape; // half size, radius, border
layout(location = 5) flat out uint v_tex;

void main()
{
   Quad q = quads[gl_InstanceIndex];
   vec2 corner = vec2(gl_VertexIndex & 1, gl_VertexIndex >> 1);
   vec2 p = q.xywh.xy + corner * q.xywh.zw;
   gl_Position = vec4(p * pc.px_to_ndc - 1.0, 0.0, 1.0);
   v_pos = (corner - 0.5) * q.xywh.zw;
   v_uv = mix(q.uv.xy, q.uv.zw, corner);
   vec4 c = unpackUnorm4x8(q.color);
   v_color = vec4(c.rgb * c.a, c.a);
   v_clip = q.clip;
   v_shape = vec4(q.xywh.zw * 0.5, q.radius, q.border);
   v_tex = q.tex;
}
