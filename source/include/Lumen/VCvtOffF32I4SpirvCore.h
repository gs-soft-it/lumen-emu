#ifndef LUMEN_VCVT_OFF_F32_I4_SPIRV_CORE_H
#define LUMEN_VCVT_OFF_F32_I4_SPIRV_CORE_H

namespace Lumen::Libs::Graphics::Detail {

// Arithmetic shared by the production lowerer and native arithmetic probe.
// Admission separately proves a known whole signed-I32 constant in [-8, 7].
// No source-bit placement or low-nibble interpretation is assumed here.
// Callers supply %int, %float, %float_0_062500 and replace the input/result
// IDs plus the instruction index. EXEC-controlled destination writes belong
// to the production caller, after this finite, exactly representable result.
inline constexpr char kVCvtOffF32I4SpirvArithmetic[] = R"spirv(
        %cvt_off_i32_<index> = OpBitcast %int %<input>
        %cvt_off_f32_<index> = OpConvertSToF %float %cvt_off_i32_<index>
        %<result> = OpFMul %float %cvt_off_f32_<index> %float_0_062500
)spirv";

} // namespace Lumen::Libs::Graphics::Detail

#endif // LUMEN_VCVT_OFF_F32_I4_SPIRV_CORE_H
