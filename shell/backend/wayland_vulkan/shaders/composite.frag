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
// Built with IHS_CLIPS defined (with and without IHS_YCBCR), for a view whose
// clips are drawn as shapes (ClipShapes in view/layer_geometry.h): up to four
// rounded rects, each in its own space, with the map from framebuffer pixels
// to that space, in a uniform buffer at set 1. Their share of the pixel is
// worked out as ClipShapeDistance does. The shares of the quad's edges
// multiply, which is right at its corners; a clip's is taken as the least of
// it and theirs, which is right where the clip runs along an edge of the view,
// as a clip around the view does, and near enough where one crosses it.
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

#ifdef IHS_CLIPS
layout(set = 1, binding = 0) uniform Clips {
  ivec4 count;    // x: how many
  vec4 map[12];   // per clip: its x, y and w rows (xyz); first w a pixel's span
  vec4 rect[4];   // left, top, right, bottom
  vec4 radii[8];  // per clip: as radii_top, then radii_bottom
} u_clips;

float Shapes(vec2 px) {
  float cover = 1.0;
  vec3 p = vec3(px, 1.0);
  for (int k = 0; k < 4; ++k) {
    if (k >= u_clips.count.x) {
      break;
    }
    vec3 mx = u_clips.map[3 * k].xyz;
    vec3 my = u_clips.map[3 * k + 1].xyz;
    vec3 mw = u_clips.map[3 * k + 2].xyz;
    float w = dot(mw, p);
    if (w <= 0.0) {
      return 0.0;
    }
    vec2 q = vec2(dot(mx, p), dot(my, p)) / w;
    vec4 b = u_clips.rect[k];
    vec4 top2 = u_clips.radii[2 * k];
    vec4 bottom2 = u_clips.radii[2 * k + 1];
    // More than a pixel inside the straight parts: wholly inside.
    float m = u_clips.map[3 * k].w;
    vec2 lo = b.xy + m;
    vec2 hi = b.zw - m;
    vec2 lo_r = lo + vec2(max(top2.x, bottom2.x), max(top2.y, top2.w));
    vec2 hi_r = hi - vec2(max(top2.z, bottom2.z), max(bottom2.y, bottom2.w));
    if ((q.x > lo.x && q.x < hi.x && q.y > lo_r.y && q.y < hi_r.y) ||
        (q.y > lo.y && q.y < hi.y && q.x > lo_r.x && q.x < hi_r.x)) {
      continue;
    }
    vec2 jx = (vec2(mx.x, my.x) - q * mw.x) / w;
    vec2 jy = (vec2(mx.y, my.y) - q * mw.y) / w;
    bool left = q.x < 0.5 * (b.x + b.z);
    bool top = q.y < 0.5 * (b.y + b.w);
    vec4 pair = top ? top2 : bottom2;
    vec2 r = left ? pair.xy : pair.zw;
    vec2 d = q - vec2(left ? b.x + r.x : b.z - r.x,
                      top ? b.y + r.y : b.w - r.y);
    float f;
    vec2 n;
    if (r.x > 0.0 && r.y > 0.0 && (left ? d.x < 0.0 : d.x > 0.0) &&
        (top ? d.y < 0.0 : d.y > 0.0)) {
      float e = length(d / r);
      f = e - 1.0;
      n = d / (r * r * e);
    } else {
      vec2 e = abs(q - 0.5 * (b.xy + b.zw)) - 0.5 * (b.zw - b.xy);
      if (e.x > e.y) {
        f = e.x;
        n = vec2(left ? -1.0 : 1.0, 0.0);
      } else {
        f = e.y;
        n = vec2(0.0, top ? -1.0 : 1.0);
      }
    }
    float rate = max(length(vec2(dot(jx, n), dot(jy, n))), 1e-6);
    cover = min(cover, clamp(0.5 - f / rate, 0.0, 1.0));
    if (cover <= 0.0) {
      break;
    }
  }
  return cover;
}
#else
float Shapes(vec2 px) {
  return 1.0;
}
#endif

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
    // Outside the quad's edges nothing is left to cut.
    coverage = Edges();
    if (coverage > 0.0) {
      coverage = min(coverage,
                     min(Round(gl_FragCoord.xy), Shapes(gl_FragCoord.xy)));
    }
  }
  vec4 c = texture(u_tex, uv);
  o_color = vec4(c.rgb, mix(c.a, 1.0, pc.uv_u.w)) * (pc.uv_v.w * coverage);
}
