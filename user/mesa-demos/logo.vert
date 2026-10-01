// vklogo's vertex shader: the logo's mesh, lit by one light from the viewer's side
#version 450
layout(push_constant) uniform P { mat4 mvp; mat4 model; } p;
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec3 a_color;
layout(location = 0) out vec3 v_color;
void main() {
    vec3 n = normalize(mat3(p.model) * a_normal);
    float light = 0.35 + 0.65 * max(dot(n, normalize(vec3(-0.3, 0.5, 1.0))), 0.0);
    v_color = a_color * light;
    gl_Position = p.mvp * vec4(a_pos, 1.0);
}
