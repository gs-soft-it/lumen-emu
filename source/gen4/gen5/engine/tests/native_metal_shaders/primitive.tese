#version 450
#ifdef QUADS
layout(quads, equal_spacing, cw) in;
#elif defined(FRACTIONAL_ODD)
layout(triangles, fractional_odd_spacing, cw) in;
#elif defined(FRACTIONAL_EVEN)
layout(triangles, fractional_even_spacing, cw) in;
#else
layout(triangles, equal_spacing, cw) in;
#endif
layout(location = 0) in vec4 input_value[];
layout(location = 0) out vec4 value;
layout(push_constant) uniform Parameters { vec4 config; uvec4 state; } parameters;
out gl_PerVertex {
    vec4 gl_Position;
#ifdef CULL
    float gl_CullDistance[2];
#endif
};
void main() {
#ifdef QUADS
    gl_Position = mix(mix(gl_in[0].gl_Position, gl_in[1].gl_Position, gl_TessCoord.x),
                      mix(gl_in[2].gl_Position, gl_in[3].gl_Position, gl_TessCoord.x), gl_TessCoord.y);
    value = mix(mix(input_value[0], input_value[1], gl_TessCoord.x),
                mix(input_value[2], input_value[3], gl_TessCoord.x), gl_TessCoord.y);
#else
    gl_Position = gl_TessCoord.x * gl_in[0].gl_Position +
                  gl_TessCoord.y * gl_in[1].gl_Position + gl_TessCoord.z * gl_in[2].gl_Position;
    value = gl_TessCoord.x * input_value[0] + gl_TessCoord.y * input_value[1] +
            gl_TessCoord.z * input_value[2];
    if (parameters.config.w != 0.0) value.r = 4.0 * gl_TessCoord.x * gl_TessCoord.y;
#endif
#ifdef CULL
    uint mode = parameters.state.x;
    gl_CullDistance[0] = mode == 0 ? -1.0 : mode == 3 ? -gl_TessCoord.x - gl_TessCoord.y :
        mode == 5 ? 0.0 : 2.0 * gl_TessCoord.z - 1.0;
    gl_CullDistance[1] = mode == 2 ? -1.0 : mode == 4 ?
        1.0 - 2.0 * gl_TessCoord.z : mode == 5 ? 0.0 : 1.0;
#endif
}
