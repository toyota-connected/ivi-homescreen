#version 450
// Draw one layer as a quad at a destination rect (in NDC) via a triangle strip.
// gl_VertexIndex: 0=(x0,y0) 1=(x1,y0) 2=(x0,y1) 3=(x1,y1).
//
// (s, t) runs over [0, 1] across the destination rect from its top-left, and
// uv_u / uv_v map it to texture coordinates (UvAffine in view/layer_geometry.h),
// which is how a source crop and a buffer transform reach the draw. The
// identity samples the whole image upright.
layout(push_constant) uniform PC {
  vec4 rect;  // NDC x0, y0, x1, y1
  vec4 uv_u;  // u = dot(uv_u.xyz, (s, t, 1))
  vec4 uv_v;  // v = dot(uv_v.xyz, (s, t, 1))
  float opaque;
} pc;
layout(location = 0) out vec2 v_uv;
void main() {
  float s = ((gl_VertexIndex & 1) == 0) ? 0.0 : 1.0;
  float t = ((gl_VertexIndex & 2) == 0) ? 0.0 : 1.0;
  vec3 st = vec3(s, t, 1.0);
  v_uv = vec2(dot(pc.uv_u.xyz, st), dot(pc.uv_v.xyz, st));
  gl_Position = vec4(mix(pc.rect.x, pc.rect.z, s), mix(pc.rect.y, pc.rect.w, t),
                     0.0, 1.0);
}
