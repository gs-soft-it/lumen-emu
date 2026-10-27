#version 450
layout(vertices = CONTROL_POINTS) out;
layout(location = 0) in vec4 input_value[];
layout(location = 0) out vec4 output_value[];
layout(push_constant) uniform Parameters { vec4 config; uvec4 state; } parameters;
void main() {
    gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;
    output_value[gl_InvocationID] = input_value[gl_InvocationID];
    if (gl_InvocationID == 0) {
        gl_TessLevelOuter[0] = parameters.config.x;
        gl_TessLevelOuter[1] = parameters.config.x;
        gl_TessLevelOuter[2] = parameters.config.x;
        gl_TessLevelOuter[3] = parameters.config.x;
        gl_TessLevelInner[0] = parameters.config.x;
        gl_TessLevelInner[1] = parameters.config.x;
        if (parameters.state.z != 0) {
            gl_TessLevelOuter[0] = 1.0;
            gl_TessLevelOuter[1] = 2.5;
            gl_TessLevelOuter[2] = 4.0;
            gl_TessLevelOuter[3] = 6.0;
            gl_TessLevelInner[0] = 3.5;
            gl_TessLevelInner[1] = 2.5;
        }
    }
}
