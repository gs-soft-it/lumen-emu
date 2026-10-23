#ifndef LUMEN_VMAC_LEGACY_F32_SPIRV_CORE_H
#define LUMEN_VMAC_LEGACY_F32_SPIRV_CORE_H

namespace Lumen::Libs::Graphics::Detail {

// Shared integer-only binary32 primitives for the experimental
// V_MAC_LEGACY_F32 profile. Callers provide %uint, %bool, %ResTypeU,
// %function_u_u and the named global constants referenced below. The
// semantic helpers exchange raw binary32
// bits, so their two rounding boundaries do not depend on host FP state,
// driver contraction or denormal handling.
inline constexpr char kVMacLegacyF32SpirvShrJamU32[] = R"spirv(
                    ; uint lumen_vmac_shr_jam_u32(uint value, uint distance)
%lumen_vmac_shr_jam_u32 = OpFunction %uint None %function_u_u
%lumen_vmac_shr_value = OpFunctionParameter %uint
%lumen_vmac_shr_distance = OpFunctionParameter %uint
%lumen_vmac_shr_entry = OpLabel
%lumen_vmac_shr_is_zero = OpIEqual %bool %lumen_vmac_shr_distance %uint_0
               OpSelectionMerge %lumen_vmac_shr_merge None
               OpBranchConditional %lumen_vmac_shr_is_zero %lumen_vmac_shr_zero %lumen_vmac_shr_nonzero
%lumen_vmac_shr_zero = OpLabel
               OpBranch %lumen_vmac_shr_merge
%lumen_vmac_shr_nonzero = OpLabel
%lumen_vmac_shr_is_wide = OpUGreaterThanEqual %bool %lumen_vmac_shr_distance %uint_32
               OpSelectionMerge %lumen_vmac_shr_nonzero_merge None
               OpBranchConditional %lumen_vmac_shr_is_wide %lumen_vmac_shr_wide %lumen_vmac_shr_narrow
%lumen_vmac_shr_wide = OpLabel
%lumen_vmac_shr_wide_nonzero = OpINotEqual %bool %lumen_vmac_shr_value %uint_0
%lumen_vmac_shr_wide_result = OpSelect %uint %lumen_vmac_shr_wide_nonzero %uint_1 %uint_0
               OpBranch %lumen_vmac_shr_nonzero_merge
%lumen_vmac_shr_narrow = OpLabel
%lumen_vmac_shr_inverse = OpISub %uint %uint_32 %lumen_vmac_shr_distance
%lumen_vmac_shr_shifted = OpShiftRightLogical %uint %lumen_vmac_shr_value %lumen_vmac_shr_distance
%lumen_vmac_shr_discarded = OpShiftLeftLogical %uint %lumen_vmac_shr_value %lumen_vmac_shr_inverse
%lumen_vmac_shr_has_sticky = OpINotEqual %bool %lumen_vmac_shr_discarded %uint_0
%lumen_vmac_shr_sticky = OpSelect %uint %lumen_vmac_shr_has_sticky %uint_1 %uint_0
%lumen_vmac_shr_narrow_result = OpBitwiseOr %uint %lumen_vmac_shr_shifted %lumen_vmac_shr_sticky
               OpBranch %lumen_vmac_shr_nonzero_merge
%lumen_vmac_shr_nonzero_merge = OpLabel
%lumen_vmac_shr_nonzero_result = OpPhi %uint %lumen_vmac_shr_wide_result %lumen_vmac_shr_wide %lumen_vmac_shr_narrow_result %lumen_vmac_shr_narrow
               OpBranch %lumen_vmac_shr_merge
%lumen_vmac_shr_merge = OpLabel
%lumen_vmac_shr_result = OpPhi %uint %lumen_vmac_shr_value %lumen_vmac_shr_zero %lumen_vmac_shr_nonzero_result %lumen_vmac_shr_nonzero_merge
               OpReturnValue %lumen_vmac_shr_result
               OpFunctionEnd
)spirv";

inline constexpr char kVMacLegacyF32SpirvMulLegacyBits[] = R"spirv(
                    ; uint lumen_vmac_mul_legacy_bits(uint lhs_bits, uint rhs_bits)
