#version 450
// Sample the layer's source image. Blending (premultiplied src-over) is fixed
// function, set in the pipeline, so this just passes the texel through.
layout(binding = 0) uniform sampler2D u_tex;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
void main() {
  o_color = texture(u_tex, v_uv);
}
