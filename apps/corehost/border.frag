#version 450
// The overlay's image, premultiplied: blended over the game
layout(set = 0, binding = 0) uniform sampler2D overlay;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;

void main() { color = texture(overlay, uv); }