%lumen_vmac_mul_legacy_bits = OpFunction %uint None %function_u_u
%lumen_vmac_mul_lhs = OpFunctionParameter %uint
%lumen_vmac_mul_rhs = OpFunctionParameter %uint
%lumen_vmac_mul_entry = OpLabel
%lumen_vmac_mul_lhs_sign = OpBitFieldUExtract %uint %lumen_vmac_mul_lhs %uint_31 %uint_1
%lumen_vmac_mul_rhs_sign = OpBitFieldUExtract %uint %lumen_vmac_mul_rhs %uint_31 %uint_1
%lumen_vmac_mul_sign = OpBitwiseXor %uint %lumen_vmac_mul_lhs_sign %lumen_vmac_mul_rhs_sign
%lumen_vmac_mul_sign_bits = OpShiftLeftLogical %uint %lumen_vmac_mul_sign %uint_31
%lumen_vmac_mul_lhs_exp = OpBitFieldUExtract %uint %lumen_vmac_mul_lhs %uint_23 %uint_8
%lumen_vmac_mul_rhs_exp = OpBitFieldUExtract %uint %lumen_vmac_mul_rhs %uint_23 %uint_8
%lumen_vmac_mul_lhs_frac = OpBitFieldUExtract %uint %lumen_vmac_mul_lhs %uint_0 %uint_23
%lumen_vmac_mul_rhs_frac = OpBitFieldUExtract %uint %lumen_vmac_mul_rhs %uint_0 %uint_23
%lumen_vmac_mul_lhs_exp_zero = OpIEqual %bool %lumen_vmac_mul_lhs_exp %uint_0
%lumen_vmac_mul_rhs_exp_zero = OpIEqual %bool %lumen_vmac_mul_rhs_exp %uint_0
%lumen_vmac_mul_any_exp_zero = OpLogicalOr %bool %lumen_vmac_mul_lhs_exp_zero %lumen_vmac_mul_rhs_exp_zero
%lumen_vmac_mul_lhs_exp_max = OpIEqual %bool %lumen_vmac_mul_lhs_exp %uint_255
%lumen_vmac_mul_rhs_exp_max = OpIEqual %bool %lumen_vmac_mul_rhs_exp %uint_255
%lumen_vmac_mul_lhs_frac_nonzero = OpINotEqual %bool %lumen_vmac_mul_lhs_frac %uint_0
%lumen_vmac_mul_rhs_frac_nonzero = OpINotEqual %bool %lumen_vmac_mul_rhs_frac %uint_0
%lumen_vmac_mul_lhs_nan = OpLogicalAnd %bool %lumen_vmac_mul_lhs_exp_max %lumen_vmac_mul_lhs_frac_nonzero
%lumen_vmac_mul_rhs_nan = OpLogicalAnd %bool %lumen_vmac_mul_rhs_exp_max %lumen_vmac_mul_rhs_frac_nonzero
%lumen_vmac_mul_any_nan = OpLogicalOr %bool %lumen_vmac_mul_lhs_nan %lumen_vmac_mul_rhs_nan
%lumen_vmac_mul_any_inf = OpLogicalOr %bool %lumen_vmac_mul_lhs_exp_max %lumen_vmac_mul_rhs_exp_max
%lumen_vmac_mul_inf_bits = OpBitwiseOr %uint %lumen_vmac_mul_sign_bits %uint_0x7f800000
%lumen_vmac_mul_lhs_sig = OpBitwiseOr %uint %lumen_vmac_mul_lhs_frac %uint_0x00800000
%lumen_vmac_mul_rhs_sig = OpBitwiseOr %uint %lumen_vmac_mul_rhs_frac %uint_0x00800000
%lumen_vmac_mul_wide = OpUMulExtended %ResTypeU %lumen_vmac_mul_lhs_sig %lumen_vmac_mul_rhs_sig
%lumen_vmac_mul_lo = OpCompositeExtract %uint %lumen_vmac_mul_wide 0
%lumen_vmac_mul_hi = OpCompositeExtract %uint %lumen_vmac_mul_wide 1
%lumen_vmac_mul_top_masked = OpBitwiseAnd %uint %lumen_vmac_mul_hi %uint_0x00008000
%lumen_vmac_mul_top = OpINotEqual %bool %lumen_vmac_mul_top_masked %uint_0
%lumen_vmac_mul_top_u = OpSelect %uint %lumen_vmac_mul_top %uint_1 %uint_0
%lumen_vmac_mul_exp_sum = OpIAdd %uint %lumen_vmac_mul_lhs_exp %lumen_vmac_mul_rhs_exp
%lumen_vmac_mul_normalized_exp_sum = OpIAdd %uint %lumen_vmac_mul_exp_sum %lumen_vmac_mul_top_u
%lumen_vmac_mul_underflow = OpULessThan %bool %lumen_vmac_mul_normalized_exp_sum %uint_128
%lumen_vmac_mul_boundary_exp = OpIEqual %bool %lumen_vmac_mul_exp_sum %uint_127
%lumen_vmac_mul_boundary_hi_gt = OpUGreaterThan %bool %lumen_vmac_mul_hi %uint_0x00007fff
%lumen_vmac_mul_boundary_hi_eq = OpIEqual %bool %lumen_vmac_mul_hi %uint_0x00007fff
%lumen_vmac_mul_boundary_lo_ge = OpUGreaterThanEqual %bool %lumen_vmac_mul_lo %uint_0xff800000
%lumen_vmac_mul_boundary_equal_hi_pass = OpLogicalAnd %bool %lumen_vmac_mul_boundary_hi_eq %lumen_vmac_mul_boundary_lo_ge
%lumen_vmac_mul_boundary_product = OpLogicalOr %bool %lumen_vmac_mul_boundary_hi_gt %lumen_vmac_mul_boundary_equal_hi_pass
%lumen_vmac_mul_rounds_min_normal = OpLogicalAnd %bool %lumen_vmac_mul_boundary_exp %lumen_vmac_mul_boundary_product
%lumen_vmac_mul_underflow_magnitude = OpSelect %uint %lumen_vmac_mul_rounds_min_normal %uint_0x00800000 %uint_0
%lumen_vmac_mul_underflow_result = OpBitwiseOr %uint %lumen_vmac_mul_sign_bits %lumen_vmac_mul_underflow_magnitude
%lumen_vmac_mul_top_sig_hi = OpShiftLeftLogical %uint %lumen_vmac_mul_hi %uint_8
%lumen_vmac_mul_top_sig_lo = OpShiftRightLogical %uint %lumen_vmac_mul_lo %uint_24
%lumen_vmac_mul_top_sig = OpBitwiseOr %uint %lumen_vmac_mul_top_sig_hi %lumen_vmac_mul_top_sig_lo
%lumen_vmac_mul_top_guard = OpBitFieldUExtract %uint %lumen_vmac_mul_lo %uint_23 %uint_1
%lumen_vmac_mul_top_sticky_mask = OpBitwiseAnd %uint %lumen_vmac_mul_lo %uint_0x007fffff
%lumen_vmac_mul_top_sticky = OpINotEqual %bool %lumen_vmac_mul_top_sticky_mask %uint_0
%lumen_vmac_mul_low_sig_hi = OpShiftLeftLogical %uint %lumen_vmac_mul_hi %uint_9
%lumen_vmac_mul_low_sig_lo = OpShiftRightLogical %uint %lumen_vmac_mul_lo %uint_23
%lumen_vmac_mul_low_sig = OpBitwiseOr %uint %lumen_vmac_mul_low_sig_hi %lumen_vmac_mul_low_sig_lo
%lumen_vmac_mul_low_guard = OpBitFieldUExtract %uint %lumen_vmac_mul_lo %uint_22 %uint_1
%lumen_vmac_mul_low_sticky_mask = OpBitwiseAnd %uint %lumen_vmac_mul_lo %uint_0x003fffff
%lumen_vmac_mul_low_sticky = OpINotEqual %bool %lumen_vmac_mul_low_sticky_mask %uint_0
%lumen_vmac_mul_sig = OpSelect %uint %lumen_vmac_mul_top %lumen_vmac_mul_top_sig %lumen_vmac_mul_low_sig
%lumen_vmac_mul_guard = OpSelect %uint %lumen_vmac_mul_top %lumen_vmac_mul_top_guard %lumen_vmac_mul_low_guard
%lumen_vmac_mul_sticky = OpSelect %bool %lumen_vmac_mul_top %lumen_vmac_mul_top_sticky %lumen_vmac_mul_low_sticky
%lumen_vmac_mul_guard_set = OpINotEqual %bool %lumen_vmac_mul_guard %uint_0
%lumen_vmac_mul_sig_lsb = OpBitwiseAnd %uint %lumen_vmac_mul_sig %uint_1
%lumen_vmac_mul_sig_odd = OpINotEqual %bool %lumen_vmac_mul_sig_lsb %uint_0
%lumen_vmac_mul_sticky_or_odd = OpLogicalOr %bool %lumen_vmac_mul_sticky %lumen_vmac_mul_sig_odd
%lumen_vmac_mul_increment_needed = OpLogicalAnd %bool %lumen_vmac_mul_guard_set %lumen_vmac_mul_sticky_or_odd
%lumen_vmac_mul_increment = OpSelect %uint %lumen_vmac_mul_increment_needed %uint_1 %uint_0
%lumen_vmac_mul_rounded = OpIAdd %uint %lumen_vmac_mul_sig %lumen_vmac_mul_increment
%lumen_vmac_mul_round_carry = OpIEqual %bool %lumen_vmac_mul_rounded %uint_0x01000000
%lumen_vmac_mul_sig_final = OpSelect %uint %lumen_vmac_mul_round_carry %uint_0x00800000 %lumen_vmac_mul_rounded
%lumen_vmac_mul_round_carry_u = OpSelect %uint %lumen_vmac_mul_round_carry %uint_1 %uint_0
%lumen_vmac_mul_normal_exp = OpISub %uint %lumen_vmac_mul_normalized_exp_sum %uint_127
%lumen_vmac_mul_exp_final = OpIAdd %uint %lumen_vmac_mul_normal_exp %lumen_vmac_mul_round_carry_u
%lumen_vmac_mul_overflow = OpUGreaterThanEqual %bool %lumen_vmac_mul_exp_final %uint_255
%lumen_vmac_mul_exp_bits = OpShiftLeftLogical %uint %lumen_vmac_mul_exp_final %uint_23
%lumen_vmac_mul_frac_final = OpBitFieldUExtract %uint %lumen_vmac_mul_sig_final %uint_0 %uint_23
%lumen_vmac_mul_normal_magnitude = OpBitwiseOr %uint %lumen_vmac_mul_exp_bits %lumen_vmac_mul_frac_final
%lumen_vmac_mul_normal_result = OpBitwiseOr %uint %lumen_vmac_mul_sign_bits %lumen_vmac_mul_normal_magnitude
%lumen_vmac_mul_finite_result = OpSelect %uint %lumen_vmac_mul_overflow %lumen_vmac_mul_inf_bits %lumen_vmac_mul_normal_result
%lumen_vmac_mul_underflow_or_finite = OpSelect %uint %lumen_vmac_mul_underflow %lumen_vmac_mul_underflow_result %lumen_vmac_mul_finite_result
%lumen_vmac_mul_inf_or_finite = OpSelect %uint %lumen_vmac_mul_any_inf %lumen_vmac_mul_inf_bits %lumen_vmac_mul_underflow_or_finite
%lumen_vmac_mul_nan_or_value = OpSelect %uint %lumen_vmac_mul_any_nan %uint_0x7fc00000 %lumen_vmac_mul_inf_or_finite
%lumen_vmac_mul_result = OpSelect %uint %lumen_vmac_mul_any_exp_zero %uint_0 %lumen_vmac_mul_nan_or_value
               OpReturnValue %lumen_vmac_mul_result
               OpFunctionEnd
)spirv";

