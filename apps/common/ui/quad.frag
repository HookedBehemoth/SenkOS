#version 450
#extension GL_EXT_nonuniform_qualifier : require

layout(set = 0, binding = 1) uniform sampler2D textures[];

layout(location = 0) in vec2 v_pos;
layout(location = 1) in vec2 v_uv;
layout(location = 2) in vec4 v_color;
layout(location = 3) flat in vec4 v_clip;
layout(location = 4) flat in vec4 v_shape;
layout(location = 5) flat in uint v_tex;
layout(location = 0) out vec4 color;

void main()
{
   // Clipped by coverage: discard would need demote-to-helper in Vulkan 1.3 SPIR-V
   bool inside = all(greaterThanEqual(gl_FragCoord.xy, v_clip.xy)) && all(lessThan(gl_FragCoord.xy, v_clip.zw));
   // Rounded box SDF, negative inside; coverage over one pixel
   float r = min(v_shape.z, min(v_shape.x, v_shape.y));
   vec2 q = abs(v_pos) - (v_shape.xy - r);
   float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
   float cover = inside ? clamp(0.5 - d, 0.0, 1.0) : 0.0;
   if (v_shape.w > 0.0)
      cover *= clamp(0.5 + d + v_shape.w, 0.0, 1.0);
   uint slot = v_tex & 0x7fffffffu;
   vec2 uv = v_uv;
   if ((v_tex & 0x80000000u) != 0u)
      uv /= vec2(textureSize(textures[nonuniformEXT(slot)], 0));
   // Textures are premultiplied, and so is v_color
   color = texture(textures[nonuniformEXT(slot)], uv) * v_color * cover;
}
