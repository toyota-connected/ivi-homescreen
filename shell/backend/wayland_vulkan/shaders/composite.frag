#version 450
// Sample the layer's source image. Blending (premultiplied src-over) is fixed
// function, set in the pipeline. An opaque layer draws alpha as 1: an XRGB
// buffer's alpha channel is undefined, and premultiplied color is unchanged.
// alpha is the view's opacity; the color is premultiplied, so it and the
// coverage scale all four channels.
//
// The coverage is the share of the pixel inside the quad's smoothed edges and
// its rounded clip. kMask is a specialization constant: the pipeline for a
// layer with neither is built with it false, so it pays nothing for them.
//
// An edge is smoothed where grow is above 0. The pixel's distance to it is
// its (s, t) over how fast that changes across the screen, which is exact
// for a parallelogram and, under a perspective, near enough over the pixel or
// so it matters for; the pixel is covered from half a pixel outside the edge
// to half inside. Past a smoothed edge, and at it, (s, t) is held half a
// texel inside it (SampleClamp in view/layer_geometry.h), so what is drawn
// there is the edge's own texels, not the buffer's beyond a crop.
//
// Built a second time with IHS_YCBCR defined, for the pipelines that sample
// through a Y'CbCr conversion, which may only be sampled: there the texture's
// size cannot be asked, and (s, t) is held at the edge itself.
//
// The rounded clip is in framebuffer pixels, which is where the quad is drawn,
// so gl_FragCoord is the pixel's position in it. The quad's crop has already
// put the pixel inside its bounding box: only the corners are left to cut,
// with about a pixel of edge. A zero radius is a square corner, so no clip is
// all radii zero.
layout(constant_id = 0) const bool kMask = false;
layout(binding = 0) uniform sampler2D u_tex;
layout(push_constant) uniform PC {
  vec4 rect;          // the vertex shader's; here only for the layout
  vec4 uv_u;          // u's row; w is opaque
  vec4 uv_v;          // v's row; w is alpha
  vec4 edge_t;        // the vertex shader's
  vec4 grow;          // past s = 0, s = 1, t = 0, t = 1; 0 is a hard edge
  vec4 rrect;         // left, top, right, bottom
  vec4 radii_top;     // upper left x y, upper right x y
  vec4 radii_bottom;  // lower left x y, lower right x y
} pc;
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec2 v_st;
layout(location = 0) out vec4 o_color;

float Round(vec2 p) {
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

float Edges() {
  // Pixels per unit of s and of t; derivatives before any branch.
  vec2 rate = vec2(length(vec2(dFdx(v_st.x), dFdy(v_st.x))),
                   length(vec2(dFdx(v_st.y), dFdy(v_st.y))));
  rate = max(rate, vec2(1e-12));
  vec4 dist = vec4(v_st.x, 1.0 - v_st.x, v_st.y, 1.0 - v_st.y) / rate.xxyy;
  vec4 c = clamp(dist + 0.5, 0.0, 1.0);
  c = mix(vec4(1.0), c, greaterThan(pc.grow, vec4(0.0)));
  return c.x * c.y * c.z * c.w;
}

void main() {
  vec2 uv = v_uv;
  float coverage = 1.0;
  if (kMask) {
    vec2 lo = vec2(0.0);
    vec2 hi = vec2(1.0);
#ifndef IHS_YCBCR
    // Texels along s and along t, and half of one in each.
    vec2 size = vec2(textureSize(u_tex, 0));
    vec2 texels = vec2(abs(pc.uv_u.x) * size.x + abs(pc.uv_v.x) * size.y,
                       abs(pc.uv_u.y) * size.x + abs(pc.uv_v.y) * size.y);
    vec2 half_texel = 0.5 / max(texels, vec2(1.0));
    lo = mix(lo, half_texel, greaterThan(pc.grow.xz, vec2(0.0)));
    hi = mix(hi, 1.0 - half_texel, greaterThan(pc.grow.yw, vec2(0.0)));
#endif
    vec3 st = vec3(clamp(v_st, lo, hi), 1.0);
    uv = vec2(dot(pc.uv_u.xyz, st), dot(pc.uv_v.xyz, st));
    coverage = Edges() * Round(gl_FragCoord.xy);
  }
  vec4 c = texture(u_tex, uv);
  o_color = vec4(c.rgb, mix(c.a, 1.0, pc.uv_u.w)) * (pc.uv_v.w * coverage);
}
