#version 450
#extension GL_EXT_nonuniform_qualifier : require

layout(set = 0, binding = 1) uniform sampler2D textures[];

// The pipeline's (gfx.c, a draw per run of one kind): a Mali-G31 pays for every
// instruction on every pixel, so each kind does only its own work. Clipping is
// the draw's scissor.
layout(constant_id = 0) const int KIND = 0; // 0: a box, 1: a textured box, 2: rounded or a border

layout(location = 0) in vec2 v_pos;
layout(location = 1) in vec2 v_uv;
layout(location = 2) in vec4 v_color;
layout(location = 4) flat in vec4 v_shape;
layout(location = 5) flat in uint v_tex;
layout(location = 0) out vec4 color;

void main()
{
   // Textures are premultiplied, and so is v_color
   color = v_color;
   if (KIND != 0)
      color *= texture(textures[nonuniformEXT(v_tex)], v_uv);
   if (KIND == 2) {
      // Rounded box SDF, negative inside; coverage over one pixel
      float r = min(v_shape.z, min(v_shape.x, v_shape.y));
      vec2 q = abs(v_pos) - (v_shape.xy - r);
      float d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
      float cover = clamp(0.5 - d, 0.0, 1.0);
      if (v_shape.w > 0.0)
         cover *= clamp(0.5 + d + v_shape.w, 0.0, 1.0);
      color *= cover;
   }
}
