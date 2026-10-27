#version 450
#extension GL_KHR_shader_subgroup_basic : require
#extension GL_KHR_shader_subgroup_ballot : require
#extension GL_KHR_shader_subgroup_shuffle : require
layout(std430, set = 0, binding = 0) readonly buffer Vertices { vec4 words[]; } vertices;
layout(push_constant) uniform Parameters { vec4 config; uvec4 state; } parameters;
layout(location = 0) out vec4 value;
out gl_PerVertex { vec4 gl_Position; };

uint readLane() { return gl_SubgroupInvocationID; }

void main() {
    uint vertex = uint(gl_VertexIndex);
    uint lane = readLane();
    uvec4 activeMask = subgroupBallot(true);
    bool elected = subgroupElect();
    bool ok = lane < 64 && subgroupBallotBitExtract(activeMask, lane);
    ok = ok && subgroupShuffle(vertex, lane) == vertex;
    ok = ok && elected == (subgroupBallotExclusiveBitCount(activeMask) == 0u);
    // Odd vertices create holes in the active lane mask. The builtin read in
    // a helper must retain its physical lane, including under divergent flow.
    if ((vertex & 1u) != 0u) {
        uint sparseLane = readLane();
        uvec4 sparse = subgroupBallot(true);
        ok = ok && sparseLane == lane && subgroupBallotBitExtract(sparse, sparseLane);
        ok = ok && subgroupShuffle(vertex, sparseLane) == vertex;
    }
    ok = ok && uint(gl_InstanceIndex) == parameters.state.y;
    gl_Position = vertices.words[vertex * 2];
    value = vec4(ok ? 1.0 : 0.0, 0.5, 0.75, 1.0);
}