inline constexpr char kVMacLegacyF32SpirvAddRneFlushBits[] = R"spirv(
                    ; uint lumen_vmac_add_rne_flush_bits(uint lhs_bits, uint rhs_bits)
%lumen_vmac_add_rne_flush_bits = OpFunction %uint None %function_u_u
%lumen_vmac_add_lhs = OpFunctionParameter %uint
%lumen_vmac_add_rhs = OpFunctionParameter %uint
%lumen_vmac_add_entry = OpLabel
%lumen_vmac_add_lhs_sign = OpBitFieldUExtract %uint %lumen_vmac_add_lhs %uint_31 %uint_1
%lumen_vmac_add_rhs_sign = OpBitFieldUExtract %uint %lumen_vmac_add_rhs %uint_31 %uint_1
%lumen_vmac_add_lhs_sign_bits = OpShiftLeftLogical %uint %lumen_vmac_add_lhs_sign %uint_31
%lumen_vmac_add_rhs_sign_bits = OpShiftLeftLogical %uint %lumen_vmac_add_rhs_sign %uint_31
%lumen_vmac_add_lhs_exp = OpBitFieldUExtract %uint %lumen_vmac_add_lhs %uint_23 %uint_8
%lumen_vmac_add_rhs_exp = OpBitFieldUExtract %uint %lumen_vmac_add_rhs %uint_23 %uint_8
%lumen_vmac_add_lhs_frac = OpBitFieldUExtract %uint %lumen_vmac_add_lhs %uint_0 %uint_23
%lumen_vmac_add_rhs_frac = OpBitFieldUExtract %uint %lumen_vmac_add_rhs %uint_0 %uint_23
%lumen_vmac_add_lhs_exp_zero = OpIEqual %bool %lumen_vmac_add_lhs_exp %uint_0
%lumen_vmac_add_rhs_exp_zero = OpIEqual %bool %lumen_vmac_add_rhs_exp %uint_0
%lumen_vmac_add_lhs_value = OpSelect %uint %lumen_vmac_add_lhs_exp_zero %lumen_vmac_add_lhs_sign_bits %lumen_vmac_add_lhs
%lumen_vmac_add_rhs_value = OpSelect %uint %lumen_vmac_add_rhs_exp_zero %lumen_vmac_add_rhs_sign_bits %lumen_vmac_add_rhs
%lumen_vmac_add_lhs_exp_max = OpIEqual %bool %lumen_vmac_add_lhs_exp %uint_255
%lumen_vmac_add_rhs_exp_max = OpIEqual %bool %lumen_vmac_add_rhs_exp %uint_255
%lumen_vmac_add_lhs_frac_zero = OpIEqual %bool %lumen_vmac_add_lhs_frac %uint_0
%lumen_vmac_add_rhs_frac_zero = OpIEqual %bool %lumen_vmac_add_rhs_frac %uint_0
%lumen_vmac_add_lhs_frac_nonzero = OpINotEqual %bool %lumen_vmac_add_lhs_frac %uint_0
%lumen_vmac_add_rhs_frac_nonzero = OpINotEqual %bool %lumen_vmac_add_rhs_frac %uint_0
%lumen_vmac_add_lhs_nan = OpLogicalAnd %bool %lumen_vmac_add_lhs_exp_max %lumen_vmac_add_lhs_frac_nonzero
%lumen_vmac_add_rhs_nan = OpLogicalAnd %bool %lumen_vmac_add_rhs_exp_max %lumen_vmac_add_rhs_frac_nonzero
%lumen_vmac_add_any_nan = OpLogicalOr %bool %lumen_vmac_add_lhs_nan %lumen_vmac_add_rhs_nan
%lumen_vmac_add_lhs_inf = OpLogicalAnd %bool %lumen_vmac_add_lhs_exp_max %lumen_vmac_add_lhs_frac_zero
%lumen_vmac_add_rhs_inf = OpLogicalAnd %bool %lumen_vmac_add_rhs_exp_max %lumen_vmac_add_rhs_frac_zero
%lumen_vmac_add_any_inf = OpLogicalOr %bool %lumen_vmac_add_lhs_inf %lumen_vmac_add_rhs_inf
%lumen_vmac_add_both_inf = OpLogicalAnd %bool %lumen_vmac_add_lhs_inf %lumen_vmac_add_rhs_inf
%lumen_vmac_add_signs_differ = OpINotEqual %bool %lumen_vmac_add_lhs_sign %lumen_vmac_add_rhs_sign
%lumen_vmac_add_opposite_inf = OpLogicalAnd %bool %lumen_vmac_add_both_inf %lumen_vmac_add_signs_differ
%lumen_vmac_add_canonical_required = OpLogicalOr %bool %lumen_vmac_add_any_nan %lumen_vmac_add_opposite_inf
%lumen_vmac_add_inf_sign_bits = OpSelect %uint %lumen_vmac_add_lhs_inf %lumen_vmac_add_lhs_sign_bits %lumen_vmac_add_rhs_sign_bits
%lumen_vmac_add_inf_value = OpBitwiseOr %uint %lumen_vmac_add_inf_sign_bits %uint_0x7f800000
%lumen_vmac_add_lhs_sig = OpBitwiseOr %uint %lumen_vmac_add_lhs_frac %uint_0x00800000
%lumen_vmac_add_rhs_sig = OpBitwiseOr %uint %lumen_vmac_add_rhs_frac %uint_0x00800000
%lumen_vmac_add_lhs_exp_gt = OpUGreaterThan %bool %lumen_vmac_add_lhs_exp %lumen_vmac_add_rhs_exp
%lumen_vmac_add_exp_equal = OpIEqual %bool %lumen_vmac_add_lhs_exp %lumen_vmac_add_rhs_exp
%lumen_vmac_add_lhs_sig_ge = OpUGreaterThanEqual %bool %lumen_vmac_add_lhs_sig %lumen_vmac_add_rhs_sig
%lumen_vmac_add_equal_exp_lhs_ge = OpLogicalAnd %bool %lumen_vmac_add_exp_equal %lumen_vmac_add_lhs_sig_ge
%lumen_vmac_add_lhs_larger = OpLogicalOr %bool %lumen_vmac_add_lhs_exp_gt %lumen_vmac_add_equal_exp_lhs_ge
%lumen_vmac_add_large_exp = OpSelect %uint %lumen_vmac_add_lhs_larger %lumen_vmac_add_lhs_exp %lumen_vmac_add_rhs_exp
%lumen_vmac_add_small_exp = OpSelect %uint %lumen_vmac_add_lhs_larger %lumen_vmac_add_rhs_exp %lumen_vmac_add_lhs_exp
%lumen_vmac_add_large_sig = OpSelect %uint %lumen_vmac_add_lhs_larger %lumen_vmac_add_lhs_sig %lumen_vmac_add_rhs_sig
%lumen_vmac_add_small_sig = OpSelect %uint %lumen_vmac_add_lhs_larger %lumen_vmac_add_rhs_sig %lumen_vmac_add_lhs_sig
%lumen_vmac_add_large_sign = OpSelect %uint %lumen_vmac_add_lhs_larger %lumen_vmac_add_lhs_sign %lumen_vmac_add_rhs_sign
%lumen_vmac_add_large_value = OpSelect %uint %lumen_vmac_add_lhs_larger %lumen_vmac_add_lhs_value %lumen_vmac_add_rhs_value
%lumen_vmac_add_exp_distance = OpISub %uint %lumen_vmac_add_large_exp %lumen_vmac_add_small_exp
%lumen_vmac_add_distance_zero = OpIEqual %bool %lumen_vmac_add_exp_distance %uint_0
%lumen_vmac_add_distance_wide = OpUGreaterThanEqual %bool %lumen_vmac_add_exp_distance %uint_26
%lumen_vmac_add_distance_clamped = OpSelect %uint %lumen_vmac_add_distance_wide %uint_25 %lumen_vmac_add_exp_distance
%lumen_vmac_add_safe_distance = OpSelect %uint %lumen_vmac_add_distance_zero %uint_1 %lumen_vmac_add_distance_clamped
%lumen_vmac_add_inverse_distance = OpISub %uint %uint_32 %lumen_vmac_add_safe_distance
%lumen_vmac_add_small_hi_shifted = OpShiftRightLogical %uint %lumen_vmac_add_small_sig %lumen_vmac_add_safe_distance)spirv"
R"spirv(
%lumen_vmac_add_small_lo_shifted = OpShiftLeftLogical %uint %lumen_vmac_add_small_sig %lumen_vmac_add_inverse_distance
%lumen_vmac_add_small_hi = OpSelect %uint %lumen_vmac_add_distance_zero %lumen_vmac_add_small_sig %lumen_vmac_add_small_hi_shifted
%lumen_vmac_add_small_lo = OpSelect %uint %lumen_vmac_add_distance_zero %uint_0 %lumen_vmac_add_small_lo_shifted
%lumen_vmac_add_same_sign = OpIEqual %bool %lumen_vmac_add_lhs_sign %lumen_vmac_add_rhs_sign
%lumen_vmac_add_sum_hi = OpIAdd %uint %lumen_vmac_add_large_sig %lumen_vmac_add_small_hi
%lumen_vmac_add_sum_lo = OpCopyObject %uint %lumen_vmac_add_small_lo
%lumen_vmac_add_sum_carry_masked = OpBitwiseAnd %uint %lumen_vmac_add_sum_hi %uint_0x01000000
%lumen_vmac_add_sum_carry = OpINotEqual %bool %lumen_vmac_add_sum_carry_masked %uint_0
%lumen_vmac_add_sum_lo_jammed = OpFunctionCall %uint %lumen_vmac_shr_jam_u32 %lumen_vmac_add_sum_lo %uint_1
%lumen_vmac_add_sum_hi_lsb = OpBitwiseAnd %uint %lumen_vmac_add_sum_hi %uint_1
%lumen_vmac_add_sum_hi_to_lo = OpShiftLeftLogical %uint %lumen_vmac_add_sum_hi_lsb %uint_31
%lumen_vmac_add_sum_lo_shifted = OpBitwiseOr %uint %lumen_vmac_add_sum_lo_jammed %lumen_vmac_add_sum_hi_to_lo
%lumen_vmac_add_sum_hi_shifted = OpShiftRightLogical %uint %lumen_vmac_add_sum_hi %uint_1
%lumen_vmac_add_add_hi = OpSelect %uint %lumen_vmac_add_sum_carry %lumen_vmac_add_sum_hi_shifted %lumen_vmac_add_sum_hi
%lumen_vmac_add_add_lo = OpSelect %uint %lumen_vmac_add_sum_carry %lumen_vmac_add_sum_lo_shifted %lumen_vmac_add_sum_lo
%lumen_vmac_add_sum_carry_u = OpSelect %uint %lumen_vmac_add_sum_carry %uint_1 %uint_0
%lumen_vmac_add_add_exp = OpIAdd %uint %lumen_vmac_add_large_exp %lumen_vmac_add_sum_carry_u
%lumen_vmac_add_sub_lo = OpISub %uint %uint_0 %lumen_vmac_add_small_lo
%lumen_vmac_add_sub_borrow = OpINotEqual %bool %lumen_vmac_add_small_lo %uint_0
%lumen_vmac_add_sub_borrow_u = OpSelect %uint %lumen_vmac_add_sub_borrow %uint_1 %uint_0
%lumen_vmac_add_sub_hi_base = OpISub %uint %lumen_vmac_add_large_sig %lumen_vmac_add_small_hi
%lumen_vmac_add_sub_hi = OpISub %uint %lumen_vmac_add_sub_hi_base %lumen_vmac_add_sub_borrow_u
%lumen_vmac_add_sub_hi_zero = OpIEqual %bool %lumen_vmac_add_sub_hi %uint_0
%lumen_vmac_add_sub_lo_zero = OpIEqual %bool %lumen_vmac_add_sub_lo %uint_0
%lumen_vmac_add_sub_pair_zero = OpLogicalAnd %bool %lumen_vmac_add_sub_hi_zero %lumen_vmac_add_sub_lo_zero
%lumen_vmac_add_exact_cancel = OpLogicalAnd %bool %lumen_vmac_add_signs_differ %lumen_vmac_add_sub_pair_zero
%lumen_vmac_add_norm16_needed = OpULessThan %bool %lumen_vmac_add_sub_hi %uint_0x00000100
%lumen_vmac_add_norm16_hi_left = OpShiftLeftLogical %uint %lumen_vmac_add_sub_hi %uint_16
%lumen_vmac_add_norm16_lo_right = OpShiftRightLogical %uint %lumen_vmac_add_sub_lo %uint_16
%lumen_vmac_add_norm16_hi_candidate = OpBitwiseOr %uint %lumen_vmac_add_norm16_hi_left %lumen_vmac_add_norm16_lo_right
%lumen_vmac_add_norm16_lo_candidate = OpShiftLeftLogical %uint %lumen_vmac_add_sub_lo %uint_16
%lumen_vmac_add_norm16_hi = OpSelect %uint %lumen_vmac_add_norm16_needed %lumen_vmac_add_norm16_hi_candidate %lumen_vmac_add_sub_hi
%lumen_vmac_add_norm16_lo = OpSelect %uint %lumen_vmac_add_norm16_needed %lumen_vmac_add_norm16_lo_candidate %lumen_vmac_add_sub_lo
%lumen_vmac_add_norm16_shift = OpSelect %uint %lumen_vmac_add_norm16_needed %uint_16 %uint_0
%lumen_vmac_add_norm8_needed = OpULessThan %bool %lumen_vmac_add_norm16_hi %uint_0x00010000
%lumen_vmac_add_norm8_hi_left = OpShiftLeftLogical %uint %lumen_vmac_add_norm16_hi %uint_8
%lumen_vmac_add_norm8_lo_right = OpShiftRightLogical %uint %lumen_vmac_add_norm16_lo %uint_24
%lumen_vmac_add_norm8_hi_candidate = OpBitwiseOr %uint %lumen_vmac_add_norm8_hi_left %lumen_vmac_add_norm8_lo_right
%lumen_vmac_add_norm8_lo_candidate = OpShiftLeftLogical %uint %lumen_vmac_add_norm16_lo %uint_8
%lumen_vmac_add_norm8_hi = OpSelect %uint %lumen_vmac_add_norm8_needed %lumen_vmac_add_norm8_hi_candidate %lumen_vmac_add_norm16_hi
%lumen_vmac_add_norm8_lo = OpSelect %uint %lumen_vmac_add_norm8_needed %lumen_vmac_add_norm8_lo_candidate %lumen_vmac_add_norm16_lo
%lumen_vmac_add_norm8_shift = OpSelect %uint %lumen_vmac_add_norm8_needed %uint_8 %uint_0
%lumen_vmac_add_norm8_shift_total = OpIAdd %uint %lumen_vmac_add_norm16_shift %lumen_vmac_add_norm8_shift
%lumen_vmac_add_norm4_needed = OpULessThan %bool %lumen_vmac_add_norm8_hi %uint_0x00100000
%lumen_vmac_add_norm4_hi_left = OpShiftLeftLogical %uint %lumen_vmac_add_norm8_hi %uint_4
%lumen_vmac_add_norm4_lo_right = OpShiftRightLogical %uint %lumen_vmac_add_norm8_lo %uint_28
%lumen_vmac_add_norm4_hi_candidate = OpBitwiseOr %uint %lumen_vmac_add_norm4_hi_left %lumen_vmac_add_norm4_lo_right
%lumen_vmac_add_norm4_lo_candidate = OpShiftLeftLogical %uint %lumen_vmac_add_norm8_lo %uint_4
%lumen_vmac_add_norm4_hi = OpSelect %uint %lumen_vmac_add_norm4_needed %lumen_vmac_add_norm4_hi_candidate %lumen_vmac_add_norm8_hi
%lumen_vmac_add_norm4_lo = OpSelect %uint %lumen_vmac_add_norm4_needed %lumen_vmac_add_norm4_lo_candidate %lumen_vmac_add_norm8_lo
%lumen_vmac_add_norm4_shift = OpSelect %uint %lumen_vmac_add_norm4_needed %uint_4 %uint_0
%lumen_vmac_add_norm4_shift_total = OpIAdd %uint %lumen_vmac_add_norm8_shift_total %lumen_vmac_add_norm4_shift)spirv"
R"spirv(
%lumen_vmac_add_norm2_needed = OpULessThan %bool %lumen_vmac_add_norm4_hi %uint_0x00400000
%lumen_vmac_add_norm2_hi_left = OpShiftLeftLogical %uint %lumen_vmac_add_norm4_hi %uint_2
%lumen_vmac_add_norm2_lo_right = OpShiftRightLogical %uint %lumen_vmac_add_norm4_lo %uint_30
%lumen_vmac_add_norm2_hi_candidate = OpBitwiseOr %uint %lumen_vmac_add_norm2_hi_left %lumen_vmac_add_norm2_lo_right
%lumen_vmac_add_norm2_lo_candidate = OpShiftLeftLogical %uint %lumen_vmac_add_norm4_lo %uint_2
%lumen_vmac_add_norm2_hi = OpSelect %uint %lumen_vmac_add_norm2_needed %lumen_vmac_add_norm2_hi_candidate %lumen_vmac_add_norm4_hi
%lumen_vmac_add_norm2_lo = OpSelect %uint %lumen_vmac_add_norm2_needed %lumen_vmac_add_norm2_lo_candidate %lumen_vmac_add_norm4_lo
%lumen_vmac_add_norm2_shift = OpSelect %uint %lumen_vmac_add_norm2_needed %uint_2 %uint_0
%lumen_vmac_add_norm2_shift_total = OpIAdd %uint %lumen_vmac_add_norm4_shift_total %lumen_vmac_add_norm2_shift
%lumen_vmac_add_norm1_needed = OpULessThan %bool %lumen_vmac_add_norm2_hi %uint_0x00800000
%lumen_vmac_add_norm1_hi_left = OpShiftLeftLogical %uint %lumen_vmac_add_norm2_hi %uint_1
%lumen_vmac_add_norm1_lo_right = OpShiftRightLogical %uint %lumen_vmac_add_norm2_lo %uint_31
%lumen_vmac_add_norm1_hi_candidate = OpBitwiseOr %uint %lumen_vmac_add_norm1_hi_left %lumen_vmac_add_norm1_lo_right
%lumen_vmac_add_norm1_lo_candidate = OpShiftLeftLogical %uint %lumen_vmac_add_norm2_lo %uint_1
%lumen_vmac_add_norm1_hi = OpSelect %uint %lumen_vmac_add_norm1_needed %lumen_vmac_add_norm1_hi_candidate %lumen_vmac_add_norm2_hi
%lumen_vmac_add_norm1_lo = OpSelect %uint %lumen_vmac_add_norm1_needed %lumen_vmac_add_norm1_lo_candidate %lumen_vmac_add_norm2_lo
%lumen_vmac_add_norm1_shift = OpSelect %uint %lumen_vmac_add_norm1_needed %uint_1 %uint_0
%lumen_vmac_add_normalize_shift = OpIAdd %uint %lumen_vmac_add_norm2_shift_total %lumen_vmac_add_norm1_shift
%lumen_vmac_add_sub_underflow = OpULessThanEqual %bool %lumen_vmac_add_large_exp %lumen_vmac_add_normalize_shift
%lumen_vmac_add_sub_exp_raw = OpISub %uint %lumen_vmac_add_large_exp %lumen_vmac_add_normalize_shift
%lumen_vmac_add_sub_exp = OpSelect %uint %lumen_vmac_add_sub_underflow %uint_0 %lumen_vmac_add_sub_exp_raw
%lumen_vmac_add_arithmetic_hi = OpSelect %uint %lumen_vmac_add_same_sign %lumen_vmac_add_add_hi %lumen_vmac_add_norm1_hi
%lumen_vmac_add_arithmetic_lo = OpSelect %uint %lumen_vmac_add_same_sign %lumen_vmac_add_add_lo %lumen_vmac_add_norm1_lo
%lumen_vmac_add_arithmetic_exp = OpSelect %uint %lumen_vmac_add_same_sign %lumen_vmac_add_add_exp %lumen_vmac_add_sub_exp
%lumen_vmac_add_result_sign = OpSelect %uint %lumen_vmac_add_same_sign %lumen_vmac_add_lhs_sign %lumen_vmac_add_large_sign
%lumen_vmac_add_result_sign_bits = OpShiftLeftLogical %uint %lumen_vmac_add_result_sign %uint_31
%lumen_vmac_add_guard = OpBitFieldUExtract %uint %lumen_vmac_add_arithmetic_lo %uint_31 %uint_1
%lumen_vmac_add_sticky_mask = OpBitFieldUExtract %uint %lumen_vmac_add_arithmetic_lo %uint_0 %uint_31
%lumen_vmac_add_sticky = OpINotEqual %bool %lumen_vmac_add_sticky_mask %uint_0
%lumen_vmac_add_guard_set = OpINotEqual %bool %lumen_vmac_add_guard %uint_0
%lumen_vmac_add_arithmetic_lsb = OpBitwiseAnd %uint %lumen_vmac_add_arithmetic_hi %uint_1
%lumen_vmac_add_arithmetic_odd = OpINotEqual %bool %lumen_vmac_add_arithmetic_lsb %uint_0
%lumen_vmac_add_sticky_or_odd = OpLogicalOr %bool %lumen_vmac_add_sticky %lumen_vmac_add_arithmetic_odd
%lumen_vmac_add_increment_needed = OpLogicalAnd %bool %lumen_vmac_add_guard_set %lumen_vmac_add_sticky_or_odd
%lumen_vmac_add_increment = OpSelect %uint %lumen_vmac_add_increment_needed %uint_1 %uint_0
%lumen_vmac_add_rounded = OpIAdd %uint %lumen_vmac_add_arithmetic_hi %lumen_vmac_add_increment
%lumen_vmac_add_round_carry = OpIEqual %bool %lumen_vmac_add_rounded %uint_0x01000000
%lumen_vmac_add_sig_final = OpSelect %uint %lumen_vmac_add_round_carry %uint_0x00800000 %lumen_vmac_add_rounded
%lumen_vmac_add_round_carry_u = OpSelect %uint %lumen_vmac_add_round_carry %uint_1 %uint_0
%lumen_vmac_add_exp_final = OpIAdd %uint %lumen_vmac_add_arithmetic_exp %lumen_vmac_add_round_carry_u
%lumen_vmac_add_overflow = OpUGreaterThanEqual %bool %lumen_vmac_add_exp_final %uint_255
%lumen_vmac_add_exp_bits = OpShiftLeftLogical %uint %lumen_vmac_add_exp_final %uint_23
%lumen_vmac_add_frac_final = OpBitFieldUExtract %uint %lumen_vmac_add_sig_final %uint_0 %uint_23
%lumen_vmac_add_magnitude = OpBitwiseOr %uint %lumen_vmac_add_exp_bits %lumen_vmac_add_frac_final
%lumen_vmac_add_packed = OpBitwiseOr %uint %lumen_vmac_add_result_sign_bits %lumen_vmac_add_magnitude
%lumen_vmac_add_overflow_result = OpBitwiseOr %uint %lumen_vmac_add_result_sign_bits %uint_0x7f800000
%lumen_vmac_add_rounded_result = OpSelect %uint %lumen_vmac_add_overflow %lumen_vmac_add_overflow_result %lumen_vmac_add_packed
%lumen_vmac_add_sub_underflow_required = OpLogicalAnd %bool %lumen_vmac_add_signs_differ %lumen_vmac_add_sub_underflow
%lumen_vmac_add_underflow_result = OpSelect %uint %lumen_vmac_add_sub_underflow_required %lumen_vmac_add_result_sign_bits %lumen_vmac_add_rounded_result
%lumen_vmac_add_cancel_result = OpSelect %uint %lumen_vmac_add_exact_cancel %uint_0 %lumen_vmac_add_underflow_result
%lumen_vmac_add_gap_result = OpSelect %uint %lumen_vmac_add_distance_wide %lumen_vmac_add_large_value %lumen_vmac_add_cancel_result
%lumen_vmac_add_rhs_zero_result = OpSelect %uint %lumen_vmac_add_rhs_exp_zero %lumen_vmac_add_lhs_value %lumen_vmac_add_gap_result
%lumen_vmac_add_one_zero_result = OpSelect %uint %lumen_vmac_add_lhs_exp_zero %lumen_vmac_add_rhs_value %lumen_vmac_add_rhs_zero_result
%lumen_vmac_add_both_zero = OpLogicalAnd %bool %lumen_vmac_add_lhs_exp_zero %lumen_vmac_add_rhs_exp_zero
%lumen_vmac_add_both_zero_sign = OpBitwiseAnd %uint %lumen_vmac_add_lhs_sign %lumen_vmac_add_rhs_sign
%lumen_vmac_add_both_zero_bits = OpShiftLeftLogical %uint %lumen_vmac_add_both_zero_sign %uint_31
%lumen_vmac_add_finite_result = OpSelect %uint %lumen_vmac_add_both_zero %lumen_vmac_add_both_zero_bits %lumen_vmac_add_one_zero_result
%lumen_vmac_add_inf_or_finite = OpSelect %uint %lumen_vmac_add_any_inf %lumen_vmac_add_inf_value %lumen_vmac_add_finite_result
%lumen_vmac_add_result = OpSelect %uint %lumen_vmac_add_canonical_required %uint_0x7fc00000 %lumen_vmac_add_inf_or_finite
               OpReturnValue %lumen_vmac_add_result
               OpFunctionEnd
)spirv";

} // namespace Lumen::Libs::Graphics::Detail

#endif // LUMEN_VMAC_LEGACY_F32_SPIRV_CORE_H
