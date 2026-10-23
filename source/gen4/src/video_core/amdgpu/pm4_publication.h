// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "video_core/amdgpu/pm4_opcodes.h"

namespace AmdGpu {

// The CPU remains excluded for the whole command-processor resume. Register
// packets only change native CP state, so they need no intermediate readback.
// Memory operations, rendering, events, nested buffers and unknown opcodes
// retain their publication boundary. ExecuteCommand still publishes on exit,
// including every outer task yield.
constexpr bool GraphicsPacketNeedsPublication(PM4ItOpcode opcode) {
    switch (opcode) {
    case PM4ItOpcode::ContextControl:
    case PM4ItOpcode::ClearState:
    case PM4ItOpcode::SetConfigReg:
    case PM4ItOpcode::SetContextReg:
    case PM4ItOpcode::SetShReg:
    case PM4ItOpcode::SetUconfigReg:
    case PM4ItOpcode::IndexType:
    case PM4ItOpcode::NumInstances:
    case PM4ItOpcode::IndexBase:
    case PM4ItOpcode::IndexBufferSize:
    case PM4ItOpcode::SetBase:
        return false;
    default:
        // Graphics NOPs may carry a flip interrupt and are not state-only.
        return true;
    }
}

// These packet bodies issue no raw guest stores. NOP eligibility comes
// from the executed decoder branch, never from the opcode alone.
// The rasterizer still inspects when a known backing write or an opaque nested
// callback occurred; this predicate never removes the publication itself.
constexpr bool GraphicsPacketMaySkipHostWriteInspection(
    PM4ItOpcode opcode, bool native_nop_body = false) {
    switch (opcode) {
    case PM4ItOpcode::Nop:
        return native_nop_body;
    case PM4ItOpcode::IncrementDeCounter:
    case PM4ItOpcode::PfpSyncMe:
    case PM4ItOpcode::AcquireMem:
    case PM4ItOpcode::DispatchDirect:
    case PM4ItOpcode::DispatchIndirect:
    case PM4ItOpcode::DrawIndex2:
    case PM4ItOpcode::DrawIndexOffset2:
    case PM4ItOpcode::DrawIndexAuto:
    case PM4ItOpcode::DrawIndirect:
    case PM4ItOpcode::DrawIndirectMulti:
    case PM4ItOpcode::DrawIndexIndirect:
    case PM4ItOpcode::DrawIndexIndirectMulti:
    case PM4ItOpcode::DrawIndexIndirectCountMulti:
        return true;
    default:
        return false;
    }
}

constexpr bool ConstantPacketNeedsPublication(PM4ItOpcode opcode) {
    switch (opcode) {
    case PM4ItOpcode::Nop:
    case PM4ItOpcode::WriteConstRam:
    case PM4ItOpcode::IncrementCeCounter:
        return false;
    default:
        return true;
    }
}

} // namespace AmdGpu
