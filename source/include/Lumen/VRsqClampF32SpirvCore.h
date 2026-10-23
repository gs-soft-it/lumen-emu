#ifndef LUMEN_VRSQ_CLAMP_F32_SPIRV_CORE_H
#define LUMEN_VRSQ_CLAMP_F32_SPIRV_CORE_H

namespace Lumen::Libs::Graphics::Detail {

// Shared integer-only implementation of the experimental V_RSQ_CLAMP_F32
// diagnostic profile. It exchanges raw binary32 bits and deliberately avoids
// floating-point SPIR-V operations, so host rounding and denormal modes cannot
// alter its result. Callers provide %uint, %int, %bool, %ResTypeU,
// %function_u1, %function_u_u_u and the named scalar constants below.
inline constexpr char kVRsqClampF32SpirvCore[] = R"spirv(
                    ; Compare m * k^2 with target_hi * 2^64.
                    ; Return 0 for less, 1 for equal and 2 for greater.
%lumen_vrsq_compare_product = OpFunction %uint None %function_u_u_u
%lumen_vrsq_compare_m = OpFunctionParameter %uint
%lumen_vrsq_compare_k = OpFunctionParameter %uint
%lumen_vrsq_compare_target_hi = OpFunctionParameter %uint
%lumen_vrsq_compare_entry = OpLabel
%lumen_vrsq_compare_square = OpUMulExtended %ResTypeU %lumen_vrsq_compare_k %lumen_vrsq_compare_k
%lumen_vrsq_compare_square_lo = OpCompositeExtract %uint %lumen_vrsq_compare_square 0
%lumen_vrsq_compare_square_hi = OpCompositeExtract %uint %lumen_vrsq_compare_square 1
%lumen_vrsq_compare_low_product = OpUMulExtended %ResTypeU %lumen_vrsq_compare_square_lo %lumen_vrsq_compare_m
%lumen_vrsq_compare_high_product = OpUMulExtended %ResTypeU %lumen_vrsq_compare_square_hi %lumen_vrsq_compare_m
%lumen_vrsq_compare_word0 = OpCompositeExtract %uint %lumen_vrsq_compare_low_product 0
%lumen_vrsq_compare_low_hi = OpCompositeExtract %uint %lumen_vrsq_compare_low_product 1
%lumen_vrsq_compare_high_lo = OpCompositeExtract %uint %lumen_vrsq_compare_high_product 0
%lumen_vrsq_compare_high_hi = OpCompositeExtract %uint %lumen_vrsq_compare_high_product 1
%lumen_vrsq_compare_word1 = OpIAdd %uint %lumen_vrsq_compare_low_hi %lumen_vrsq_compare_high_lo
%lumen_vrsq_compare_carry = OpULessThan %bool %lumen_vrsq_compare_word1 %lumen_vrsq_compare_low_hi
%lumen_vrsq_compare_carry_u = OpSelect %uint %lumen_vrsq_compare_carry %uint_1 %uint_0
%lumen_vrsq_compare_word2 = OpIAdd %uint %lumen_vrsq_compare_high_hi %lumen_vrsq_compare_carry_u
%lumen_vrsq_compare_high_less = OpULessThan %bool %lumen_vrsq_compare_word2 %lumen_vrsq_compare_target_hi
%lumen_vrsq_compare_high_equal = OpIEqual %bool %lumen_vrsq_compare_word2 %lumen_vrsq_compare_target_hi
%lumen_vrsq_compare_word1_zero = OpIEqual %bool %lumen_vrsq_compare_word1 %uint_0
%lumen_vrsq_compare_word0_zero = OpIEqual %bool %lumen_vrsq_compare_word0 %uint_0
%lumen_vrsq_compare_low_zero = OpLogicalAnd %bool %lumen_vrsq_compare_word1_zero %lumen_vrsq_compare_word0_zero
%lumen_vrsq_compare_equal = OpLogicalAnd %bool %lumen_vrsq_compare_high_equal %lumen_vrsq_compare_low_zero
%lumen_vrsq_compare_not_equal = OpSelect %uint %lumen_vrsq_compare_high_less %uint_0 %uint_2
%lumen_vrsq_compare_result = OpSelect %uint %lumen_vrsq_compare_equal %uint_1 %lumen_vrsq_compare_not_equal
               OpReturnValue %lumen_vrsq_compare_result
               OpFunctionEnd

