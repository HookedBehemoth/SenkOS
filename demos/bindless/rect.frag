#version 450
#extension GL_EXT_nonuniform_qualifier : require

layout(set = 0, binding = 1) uniform sampler2D textures[];

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_tint;
layout(location = 2) flat in uint v_img;
layout(location = 0) out vec4 color;

void main()
{
   color = texture(textures[nonuniformEXT(v_img)], v_uv) * v_tint;
}
