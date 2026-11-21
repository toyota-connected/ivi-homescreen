#version 450
// Sample the layer's source image. Blending (premultiplied src-over) is fixed
// function, set in the pipeline. An opaque layer draws alpha as 1: an XRGB
// buffer's alpha channel is undefined, and premultiplied color is unchanged.
// alpha is the view's opacity; the color is premultiplied, so it and the
// rounded clip's coverage scale all four channels.
//
// The rounded clip is in framebuffer pixels, which is where the quad is drawn,
// so gl_FragCoord is the pixel's position in it. The quad's crop has already
// put the pixel inside its bounding box: only the corners are left to cut,
// with about a pixel of edge. A zero radius is a square corner, so no clip is
// all radii zero.
//
// kMask is a specialization constant: the pipeline for a layer with no rounded
// clip is built with it false, so it pays nothing for the clip.
layout(constant_id = 0) const bool kMask = false;
layout(binding = 0) uniform sampler2D u_tex;
layout(push_constant) uniform PC {
  vec4 rect;
  vec4 uv_u;
  vec4 uv_v;
  float opaque;
  float alpha;
  vec2 edge_t;        // the vertex shader's; here only for the layout
  vec4 persp;         // likewise
  vec4 rrect;         // left, top, right, bottom
  vec4 radii_top;     // upper left x y, upper right x y
  vec4 radii_bottom;  // lower left x y, lower right x y
} pc;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

float Coverage(vec2 p) {
  bool left = p.x < 0.5 * (pc.rrect.x + pc.rrect.z);
  bool top = p.y < 0.5 * (pc.rrect.y + pc.rrect.w);
  vec4 pair = top ? pc.radii_top : pc.radii_bottom;
  vec2 r = left ? pair.xy : pair.zw;
  if (r.x <= 0.0 || r.y <= 0.0) {
    return 1.0;
  }
  vec2 d = p - vec2(left ? pc.rrect.x + r.x : pc.rrect.z - r.x,
                    top ? pc.rrect.y + r.y : pc.rrect.w - r.y);
  if ((left ? d.x >= 0.0 : d.x <= 0.0) || (top ? d.y >= 0.0 : d.y <= 0.0)) {
    return 1.0;
  }
  float k = length(d / r);
  return clamp(0.5 - (k - 1.0) * min(r.x, r.y), 0.0, 1.0);
}

void main() {
  vec4 c = texture(u_tex, v_uv);
  float coverage = kMask ? Coverage(gl_FragCoord.xy) : 1.0;
  o_color = vec4(c.rgb, mix(c.a, 1.0, pc.opaque)) * (pc.alpha * coverage);
}
