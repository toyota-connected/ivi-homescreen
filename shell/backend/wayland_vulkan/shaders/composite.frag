#version 450
// Sample the layer's source image. Blending (premultiplied src-over) is fixed
// function, set in the pipeline. An opaque layer draws alpha as 1: an XRGB
// buffer's alpha channel is undefined, and premultiplied color is unchanged.
// alpha is the view's opacity; the color is premultiplied, so it scales all
// four channels.
layout(binding = 0) uniform sampler2D u_tex;
layout(push_constant) uniform PC {
  vec4 rect;
  vec4 uv_u;
  vec4 uv_v;
  float opaque;
  float alpha;
} pc;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
void main() {
  vec4 c = texture(u_tex, v_uv);
  o_color = vec4(c.rgb, mix(c.a, 1.0, pc.opaque)) * pc.alpha;
}
