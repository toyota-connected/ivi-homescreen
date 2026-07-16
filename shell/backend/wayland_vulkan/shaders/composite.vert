#version 450
// Draw one layer as a quad at a destination rect (in NDC) via a triangle strip.
// gl_VertexIndex: 0=(x0,y0) 1=(x1,y0) 2=(x0,y1) 3=(x1,y1).
layout(push_constant) uniform PC {
  vec4 rect;  // NDC x0, y0, x1, y1
} pc;
layout(location = 0) out vec2 v_uv;
void main() {
  float x = ((gl_VertexIndex & 1) == 0) ? pc.rect.x : pc.rect.z;
  float y = ((gl_VertexIndex & 2) == 0) ? pc.rect.y : pc.rect.w;
  v_uv = vec2(((gl_VertexIndex & 1) == 0) ? 0.0 : 1.0,
              ((gl_VertexIndex & 2) == 0) ? 0.0 : 1.0);
  gl_Position = vec4(x, y, 0.0, 1.0);
}
