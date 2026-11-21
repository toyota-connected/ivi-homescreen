#version 450
// Draw one layer as a quad via a triangle strip.
// gl_VertexIndex: 0=(s,t)=(0,0) 1=(1,0) 2=(0,1) 3=(1,1).
//
// The quad is a parallelogram in NDC: its corner at (0, 0) and its edges
// along s and t. A layer drawn as a rect has its edges along the axes; one of
// a view under a rotation, skew or mirror does not.
//
// (s, t) runs over [0, 1] across the quad from that corner, and uv_u / uv_v
// map it to texture coordinates (UvAffine in view/layer_geometry.h), which is
// how a source crop and a buffer transform reach the draw. The identity
// samples the whole image upright.
layout(push_constant) uniform PC {
  vec4 rect;  // NDC: the corner at (0, 0), then the edge along s
  vec4 uv_u;  // u = dot(uv_u.xyz, (s, t, 1))
  vec4 uv_v;  // v = dot(uv_v.xyz, (s, t, 1))
  float opaque;
  float alpha;
  vec2 edge_t;  // NDC: the edge along t
} pc;
layout(location = 0) out vec2 v_uv;
void main() {
  float s = ((gl_VertexIndex & 1) == 0) ? 0.0 : 1.0;
  float t = ((gl_VertexIndex & 2) == 0) ? 0.0 : 1.0;
  vec3 st = vec3(s, t, 1.0);
  v_uv = vec2(dot(pc.uv_u.xyz, st), dot(pc.uv_v.xyz, st));
  gl_Position = vec4(pc.rect.xy + s * pc.rect.zw + t * pc.edge_t, 0.0, 1.0);
}
