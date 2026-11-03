uniform float u_strength;
void main(){
  o = vec4(clamp(textureLod(u_bloom, v_uv, 0.0).rgb * u_strength, 0.0, 1.0), 0.0);
}
