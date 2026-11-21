#version 450
// Draw one layer as a quad via a triangle strip.
// gl_VertexIndex: 0=(s,t)=(0,0) 1=(1,0) 2=(0,1) 3=(1,1).
//
// The quad's corner at (0, 0) and its edges along s and t give its position
// in clip space times w, and persp gives w, both affine in (s, t). A layer
// drawn as a rect has its edges along the axes and w = 1, so the quad is the
// rect in NDC; one of a view under a rotation, skew or mirror is a
// parallelogram, and under a perspective a quad whose w differs per corner,
// which the rasterizer then divides out per pixel, the texture coordinates
// with it.
//
// (s, t) runs over [0, 1] across the quad from that corner, and uv_u / uv_v
// map it to texture coordinates (UvAffine in view/layer_geometry.h), which is
// how a source crop and a buffer transform reach the draw. The identity
// samples the whole image upright.
layout(push_constant) uniform PC {
  vec4 rect;  // clip space times w: the corner at (0, 0), then the s edge
  vec4 uv_u;  // u = dot(uv_u.xyz, (s, t, 1))
  vec4 uv_v;  // v = dot(uv_v.xyz, (s, t, 1))
  float opaque;
  float alpha;
  vec2 edge_t;  // clip space times w: the edge along t
  vec4 persp;   // w = persp.x + s * persp.y + t * persp.z
} pc;
layout(location = 0) out vec2 v_uv;
void main() {
  float s = ((gl_VertexIndex & 1) == 0) ? 0.0 : 1.0;
  float t = ((gl_VertexIndex & 2) == 0) ? 0.0 : 1.0;
  vec3 st = vec3(s, t, 1.0);
  v_uv = vec2(dot(pc.uv_u.xyz, st), dot(pc.uv_v.xyz, st));
  float w = pc.persp.x + s * pc.persp.y + t * pc.persp.z;
  gl_Position = vec4(pc.rect.xy + s * pc.rect.zw + t * pc.edge_t, 0.0, w);
}
