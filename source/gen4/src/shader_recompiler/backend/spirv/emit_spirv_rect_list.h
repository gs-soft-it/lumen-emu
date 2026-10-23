// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <sirit/sirit.h>

namespace Shader::IR {
struct Program;
}

namespace Shader::Backend::SPIRV {
class EmitContext;

// A replay is valid only when one invocation can be repeated independently.
// Returns nullptr for supported IR, otherwise an explicit capability reason.
[[nodiscard]] const char* RectListReplayUnsupportedReason(const IR::Program& program);

// The guest function writes ordinary VS outputs and reads rect_vertex_index.
// The wrapper implements one of six triangle-list vertices for each three
// original vertices, including post-VS reconstruction of the fourth corner.
[[nodiscard]] Sirit::Id EmitRectListVertexWrapper(EmitContext& ctx, Sirit::Id guest);
} // namespace Shader::Backend::SPIRV
