// SPDX-License-Identifier: MIT
#pragma once
#include <array>
#include <cstring>
#include "core/libraries/font/font_internal.h"
#include "core/libraries/font/fontft_internal.h"
#include "core/libraries/font/font_driver_abi.h"
#include "core/libraries/font/font_error.h"
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
#include "core/guest_cpu/guest_callback.h"
#include "core/guest_cpu/guest_callback_scratch.h"
#endif

namespace Libraries::Font::Internal::DriverCallbacks {
namespace Ft = Libraries::FontFt::Internal;

// The only native callback is the exact implementation installed by this HLE.
// A failed or unmapped guest callback never becomes a native function call.
template <auto Native, typename Fn, typename... Args>
inline auto Call(Fn fn, const char* label, Args... args) {
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
    if (fn != Native) {
        using Result = decltype(Native(args...));
        return static_cast<Result>(Core::GuestCpu::RunGuestFunctionOrAbort(
            reinterpret_cast<const void*>(fn), label, args...));
    }
#endif
    return fn(args...);
}

inline s32 Open(SysDriver::OpenFn fn, void* library, u32 mode, const void* address,
                u32 size, u32 index, u32 unique, void** object, const char* guest_path = nullptr) {
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
    if (fn != &Ft::LibraryOpenFontMemoryStub) {
        if (!object || !address) return ORBIS_FONT_ERROR_INVALID_PARAMETER;
        // Driver modes 5..7 accept a synchronous guest pathname. The host
        // resolved path belongs only to the native driver; mode 1 borrows the
        // persistent guest font bytes. Do not expose a host path to guest code.
        struct BorrowedOpen { void* object; char path[4096]; } local{};
        local.object = *object;
        const bool path_mode = mode >= 5 && mode <= 7;
        if (path_mode) {
            if (!guest_path) return ORBIS_FONT_ERROR_NO_SUPPORT_FUNCTION;
            const auto length = strnlen(guest_path, sizeof(local.path));
            if (length == sizeof(local.path)) return ORBIS_FONT_ERROR_INVALID_PARAMETER;
            std::memcpy(local.path, guest_path, length + 1);
        }
        Core::GuestCpu::GuestCallbackScratch<BorrowedOpen> scratch{local};
        auto* guest = scratch.Data();
        if (!guest) return ORBIS_FONT_ERROR_ALLOCATION_FAILED;
        const std::array<u64, 7> args{reinterpret_cast<u64>(library), mode,
            reinterpret_cast<u64>(path_mode ? guest->path : address), size, index, unique,
            reinterpret_cast<u64>(&guest->object)};
        const auto rc = static_cast<s32>(Core::GuestCpu::RunGuestFunctionOrAbort(
            reinterpret_cast<const void*>(fn), args, "font driver open"));
        if (!scratch.Read(local)) return ORBIS_FONT_ERROR_INVALID_MEMORY;
        *object = local.object;
        return rc;
    }
#endif
    return fn(library, mode, address, size, index, unique, object);
}

inline s32 Metric(SysDriver::MetricFn fn, void* object, u32 id, u16* output) {
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
    if (fn != &Ft::LibraryGetFaceMetricStub) {
        if (!output) return ORBIS_FONT_ERROR_INVALID_PARAMETER;
        u16 local = *output;
        Core::GuestCpu::GuestCallbackScratch<u16> scratch{local};
        if (!scratch.Data()) return ORBIS_FONT_ERROR_ALLOCATION_FAILED;
        const auto rc = static_cast<s32>(Core::GuestCpu::RunGuestFunctionOrAbort(
            reinterpret_cast<const void*>(fn), "font driver metric", object, id, scratch.Data()));
        if (!scratch.Read(local)) return ORBIS_FONT_ERROR_INVALID_MEMORY;
        *output = local;
        return rc;
    }
#endif
    return fn(object, id, output);
}

inline s32 Scale(SysDriver::ScaleFn fn, void* object, u16* units, float* scale) {
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
    if (fn != &Ft::LibraryGetFaceScaleStub) {
        if (!units || !scale) return ORBIS_FONT_ERROR_INVALID_PARAMETER;
        struct Outputs { u16 units; float scale; } local{*units, *scale};
        Core::GuestCpu::GuestCallbackScratch<Outputs> scratch{local};
        auto* guest = scratch.Data();
        if (!guest) return ORBIS_FONT_ERROR_ALLOCATION_FAILED;
        const auto rc = static_cast<s32>(Core::GuestCpu::RunGuestFunctionOrAbort(
            reinterpret_cast<const void*>(fn), "font driver scale", object,
            &guest->units, &guest->scale));
        if (!scratch.Read(local)) return ORBIS_FONT_ERROR_INVALID_MEMORY;
        *units = local.units; *scale = local.scale;
        return rc;
    }
#endif
    return fn(object, units, scale);
}

inline s32 GlyphIndex(SysDriver::GlyphIndexFn fn, void* object, u32 code, u32* output) {
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
    if (fn != &Ft::LibraryGetGlyphIndexStub) {
        if (!output) return ORBIS_FONT_ERROR_INVALID_PARAMETER;
        u32 local = *output;
        Core::GuestCpu::GuestCallbackScratch<u32> scratch{local};
        if (!scratch.Data()) return ORBIS_FONT_ERROR_ALLOCATION_FAILED;
        const auto rc = static_cast<s32>(Core::GuestCpu::RunGuestFunctionOrAbort(
            reinterpret_cast<const void*>(fn), "font driver glyph index", object, code, scratch.Data()));
        if (!scratch.Read(local)) return ORBIS_FONT_ERROR_INVALID_MEMORY;
        *output = local;
        return rc;
    }
#endif
    return fn(object, code, output);
}

#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
inline s32 GuestSetChar(const void* fn, void* object, bool with_dpi, u32 dpi_x, u32 dpi_y,
                        float scale_x, float scale_y, float* out_x, float* out_y) {
    if (!out_x || !out_y) return ORBIS_FONT_ERROR_INVALID_PARAMETER;
    struct Outputs { float x, y; } local{*out_x, *out_y};
    Core::GuestCpu::GuestCallbackScratch<Outputs> scratch{local};
    auto* guest = scratch.Data();
    if (!guest) return ORBIS_FONT_ERROR_ALLOCATION_FAILED;
    const auto args = MakeFontScaleCallbackArguments(reinterpret_cast<u64>(object), with_dpi,
        dpi_x, dpi_y, scale_x, scale_y, reinterpret_cast<u64>(&guest->x),
        reinterpret_cast<u64>(&guest->y));
    const auto rc = static_cast<s32>(Core::GuestCpu::RunGuestFunctionOrAbort(
        fn, args.Integers(), "font driver set char", 0, args.xmm));
    if (!scratch.Read(local)) return ORBIS_FONT_ERROR_INVALID_MEMORY;
    *out_x = local.x; *out_y = local.y;
    return rc;
}
#endif
inline s32 SetCharWithDpi(SysDriver::SetCharWithDpiFn fn, void* object, u32 dpi_x, u32 dpi_y,
                          float x, float y, float* out_x, float* out_y) {
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
    if (fn != &Ft::LibrarySetCharSizeWithDpiStub)
        return GuestSetChar(reinterpret_cast<const void*>(fn), object, true,
                            dpi_x, dpi_y, x, y, out_x, out_y);
#endif
    return fn(object, dpi_x, dpi_y, x, y, out_x, out_y);
}
inline s32 SetCharDefaultDpi(SysDriver::SetCharDefaultDpiFn fn, void* object,
                             float x, float y, float* out_x, float* out_y) {
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
    if (fn != &Ft::LibrarySetCharSizeDefaultDpiStub)
        return GuestSetChar(reinterpret_cast<const void*>(fn), object, false,
                            0, 0, x, y, out_x, out_y);
#endif
    return fn(object, x, y, out_x, out_y);
}

template <bool Alternate>
inline s32 Layout(SysDriver::ComputeLayoutFn fn, void* object, const void* style,
                   u8 (*output)[16]) {
    constexpr auto native = Alternate ? &Ft::LibraryComputeLayoutAltBlockStub
                                      : &Ft::LibraryComputeLayoutBlockStub;
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
    if (fn != native) {
        if (!style || !output) return ORBIS_FONT_ERROR_INVALID_PARAMETER;
        constexpr std::size_t blocks = Alternate ? 2 : 3;
        struct InputsOutputs { StyleStateBlock style; alignas(16) u8 words[blocks][16]; } local{};
        std::memcpy(&local.style, style, sizeof(local.style));
        std::memcpy(local.words, output, sizeof(local.words));
        Core::GuestCpu::GuestCallbackScratch<InputsOutputs> scratch{local};
        auto* guest = scratch.Data();
        if (!guest) return ORBIS_FONT_ERROR_ALLOCATION_FAILED;
        const auto rc = static_cast<s32>(Core::GuestCpu::RunGuestFunctionOrAbort(
            reinterpret_cast<const void*>(fn), "font driver layout", object,
            &guest->style, guest->words));
        if (!scratch.Read(local)) return ORBIS_FONT_ERROR_INVALID_MEMORY;
        std::memcpy(output, local.words, sizeof(local.words));
        return rc;
    }
#endif
    return fn(object, style, output);
}
} // namespace Libraries::Font::Internal::DriverCallbacks
