#version 450
// Draw one layer as a quad via a triangle strip.
// gl_VertexIndex: 0=(s,t)=(0,0) 1=(1,0) 2=(0,1) 3=(1,1).
//
// The quad's corner at (0, 0) and its edges along s and t give its position
// in clip space times w, and w is 1 + s * ws + t * wt, both affine in (s, t)
// (the position is given over the corner's own w, which leaves that 1). A
// layer drawn as a rect has its edges along the axes and w = 1, so the quad
// is the rect in NDC; one of a view under a rotation, skew or mirror is a
// parallelogram, and under a perspective a quad whose w differs per corner,
// which the rasterizer then divides out per pixel, the texture coordinates
// with it.
//
// (s, t) runs over [0, 1] across the quad from that corner, and uv_u / uv_v
// map it to texture coordinates (UvAffine in view/layer_geometry.h), which is
// how a source crop and a buffer transform reach the draw. The identity
// samples the whole image upright. grow draws the quad past its smoothed
// edges (QuadPlacement::grow): (s, t) then runs from -grow.x to 1 + grow.y
// and -grow.z to 1 + grow.w, and the fragment shader works out how much of
// each pixel is inside.
layout(push_constant) uniform PC {
  vec4 rect;    // clip space times w: the corner at (0, 0), then the s edge
  vec4 uv_u;    // u = dot(uv_u.xyz, (s, t, 1)); w is opaque
  vec4 uv_v;    // v = dot(uv_v.xyz, (s, t, 1)); w is alpha
  vec4 edge_t;  // clip space times w: the edge along t, then ws and wt
  vec4 grow;    // past s = 0, s = 1, t = 0, t = 1
} pc;
layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec2 v_st;
void main() {
  float s = ((gl_VertexIndex & 1) == 0) ? -pc.grow.x : 1.0 + pc.grow.y;
  float t = ((gl_VertexIndex & 2) == 0) ? -pc.grow.z : 1.0 + pc.grow.w;
  vec3 st = vec3(s, t, 1.0);
  v_uv = vec2(dot(pc.uv_u.xyz, st), dot(pc.uv_v.xyz, st));
  v_st = vec2(s, t);
  float w = 1.0 + s * pc.edge_t.z + t * pc.edge_t.w;
  gl_Position = vec4(pc.rect.xy + s * pc.rect.zw + t * pc.edge_t.xy, 0.0, w);
}
