uniform float u_thr;
void main(){
  vec3 c = textureLod(u_tex, v_uv, 0.0).rgb;
  float l = max(c.r, max(c.g, c.b));
  float k = 0.15; float s = clamp(l - u_thr + k, 0.0, 2.0*k); s = s*s/(4.0*k+1e-5);
  float w = max(s, l - u_thr) / max(l, 1e-4);
  o = vec4(c * w, 1.0);
}
