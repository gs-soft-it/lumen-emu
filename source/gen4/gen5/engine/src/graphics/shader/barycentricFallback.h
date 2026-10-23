// Optional geometry-assisted fragment barycentrics. SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include "graphics/primitiveAssembly.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace Libs::Graphics::BarycentricFallback {

struct Limits {
    uint32_t max_geometry_input_components = 0;
    uint32_t max_geometry_output_components = 0;
    uint32_t max_geometry_total_output_components = 0;
    uint32_t max_geometry_output_vertices = 0;
    uint32_t max_fragment_input_components = 0;
};

enum class Status { NotNeeded, Ready, Unsupported };
enum class Topology { TriangleList, TriangleStrip, Unsupported };

struct BarycentricInput {
    uint32_t variable = 0;
    uint32_t location = 0;
    bool no_perspective = false;
};

struct PerVertexInput {
    uint32_t variable = 0;
    uint32_t source_location = 0;
    uint32_t location = 0;
};

struct FragmentPlan {
    std::vector<uint32_t> spirv;
    std::vector<BarycentricInput> barycentric_inputs;
    std::vector<PerVertexInput> per_vertex_inputs;
};

struct FragmentResult {
    Status status = Status::Unsupported;
    std::string reason;
    FragmentPlan plan;
};

// A compact plan reconstructs center coordinates from FragCoord and two vec4
// inverse rows per primitive. Other plans retain interpolated clip XYW and
// vec3 inverse rows. Raw corner arrays are per primitive in both paths.
struct MeshPlan {
    bool compact = false;
    std::vector<uint32_t> spirv;
    std::vector<BarycentricInput> coordinate_inputs;
    std::array<uint32_t, 2> coefficient_locations {};
    std::vector<PerVertexInput> raw_inputs;
    std::vector<uint32_t> ordinary_locations;
};

struct MeshResult {
    Status status = Status::Unsupported;
    std::string reason;
    MeshPlan plan;
};

struct GeometryResult {
    bool success = false;
    std::string reason;
    std::vector<uint32_t> spirv;
};

// PerVertexKHR inputs are retained as flat arrays of three scalar/vector32
// values at a free three-location span. Their types and indexed loads are unchanged.
// These functions operate only on final SPIR-V; no guest program or shader hash
// is special-cased. A Ready plan must be paired with its generated geometry
// stage. Unsupported leaves the original module for the normal admission gate.
FragmentResult PrepareFragment(std::span<const uint32_t> words, const Limits& limits);

// Convert a validated geometry fallback plan into the corresponding mesh
// fragment interface. Mesh output/shared-memory limits are checked by the
// producer; this checks fragment locations, shape and supported pointer uses.
// Native mesh parameter exports are float32 vec4: ordinary inputs use that
// shape, and raw inputs use array[3] of that shape. Scalar/integer raw inputs
// remain supported only by the geometry fallback. Compact coordinates require
// center invocations: callers must disable them when sample shading is enabled.
MeshResult PrepareMeshFragment(const FragmentPlan& fragment, const Limits& limits,
                               bool allow_compact = true);

// The caller must exclude existing geometry/tessellation/mesh stages, emulated
// rectangle topology, multiview, and non-fill rasterization. Only ordinary
// VS -> triangle-list -> PS pipelines are supported. Callers must canonicalize
// triangle strips with PrimitiveAssembly before building the GS. PointSize, Layer
// and ViewportIndex outputs are rejected. Position and clip/cull distances are
// the supported vertex built-ins.
// The supplied limits must be those of the device whose geometryShader feature
// was enabled. Clip/cull distance features must be enabled, as required by the
// direct renderer. Coordinates describe vertices emitted by the GS;
// the experimental path requires device qualification of triangle-list input
// order and flat provoking-vertex behavior. Direct GS strip input is rejected:
// drivers can rotate odd triangles differently even with FIRST provoking mode.
GeometryResult BuildGeometry(std::span<const uint32_t> vertex_spirv,
                             const FragmentPlan& fragment, Topology topology,
                             bool provoking_vertex_last, const Limits& limits);

} // namespace Libs::Graphics::BarycentricFallback