                    ; Correctly-rounded binary32 1/sqrt(x), clamped for zero.
%lumen_vrsq_clamp_f32_bits = OpFunction %uint None %function_u1
%lumen_vrsq_bits = OpFunctionParameter %uint
%lumen_vrsq_entry = OpLabel
%lumen_vrsq_sign_u = OpShiftRightLogical %uint %lumen_vrsq_bits %uint_31
%lumen_vrsq_sign = OpINotEqual %bool %lumen_vrsq_sign_u %uint_0
%lumen_vrsq_exp = OpBitFieldUExtract %uint %lumen_vrsq_bits %uint_23 %uint_8
%lumen_vrsq_frac = OpBitwiseAnd %uint %lumen_vrsq_bits %uint_0x007fffff
%lumen_vrsq_exp_zero = OpIEqual %bool %lumen_vrsq_exp %uint_0
%lumen_vrsq_frac_zero = OpIEqual %bool %lumen_vrsq_frac %uint_0
%lumen_vrsq_zero = OpLogicalAnd %bool %lumen_vrsq_exp_zero %lumen_vrsq_frac_zero
%lumen_vrsq_exp_max = OpIEqual %bool %lumen_vrsq_exp %uint_255
%lumen_vrsq_special_exp_or_sign = OpLogicalOr %bool %lumen_vrsq_exp_max %lumen_vrsq_sign
%lumen_vrsq_special = OpLogicalOr %bool %lumen_vrsq_zero %lumen_vrsq_special_exp_or_sign
%lumen_vrsq_zero_result = OpSelect %uint %lumen_vrsq_sign %uint_0xff7fffff %uint_0x7f7fffff
%lumen_vrsq_positive_inf = OpLogicalAnd %bool %lumen_vrsq_exp_max %lumen_vrsq_frac_zero
%lumen_vrsq_unsigned = OpLogicalNot %bool %lumen_vrsq_sign
%lumen_vrsq_positive_inf_exact = OpLogicalAnd %bool %lumen_vrsq_positive_inf %lumen_vrsq_unsigned
%lumen_vrsq_inf_or_nan_result = OpSelect %uint %lumen_vrsq_positive_inf_exact %uint_0 %uint_0x7fc00000
%lumen_vrsq_nonzero_special_result = OpSelect %uint %lumen_vrsq_exp_max %lumen_vrsq_inf_or_nan_result %uint_0x7fc00000
%lumen_vrsq_special_result = OpSelect %uint %lumen_vrsq_zero %lumen_vrsq_zero_result %lumen_vrsq_nonzero_special_result
               OpSelectionMerge %lumen_vrsq_class_merge None
               OpBranchConditional %lumen_vrsq_special %lumen_vrsq_special_path %lumen_vrsq_finite_path
%lumen_vrsq_special_path = OpLabel
               OpBranch %lumen_vrsq_class_merge
%lumen_vrsq_finite_path = OpLabel
               OpSelectionMerge %lumen_vrsq_normalized_merge None
               OpBranchConditional %lumen_vrsq_exp_zero %lumen_vrsq_subnormal_start %lumen_vrsq_normal_start
%lumen_vrsq_normal_start = OpLabel
%lumen_vrsq_normal_m = OpBitwiseOr %uint %lumen_vrsq_frac %uint_0x00800000
%lumen_vrsq_normal_exp_i = OpBitcast %int %lumen_vrsq_exp
%lumen_vrsq_normal_e = OpISub %int %lumen_vrsq_normal_exp_i %int_127
               OpBranch %lumen_vrsq_normalized_merge
%lumen_vrsq_subnormal_start = OpLabel
               OpBranch %lumen_vrsq_subnormal_header
%lumen_vrsq_subnormal_header = OpLabel
%lumen_vrsq_subnormal_m = OpPhi %uint %lumen_vrsq_frac %lumen_vrsq_subnormal_start %lumen_vrsq_subnormal_m_next %lumen_vrsq_subnormal_continue
%lumen_vrsq_subnormal_e = OpPhi %int %int_m126 %lumen_vrsq_subnormal_start %lumen_vrsq_subnormal_e_next %lumen_vrsq_subnormal_continue
%lumen_vrsq_subnormal_hidden = OpBitwiseAnd %uint %lumen_vrsq_subnormal_m %uint_0x00800000
%lumen_vrsq_subnormal_ready = OpINotEqual %bool %lumen_vrsq_subnormal_hidden %uint_0
               OpLoopMerge %lumen_vrsq_subnormal_done %lumen_vrsq_subnormal_continue None
               OpBranchConditional %lumen_vrsq_subnormal_ready %lumen_vrsq_subnormal_done %lumen_vrsq_subnormal_continue
%lumen_vrsq_subnormal_continue = OpLabel
%lumen_vrsq_subnormal_m_next = OpShiftLeftLogical %uint %lumen_vrsq_subnormal_m %uint_1
%lumen_vrsq_subnormal_e_next = OpISub %int %lumen_vrsq_subnormal_e %int_1
               OpBranch %lumen_vrsq_subnormal_header
%lumen_vrsq_subnormal_done = OpLabel
               OpBranch %lumen_vrsq_normalized_merge
%lumen_vrsq_normalized_merge = OpLabel
%lumen_vrsq_m = OpPhi %uint %lumen_vrsq_normal_m %lumen_vrsq_normal_start %lumen_vrsq_subnormal_m %lumen_vrsq_subnormal_done
%lumen_vrsq_e = OpPhi %int %lumen_vrsq_normal_e %lumen_vrsq_normal_start %lumen_vrsq_subnormal_e %lumen_vrsq_subnormal_done
%lumen_vrsq_half_e = OpShiftRightArithmetic %int %lumen_vrsq_e %uint_1
%lumen_vrsq_output_exp_i = OpISub %int %int_126 %lumen_vrsq_half_e
%lumen_vrsq_parity_i = OpBitwiseAnd %int %lumen_vrsq_e %int_1
%lumen_vrsq_parity = OpINotEqual %bool %lumen_vrsq_parity_i %int_0
%lumen_vrsq_target_hi = OpSelect %uint %lumen_vrsq_parity %uint_64 %uint_128
               OpBranch %lumen_vrsq_search_header
%lumen_vrsq_search_header = OpLabel
%lumen_vrsq_low = OpPhi %uint %uint_0x00800000 %lumen_vrsq_normalized_merge %lumen_vrsq_low_next %lumen_vrsq_search_body
%lumen_vrsq_high = OpPhi %uint %uint_0x01000000 %lumen_vrsq_normalized_merge %lumen_vrsq_high_next %lumen_vrsq_search_body
%lumen_vrsq_search_more = OpULessThan %bool %lumen_vrsq_low %lumen_vrsq_high
               OpLoopMerge %lumen_vrsq_search_done %lumen_vrsq_search_body None
               OpBranchConditional %lumen_vrsq_search_more %lumen_vrsq_search_body %lumen_vrsq_search_done
%lumen_vrsq_search_body = OpLabel
%lumen_vrsq_search_delta = OpISub %uint %lumen_vrsq_high %lumen_vrsq_low
%lumen_vrsq_search_delta_up = OpIAdd %uint %lumen_vrsq_search_delta %uint_1
%lumen_vrsq_search_half = OpShiftRightLogical %uint %lumen_vrsq_search_delta_up %uint_1
%lumen_vrsq_mid = OpIAdd %uint %lumen_vrsq_low %lumen_vrsq_search_half
%lumen_vrsq_search_relation = OpFunctionCall %uint %lumen_vrsq_compare_product %lumen_vrsq_m %lumen_vrsq_mid %lumen_vrsq_target_hi
%lumen_vrsq_search_feasible = OpULessThanEqual %bool %lumen_vrsq_search_relation %uint_1
%lumen_vrsq_mid_minus_one = OpISub %uint %lumen_vrsq_mid %uint_1
%lumen_vrsq_low_next = OpSelect %uint %lumen_vrsq_search_feasible %lumen_vrsq_mid %lumen_vrsq_low
%lumen_vrsq_high_next = OpSelect %uint %lumen_vrsq_search_feasible %lumen_vrsq_high %lumen_vrsq_mid_minus_one
               OpBranch %lumen_vrsq_search_header
%lumen_vrsq_search_done = OpLabel
%lumen_vrsq_twice_floor = OpShiftLeftLogical %uint %lumen_vrsq_low %uint_1
%lumen_vrsq_midpoint_k = OpIAdd %uint %lumen_vrsq_twice_floor %uint_1
%lumen_vrsq_round_target_hi = OpShiftLeftLogical %uint %lumen_vrsq_target_hi %uint_2
%lumen_vrsq_round_relation = OpFunctionCall %uint %lumen_vrsq_compare_product %lumen_vrsq_m %lumen_vrsq_midpoint_k %lumen_vrsq_round_target_hi
%lumen_vrsq_round_below = OpIEqual %bool %lumen_vrsq_round_relation %uint_0
%lumen_vrsq_round_equal = OpIEqual %bool %lumen_vrsq_round_relation %uint_1
%lumen_vrsq_floor_lsb = OpBitwiseAnd %uint %lumen_vrsq_low %uint_1
%lumen_vrsq_floor_odd = OpINotEqual %bool %lumen_vrsq_floor_lsb %uint_0
%lumen_vrsq_tie_up = OpLogicalAnd %bool %lumen_vrsq_round_equal %lumen_vrsq_floor_odd
%lumen_vrsq_round_up = OpLogicalOr %bool %lumen_vrsq_round_below %lumen_vrsq_tie_up
%lumen_vrsq_round_increment = OpSelect %uint %lumen_vrsq_round_up %uint_1 %uint_0
%lumen_vrsq_rounded = OpIAdd %uint %lumen_vrsq_low %lumen_vrsq_round_increment
%lumen_vrsq_round_carry = OpIEqual %bool %lumen_vrsq_rounded %uint_0x01000000
%lumen_vrsq_significand = OpSelect %uint %lumen_vrsq_round_carry %uint_0x00800000 %lumen_vrsq_rounded
%lumen_vrsq_round_carry_i = OpSelect %int %lumen_vrsq_round_carry %int_1 %int_0
%lumen_vrsq_final_exp_i = OpIAdd %int %lumen_vrsq_output_exp_i %lumen_vrsq_round_carry_i
%lumen_vrsq_final_exp = OpBitcast %uint %lumen_vrsq_final_exp_i
%lumen_vrsq_exp_bits = OpShiftLeftLogical %uint %lumen_vrsq_final_exp %uint_23
%lumen_vrsq_final_frac = OpBitwiseAnd %uint %lumen_vrsq_significand %uint_0x007fffff
%lumen_vrsq_regular_result = OpBitwiseOr %uint %lumen_vrsq_exp_bits %lumen_vrsq_final_frac
               OpBranch %lumen_vrsq_class_merge
%lumen_vrsq_class_merge = OpLabel
%lumen_vrsq_result = OpPhi %uint %lumen_vrsq_special_result %lumen_vrsq_special_path %lumen_vrsq_regular_result %lumen_vrsq_search_done
               OpReturnValue %lumen_vrsq_result
               OpFunctionEnd
)spirv";

} // namespace Lumen::Libs::Graphics::Detail

#endif // LUMEN_VRSQ_CLAMP_F32_SPIRV_CORE_H
