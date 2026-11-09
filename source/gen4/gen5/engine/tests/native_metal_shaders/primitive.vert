#version 450
#ifdef FIXED_VERTEX_INPUT
layout(location = 0) in vec4 position;
layout(location = 1) in vec4 color;
#else
layout(std430, set = 0, binding = 0) readonly buffer Vertices { vec4 words[]; } vertices;
#endif
layout(push_constant) uniform Parameters { vec4 config; uvec4 state; } parameters;
layout(location = 0) out vec4 value;
out gl_PerVertex {
    vec4 gl_Position;
#ifdef CULL
    float gl_CullDistance[2];
#endif
};
void main() {
#ifdef FIXED_VERTEX_INPUT
    gl_Position = position;
#else
    gl_Position = vertices.words[uint(gl_VertexIndex) * 2];
#endif
    gl_Position.x = gl_Position.x * parameters.config.y + parameters.config.z;
#ifdef FIXED_VERTEX_INPUT
    value = color;
#else
    value = vertices.words[uint(gl_VertexIndex) * 2 + 1];
#endif
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
