#version 450
layout(std430, set = 0, binding = 0) readonly buffer Vertices { vec4 words[]; } vertices;
layout(push_constant) uniform Parameters { vec4 config; uvec4 state; } parameters;
layout(location = 0) out vec4 value;
out gl_PerVertex {
    vec4 gl_Position;
#ifdef CULL
    float gl_CullDistance[2];
#endif
};
void main() {
    gl_Position = vertices.words[uint(gl_VertexIndex) * 2];
    gl_Position.x = gl_Position.x * parameters.config.y + parameters.config.z;
    value = vertices.words[uint(gl_VertexIndex) * 2 + 1];
    value.b += float(uint(gl_InstanceIndex) - parameters.state.y) * 0.125;
#ifdef CULL
    uint corner = uint(gl_VertexIndex) % 3;
    uint mode = parameters.state.x;
    gl_CullDistance[0] = mode == 0 ? -1.0 : mode == 3 ? (corner == 2 ? 0.0 : -1.0) :
        mode == 5 ? 0.0 : (corner == 2 ? 1.0 : -1.0);
    gl_CullDistance[1] = mode == 2 ? -1.0 : mode == 4 ?
        (corner == 2 ? -1.0 : 1.0) : mode == 5 ? 0.0 : 1.0;
#endif
}
