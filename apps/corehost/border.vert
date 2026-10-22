#version 450
// The overlay over the window: the shaders' quad as it is
layout(location = 0) in vec4 Position;
layout(location = 1) in vec2 TexCoord;
layout(location = 0) out vec2 uv;

void main() {
  gl_Position = Position;
  uv = TexCoord;
}
