#include "graphics/shader/recompiler/backend/spirv/spirvEmitterInstructions.h"
#include "graphics/shader/barycentricFallback.h"
#include "common/logging/log.h"

namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter {
namespace {

uint32_t MeshArray(EmitterState& state, spv::StorageClass storage, uint32_t type, uint32_t count) {
	const auto array = state.builder.Type(spv::OpTypeArray, type, ConstantU32(state, count));
	return state.builder.DefineGlobalVariable(TypePointer(state, storage, array), storage);
}

uint32_t MeshElement(EmitterState& state, uint32_t variable, spv::StorageClass storage,
                     uint32_t type, uint32_t index) {
	const auto pointer = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpAccessChain, TypePointer(state, storage, type), pointer,
	                          variable, index);
	return pointer;
}

uint32_t MeshLoad(EmitterState& state, uint32_t variable, spv::StorageClass storage, uint32_t type,
                  uint32_t index) {
	const auto pointer = MeshElement(state, variable, storage, type, index);
	const auto value   = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpLoad, type, value, pointer);
	return value;
}

uint32_t MeshOutputType(EmitterState& state, IR::StageOutputKind kind) {
	switch (kind) {
		case IR::StageOutputKind::Layer: return TypeU32(state);
		case IR::StageOutputKind::ClipDistance:
		case IR::StageOutputKind::CullDistance: return TypeF32(state);
		case IR::StageOutputKind::Position:
		case IR::StageOutputKind::Parameter: return TypeF32Vector(state, 4);
		default: EXIT("unsupported mesh output kind=%u\n", static_cast<uint32_t>(kind));
	}
}

bool MeshRawParameter(const BarycentricFallback::MeshPlan& plan, uint32_t location) {
	return std::ranges::any_of(plan.raw_inputs, [=](const auto& raw) {
		return raw.source_location == location;
	});
}

uint32_t MeshStagingVertices(const EmitterState& state) {
	const auto& mesh = state.input_info.vertex->mesh;
	uint64_t threads = uint64_t(mesh.threads_num[0]) * mesh.threads_num[1] * mesh.threads_num[2];
	if (state.lane_count == 2) threads = ((threads + 63u) / 64u) * 64u;
	EXIT_IF(threads > UINT32_MAX);
	return std::max(mesh.max_vertices, static_cast<uint32_t>(threads));
}

uint32_t MeshUserOutput(EmitterState& state, uint32_t type, uint32_t location, bool primitive) {
	const auto& mesh = state.input_info.vertex->mesh;
	const auto variable = MeshArray(state, spv::StorageClassOutput, type,
	                                primitive ? mesh.max_primitives : mesh.max_vertices);
	state.interface_variables.push_back(variable);
	state.builder.AddAnnotation(spv::OpDecorate, variable, spv::DecorationLocation, location);
	if (primitive) {
		state.builder.AddAnnotation(spv::OpDecorate, variable, spv::DecorationPerPrimitiveEXT);
		state.builder.AddAnnotation(spv::OpDecorate, variable, spv::DecorationFlat);
	}
	return variable;
}

const OutputBinding& MeshSource(const EmitterState& state, IR::StageOutputKind kind, uint32_t location = 0) {
	const auto found = std::ranges::find_if(state.outputs, [=](const auto& output) {
		return output.kind == kind && (kind != IR::StageOutputKind::Parameter || output.location == location);
	});
	if (found == state.outputs.end()) {
		EXIT("mesh barycentric source is missing: kind=%u location=%u\n", uint32_t(kind), location);
	}
	return *found;
}

// Normalize the homogeneous basis independently at each vertex. This changes
// only the auxiliary varying, never Position, and bounds the inverse matrix's
// inputs without dividing by clip W (which may be zero before clipping).
uint32_t MeshHomogeneousBasis(EmitterState& state, uint32_t position,
                               uint32_t* normalization_scale = nullptr) {
	const auto type = TypeF32Vector(state, 3);
	const auto basis = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpVectorShuffle, type, basis, position, position, 0u, 1u, 3u);
	const auto absolute = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpExtInst, type, absolute, GlslStd450(state), GLSLstd450FAbs, basis);
	uint32_t components[3] {};
	for (uint32_t i = 0; i < 3; ++i) {
		components[i] = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpCompositeExtract, TypeF32(state), components[i], absolute, i);
	}
	const auto maximum_xy = state.builder.AllocateId();
	const auto maximum = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpExtInst, TypeF32(state), maximum_xy, GlslStd450(state),
	                          GLSLstd450FMax, components[0], components[1]);
	state.builder.AddFunction(spv::OpExtInst, TypeF32(state), maximum, GlslStd450(state),
	                          GLSLstd450FMax, maximum_xy, components[2]);
	const auto positive = state.builder.AllocateId();
	const auto divisor = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpFOrdGreaterThan, TypeBool(state), positive, maximum, ConstantF32Value(state, 0.0f));
	state.builder.AddFunction(spv::OpSelect, TypeF32(state), divisor, positive, maximum, ConstantF32Value(state, 1.0f));
	if (normalization_scale != nullptr) *normalization_scale = maximum;
	const auto denominator = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpCompositeConstruct, type, denominator, divisor, divisor, divisor);
	const auto result = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpFDiv, type, result, basis, denominator);
	return result;
}

void ValidateMeshFallbackBudget(const EmitterState& state, uint32_t clip_count, uint32_t cull_count) {
	const auto& mesh = state.input_info.vertex->mesh;
	const auto& plan = *mesh.barycentric_fallback;
	const auto& limits = mesh.barycentric_limits;
	if (limits.max_shared_memory_bytes == 0 || limits.max_output_memory_bytes == 0 ||
	    limits.max_output_components == 0 || limits.output_vertex_granularity == 0 ||
	    limits.output_primitive_granularity == 0) {
		EXIT("mesh barycentric fallback requires enabled device memory limits\n");
	}
	const auto check_location = [&](uint32_t location, uint32_t components) {
		if (uint64_t(location) * 4u + components > limits.max_output_components)
			EXIT("mesh barycentric output location exceeds device component limit: location=%u width=%u limit=%u\n",
			     location, components, limits.max_output_components);
	};
	for (const auto location : plan.ordinary_locations) check_location(location, 4u);
	if (!plan.compact)
		for (const auto& input : plan.coordinate_inputs) check_location(input.location, 3u);
	if (!plan.coordinate_inputs.empty())
		for (const auto location : plan.coefficient_locations) check_location(location, plan.compact ? 4u : 3u);
	for (const auto& input : plan.raw_inputs) check_location(input.location, 12u);
	uint64_t shared_bytes = 8u;
	if (state.program.info.uses_lds && state.lds_storage_class == spv::StorageClassWorkgroup)
		shared_bytes += uint64_t(mesh.lds_size_dwords) * sizeof(uint32_t);
	uint64_t vertex_components = ((clip_count + 3u) / 4u + (cull_count + 3u) / 4u) * 4u;
	// The compact fill-only path represents culled primitives by degenerate
	// indices, which Vulkan excludes from mesh output memory accounting.
	uint64_t primitive_components = plan.compact ? 0u : 4u;
	for (const auto& output : state.outputs) {
		if (output.mesh_data_shared) {
			const auto count = output.kind == IR::StageOutputKind::Layer ? mesh.max_vertices : MeshStagingVertices(state);
			shared_bytes += uint64_t(count) * (output.kind == IR::StageOutputKind::Layer ? 4u : 16u);
		}
		if (output.variable_id == 0 || output.kind == IR::StageOutputKind::ClipDistance ||
		    output.kind == IR::StageOutputKind::CullDistance) continue;
		(output.kind == IR::StageOutputKind::Layer ? primitive_components : vertex_components) += 4u;
	}
	if (!plan.compact) vertex_components += plan.coordinate_inputs.size() * 4u;
	primitive_components += (plan.coordinate_inputs.empty() ? 0u : 8u) + plan.raw_inputs.size() * 12u;
	const auto rounded = [](uint64_t count, uint32_t granularity) {
		return (count + granularity - 1u) / granularity * granularity;
	};
	const auto output_bytes = 4u * (vertex_components * rounded(mesh.max_vertices, limits.output_vertex_granularity) +
	                               primitive_components * rounded(mesh.max_primitives, limits.output_primitive_granularity));
	if (shared_bytes > limits.max_shared_memory_bytes || output_bytes > limits.max_output_memory_bytes ||
	    vertex_components + primitive_components > limits.max_output_components) {
		EXIT("mesh barycentric fallback exceeds device limits: shared=%llu/%u output=%llu/%u components=%llu/%u vertices=%u primitives=%u\n",
		     static_cast<unsigned long long>(shared_bytes), limits.max_shared_memory_bytes,
		     static_cast<unsigned long long>(output_bytes), limits.max_output_memory_bytes,
		     static_cast<unsigned long long>(vertex_components + primitive_components), limits.max_output_components,
		     mesh.max_vertices, mesh.max_primitives);
	}
	LOGF("PS5_MESH_BARYCENTRIC_READY shared=%llu/%u output=%llu/%u components=%llu/%u vertices=%u primitives=%u coordinates=%zu raw=%zu compact=%u\n",
	     static_cast<unsigned long long>(shared_bytes), limits.max_shared_memory_bytes,
	     static_cast<unsigned long long>(output_bytes), limits.max_output_memory_bytes,
	     static_cast<unsigned long long>(vertex_components + primitive_components), limits.max_output_components,
	     mesh.max_vertices, mesh.max_primitives, plan.coordinate_inputs.size(), plan.raw_inputs.size(), unsigned(plan.compact));
}

uint32_t MeshPrimitiveIsValid(EmitterState& state, const uint32_t (&vertices)[3],
                               uint32_t vertex_count, uint32_t culled) {
	const auto not_culled = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpLogicalNot, TypeBool(state), not_culled, culled);
	uint32_t valid = not_culled;
	for (const auto vertex : vertices) {
		const auto in_bounds = state.builder.AllocateId();
		const auto both = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpULessThan, TypeBool(state), in_bounds, vertex, vertex_count);
		state.builder.AddFunction(spv::OpLogicalAnd, TypeBool(state), both, valid, in_bounds);
		valid = both;
	}
	return valid;
}

void EmitMeshPrimitiveFallback(EmitterState& state, uint32_t primitive, const uint32_t (&vertices)[3],
                               uint32_t vertex_count, uint32_t culled) {
	const auto* plan = state.input_info.vertex->mesh.barycentric_fallback.get();
	if (plan == nullptr) return;
	const auto valid = MeshPrimitiveIsValid(state, vertices, vertex_count, culled);
	EmitIfCondition(state, valid, [&] {
		if (!plan->coordinate_inputs.empty()) {
			const auto& position = MeshSource(state, IR::StageOutputKind::Position);
			uint32_t basis[3] {};
			uint32_t positions[3] {};
			uint32_t scales[3] {};
			for (uint32_t corner = 0; corner < 3; ++corner) {
				positions[corner] = MeshLoad(state, position.mesh_data_variable,
				    spv::StorageClassWorkgroup, TypeF32Vector(state, 4), vertices[corner]);
				basis[corner] = MeshHomogeneousBasis(state, positions[corner], &scales[corner]);
			}
			uint32_t viewport[4] {};
			uint32_t common_scale = 0;
			if (plan->compact) {
				// A common scale keeps the smooth reconstruction denominator
				// S * FragCoord.w well-conditioned under uniform clip scaling.
				const auto maximum01 = state.builder.AllocateId();
				const auto maximum = state.builder.AllocateId();
				const auto positive = state.builder.AllocateId();
				common_scale = state.builder.AllocateId();
				state.builder.AddFunction(spv::OpExtInst, TypeF32(state), maximum01, GlslStd450(state), GLSLstd450FMax, scales[0], scales[1]);
				state.builder.AddFunction(spv::OpExtInst, TypeF32(state), maximum, GlslStd450(state), GLSLstd450FMax, maximum01, scales[2]);
				state.builder.AddFunction(spv::OpFOrdGreaterThan, TypeBool(state), positive, maximum, ConstantF32Value(state, 0.0f));
				state.builder.AddFunction(spv::OpSelect, TypeF32(state), common_scale, positive, maximum, ConstantF32Value(state, 1.0f));
				const auto denominator = state.builder.AllocateId();
				state.builder.AddFunction(spv::OpCompositeConstruct, TypeF32Vector(state, 3), denominator,
				    common_scale, common_scale, common_scale);
				for (uint32_t corner = 0; corner < 3; ++corner) {
					const auto coordinates = state.builder.AllocateId();
					basis[corner] = state.builder.AllocateId();
					state.builder.AddFunction(spv::OpVectorShuffle, TypeF32Vector(state, 3), coordinates,
					    positions[corner], positions[corner], 0u, 1u, 3u);
					state.builder.AddFunction(spv::OpFDiv, TypeF32Vector(state, 3), basis[corner], coordinates, denominator);
				}
				for (uint32_t i = 0; i < 4; ++i) {
					viewport[i] = state.builder.AllocateId();
					state.builder.AddFunction(spv::OpBitcast, TypeF32(state), viewport[i],
					    EmitPushDataLoad(state, ConstantU32(state, 6u + i)));
				}
			}
			const auto type = TypeF32Vector(state, 3);
			uint32_t cross[2] {};
			for (uint32_t row = 0; row < 2; ++row) {
				cross[row] = state.builder.AllocateId();
				state.builder.AddFunction(spv::OpExtInst, type, cross[row], GlslStd450(state), GLSLstd450Cross,
				                          basis[(row + 1u) % 3u], basis[(row + 2u) % 3u]);
			}
			const auto determinant = state.builder.AllocateId();
			state.builder.AddFunction(spv::OpDot, TypeF32(state), determinant, basis[0], cross[0]);
			const auto nonzero = state.builder.AllocateId();
			const auto divisor = state.builder.AllocateId();
			state.builder.AddFunction(spv::OpFOrdNotEqual, TypeBool(state), nonzero, determinant, ConstantF32Value(state, 0.0f));
			state.builder.AddFunction(spv::OpSelect, TypeF32(state), divisor, nonzero, determinant, ConstantF32Value(state, 1.0f));
			const auto denominator = state.builder.AllocateId();
			state.builder.AddFunction(spv::OpCompositeConstruct, type, denominator, divisor, divisor, divisor);
			const auto zero = state.builder.Constant(spv::OpConstantNull, type);
			for (uint32_t row = 0; row < 2; ++row) {
				const auto quotient = state.builder.AllocateId();
				const auto coefficient = state.builder.AllocateId();
				state.builder.AddFunction(spv::OpFDiv, type, quotient, cross[row], denominator);
				// A singular homogeneous triangle has zero screen-space area. Avoid
				// undefined arithmetic while retaining the original primitive/cull state.
				state.builder.AddFunction(spv::OpSelect, type, coefficient, nonzero, quotient, zero);
				if (plan->compact) {
					// inverse(C/S) yields S times the perspective numerator.
					// The fragment divides by S*FragCoord.w for smooth input,
					// or multiplies by Wi/S for affine input. Never divide by Wi.
					uint32_t elements[3] {};
					for (uint32_t i = 0; i < 3; ++i) {
						elements[i] = state.builder.AllocateId();
						state.builder.AddFunction(spv::OpCompositeExtract, TypeF32(state), elements[i], coefficient, i);
					}
					uint32_t scaled[2] {}, translated[2] {};
					for (uint32_t i = 0; i < 2; ++i) {
						scaled[i] = state.builder.AllocateId();
						translated[i] = state.builder.AllocateId();
						state.builder.AddFunction(spv::OpFMul, TypeF32(state), scaled[i], elements[i], viewport[i]);
						state.builder.AddFunction(spv::OpFMul, TypeF32(state), translated[i], elements[i], viewport[2u + i]);
					}
					const auto offset_x = state.builder.AllocateId();
					const auto offset_xy = state.builder.AllocateId();
					uint32_t weight = common_scale;
					const auto framebuffer_row = state.builder.AllocateId();
					state.builder.AddFunction(spv::OpFAdd, TypeF32(state), offset_x, elements[2], translated[0]);
					state.builder.AddFunction(spv::OpFAdd, TypeF32(state), offset_xy, offset_x, translated[1]);
					if (plan->coordinate_inputs.front().no_perspective) {
						const auto clip_w = state.builder.AllocateId();
						weight = state.builder.AllocateId();
						state.builder.AddFunction(spv::OpCompositeExtract, TypeF32(state), clip_w, positions[row], 3u);
						state.builder.AddFunction(spv::OpFDiv, TypeF32(state), weight, clip_w, common_scale);
					}
					state.builder.AddFunction(spv::OpCompositeConstruct, TypeF32Vector(state, 4), framebuffer_row,
					    scaled[0], scaled[1], offset_xy, weight);
					state.builder.AddFunction(spv::OpStore, MeshElement(state, state.mesh_barycentric_coefficients[row],
					    spv::StorageClassOutput, TypeF32Vector(state, 4), primitive), framebuffer_row);
				} else {
					state.builder.AddFunction(spv::OpStore, MeshElement(state, state.mesh_barycentric_coefficients[row],
					    spv::StorageClassOutput, type, primitive), coefficient);
				}
			}
		}
		const auto vec4 = TypeF32Vector(state, 4);
		const auto array = state.builder.Type(spv::OpTypeArray, vec4, ConstantU32(state, 3));
		for (size_t attribute = 0; attribute < plan->raw_inputs.size(); ++attribute) {
			const auto& source = MeshSource(state, IR::StageOutputKind::Parameter, plan->raw_inputs[attribute].source_location);
			uint32_t corners[3] {};
			for (uint32_t corner = 0; corner < 3; ++corner)
				corners[corner] = MeshLoad(state, source.mesh_data_variable, spv::StorageClassWorkgroup, vec4, vertices[corner]);
			const auto value = state.builder.AllocateId();
			state.builder.AddFunction(spv::OpCompositeConstruct, array, value, corners[0], corners[1], corners[2]);
			state.builder.AddFunction(spv::OpStore, MeshElement(state, state.mesh_barycentric_raw[attribute],
			    spv::StorageClassOutput, array, primitive), value);
		}
	});
}

} // namespace

void DefineMeshOutputs(EmitterState& state, uint32_t clip_distance_count,
                       uint32_t cull_distance_count) {
	const auto& mesh = state.input_info.vertex->mesh;
	const auto* fallback = mesh.barycentric_fallback.get();
	const bool compact = fallback != nullptr && fallback->compact;
	EXIT_IF(compact && fallback->coordinate_inputs.size() > 1);
	for (auto& output: state.outputs) {
		const auto type = MeshOutputType(state, output.kind);
		const bool clip = output.kind == IR::StageOutputKind::ClipDistance;
		const bool cull = output.kind == IR::StageOutputKind::CullDistance;
		auto& variable = clip ? state.clip_distance_variable
		                 : cull ? state.cull_distance_variable : output.variable_id;
		const bool published = fallback == nullptr || output.kind != IR::StageOutputKind::Parameter ||
		    std::ranges::find(fallback->ordinary_locations, output.location) != fallback->ordinary_locations.end();
		if (variable == 0 && published) {
			const auto element_type = clip || cull
			                              ? state.builder.Type(spv::OpTypeArray, type,
			                                                   ConstantU32(state, clip ? clip_distance_count
			                                                                           : cull_distance_count))
			                              : type;
			variable = MeshArray(state, spv::StorageClassOutput, element_type,
			                     output.kind == IR::StageOutputKind::Layer ? mesh.max_primitives
			                                                             : mesh.max_vertices);
			state.interface_variables.push_back(variable);
			state.builder.AddName(variable, output.debug_name.c_str());
			if (output.kind == IR::StageOutputKind::Parameter) {
				state.builder.AddAnnotation(spv::OpDecorate, variable,
				                            spv::DecorationLocation, output.location);
			} else {
				const auto builtin = clip ? spv::BuiltInClipDistance
				                     : cull ? spv::BuiltInCullDistance
				                     : output.kind == IR::StageOutputKind::Layer ? spv::BuiltInLayer
				                                                                 : spv::BuiltInPosition;
				state.builder.AddAnnotation(spv::OpDecorate, variable, spv::DecorationBuiltIn, builtin);
			}
			if (output.kind == IR::StageOutputKind::Layer) {
				state.builder.AddAnnotation(spv::OpDecorate, variable, spv::DecorationPerPrimitiveEXT);
			}
		}
		output.variable_id = variable;
		// Raw corners and the homogeneous position basis are gathered by primitive
		// invocations only after every guest wave has completed.
		const bool shared = output.kind == IR::StageOutputKind::Layer ||
		    (fallback != nullptr &&
		     ((output.kind == IR::StageOutputKind::Position && !fallback->coordinate_inputs.empty()) ||
		      (output.kind == IR::StageOutputKind::Parameter && MeshRawParameter(*fallback, output.location))));
		output.mesh_data_shared = shared;
		const auto shared_count = output.kind == IR::StageOutputKind::Layer ? mesh.max_vertices : MeshStagingVertices(state);
		output.mesh_data_variable =
		    MeshArray(state, shared ? spv::StorageClassWorkgroup : spv::StorageClassPrivate, type,
		              shared ? shared_count : state.lane_count);
	}
	if (fallback != nullptr) {
		const auto vec3 = TypeF32Vector(state, 3);
		if (!fallback->coordinate_inputs.empty())
			(void)MeshSource(state, IR::StageOutputKind::Position);
		if (!compact) {
			for (const auto& coordinate : fallback->coordinate_inputs) {
				const auto output = MeshUserOutput(state, vec3, coordinate.location, false);
				if (coordinate.no_perspective)
					state.builder.AddAnnotation(spv::OpDecorate, output, spv::DecorationNoPerspective);
				state.mesh_barycentric_coordinates.push_back(output);
			}
		}
		if (!fallback->coordinate_inputs.empty()) {
			for (uint32_t row = 0; row < 2; ++row)
				state.mesh_barycentric_coefficients[row] = MeshUserOutput(state,
				    compact ? TypeF32Vector(state, 4) : vec3, fallback->coefficient_locations[row], true);
		}
		const auto raw_type = state.builder.Type(spv::OpTypeArray, TypeF32Vector(state, 4), ConstantU32(state, 3));
		for (const auto& raw : fallback->raw_inputs) {
			(void)MeshSource(state, IR::StageOutputKind::Parameter, raw.source_location);
			state.mesh_barycentric_raw.push_back(MeshUserOutput(state, raw_type, raw.location, true));
		}
		for (const auto location : fallback->ordinary_locations)
			(void)MeshSource(state, IR::StageOutputKind::Parameter, location);
		ValidateMeshFallbackBudget(state, clip_distance_count, cull_distance_count);
	}
	state.mesh_allocation = MeshArray(state, spv::StorageClassWorkgroup, TypeU32(state), 2);
	state.mesh_primitive_data =
	    MeshArray(state, spv::StorageClassPrivate, TypeU32(state), state.lane_count);
	state.mesh_primitives =
	    MeshArray(state, spv::StorageClassOutput, TypeU32Vector(state, 3), mesh.max_primitives);
	state.interface_variables.push_back(state.mesh_primitives);
	state.builder.AddAnnotation(
	    spv::OpDecorate, state.mesh_primitives, spv::DecorationBuiltIn,
	    spv::BuiltInPrimitiveTriangleIndicesEXT); // PrimitiveTriangleIndicesEXT
	if (!compact) {
		state.mesh_cull = MeshArray(state, spv::StorageClassOutput, TypeBool(state), mesh.max_primitives);
		state.interface_variables.push_back(state.mesh_cull);
		state.builder.AddAnnotation(spv::OpDecorate, state.mesh_cull, spv::DecorationBuiltIn,
		                            spv::BuiltInCullPrimitiveEXT); // CullPrimitiveEXT
		state.builder.AddAnnotation(spv::OpDecorate, state.mesh_cull, spv::DecorationPerPrimitiveEXT);
	}
}

uint32_t MeshOutputPointer(EmitterState& state, IR::StageOutputKind kind, uint32_t index) {
	const auto output = std::ranges::find_if(state.outputs, [=](const OutputBinding& binding) {
		return binding.kind == kind && binding.index == index;
	});
	if (output == state.outputs.end()) {
		EXIT("mesh export has no output binding: kind=%u index=%u\n", static_cast<uint32_t>(kind),
		     index);
	}
	const bool shared = output->mesh_data_shared;
	return MeshElement(
	    state, output->mesh_data_variable,
	    shared ? spv::StorageClassWorkgroup : spv::StorageClassPrivate, MeshOutputType(state, kind),
	    shared ? EmitLocalInvocationIndex(state) : ConstantU32(state, state.lane_half));
}

uint32_t MeshPrimitivePointer(EmitterState& state) {
	return MeshElement(state, state.mesh_primitive_data, spv::StorageClassPrivate, TypeU32(state),
	                   ConstantU32(state, state.lane_half));
}

void EmitMeshAllocate(ValueEmitContext& ctx, const IR::Inst& inst) {
	auto&      state = ctx.state;
	const auto first = state.builder.AllocateId();
	state.builder.AddFunction(spv::OpIEqual, TypeBool(state), first,
	                          EmitLocalInvocationIndex(state), ConstantU32(state, 0));
	EmitIfCondition(state, first, [&] {
		const auto allocation = ctx.Arg(inst, 0);
		for (uint32_t field = 0; field < 2; field++) {
			const auto value = state.builder.AllocateId();
			state.builder.AddFunction(spv::OpBitFieldUExtract, TypeU32(state), value, allocation,
			                          ConstantU32(state, field * 12u),
			                          ConstantU32(state, field == 0 ? 10u : 11u));
			const auto pointer =
			    MeshElement(state, state.mesh_allocation, spv::StorageClassWorkgroup,
			                TypeU32(state), ConstantU32(state, field));
			state.builder.AddFunction(spv::OpStore, pointer, value);
		}
	});
}

void EmitMeshEntryPoint(EmitterState& state) {
	const auto* fallback = state.input_info.vertex->mesh.barycentric_fallback.get();
	const bool compact = fallback != nullptr && fallback->compact;
	state.builder.AddFunction(spv::OpFunction, TypeVoid(state), state.main_func,
	                          spv::FunctionControlMaskNone, TypeFunction(state));
	EmitLabel(state, state.builder.AllocateId());
	state.builder.AddFunction(spv::OpFunctionCall, TypeVoid(state), state.builder.AllocateId(),
	                          state.mesh_guest_func);
	// All guest waves finish before the uniform Vulkan allocation and output stores.
	EmitBarrier(state);
	const auto vertices   = MeshLoad(state, state.mesh_allocation, spv::StorageClassWorkgroup,
	                                 TypeU32(state), ConstantU32(state, 0));
	uint32_t primitives = MeshLoad(state, state.mesh_allocation, spv::StorageClassWorkgroup,
	                               TypeU32(state), ConstantU32(state, 1));
	if (compact) {
		// With no vertices there is no valid degenerate index. The entire
		// workgroup uniformly emits no primitives and performs no index stores.
		const auto has_vertices = state.builder.AllocateId();
		const auto bounded = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpINotEqual, TypeBool(state), has_vertices, vertices, ConstantU32(state, 0));
		state.builder.AddFunction(spv::OpSelect, TypeU32(state), bounded, has_vertices, primitives, ConstantU32(state, 0));
		primitives = bounded;
	}
	state.builder.AddFunction(spv::OpSetMeshOutputsEXT, vertices,
	                          primitives); // OpSetMeshOutputsEXT
	for (uint32_t half = 0; half < state.lane_count; half++) {
		state.lane_half      = half;
		const auto index     = EmitLocalInvocationIndex(state);
		const auto is_vertex = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpULessThan, TypeBool(state), is_vertex, index, vertices);
		EmitIfCondition(state, is_vertex, [&] {
			for (const auto& output: state.outputs) {
				if (output.kind == IR::StageOutputKind::Layer || output.variable_id == 0) {
					continue;
				}
				const auto type  = MeshOutputType(state, output.kind);
				const auto value =
				    MeshLoad(state, output.mesh_data_variable,
				             output.mesh_data_shared ? spv::StorageClassWorkgroup : spv::StorageClassPrivate,
				             type, output.mesh_data_shared ? index : ConstantU32(state, half));
				uint32_t pointer;
				if (output.kind == IR::StageOutputKind::ClipDistance ||
				    output.kind == IR::StageOutputKind::CullDistance) {
					pointer = state.builder.AllocateId();
					state.builder.AddFunction(
					    spv::OpAccessChain, TypePointer(state, spv::StorageClassOutput, type), pointer,
					    output.variable_id, index, ConstantU32(state, output.index));
				} else {
					pointer = MeshElement(state, output.variable_id, spv::StorageClassOutput, type, index);
				}
				state.builder.AddFunction(spv::OpStore, pointer, value);
			}
			if (!state.mesh_barycentric_coordinates.empty()) {
				const auto& source = MeshSource(state, IR::StageOutputKind::Position);
				const auto basis = MeshHomogeneousBasis(state, MeshLoad(state, source.mesh_data_variable,
				    spv::StorageClassWorkgroup, TypeF32Vector(state, 4), index));
				for (const auto output : state.mesh_barycentric_coordinates)
					state.builder.AddFunction(spv::OpStore, MeshElement(state, output, spv::StorageClassOutput,
					    TypeF32Vector(state, 3), index), basis);
			}
		});
		const auto is_primitive = state.builder.AllocateId();
		state.builder.AddFunction(spv::OpULessThan, TypeBool(state), is_primitive, index,
		                          primitives);
		EmitIfCondition(state, is_primitive, [&] {
			const auto packed = MeshLoad(state, state.mesh_primitive_data, spv::StorageClassPrivate,
			                             TypeU32(state), ConstantU32(state, half));
			uint32_t   vertex[3] {};
			for (uint32_t component = 0; component < 3; component++) {
				vertex[component] = state.builder.AllocateId();
				state.builder.AddFunction(
				    spv::OpBitFieldUExtract, TypeU32(state), vertex[component], packed,
				    ConstantU32(state, component * 10u), ConstantU32(state, 10));
			}
			const auto null_bit =
			    EmitBinaryU32(state, spv::OpBitwiseAnd, packed, ConstantU32(state, 0x80000000u));
			const auto culled = state.builder.AllocateId();
			state.builder.AddFunction(spv::OpINotEqual, TypeBool(state), culled, null_bit,
			                          ConstantU32(state, 0));
			uint32_t emitted_vertex[3] {vertex[0], vertex[1], vertex[2]};
			if (compact) {
				// The fallback is admitted only for triangle fill without
				// conservative rasterization: a repeated index produces no
				// fragments, preserving live primitive order and provoking mode.
				for (uint32_t component = 0; component < 3; ++component) {
					emitted_vertex[component] = state.builder.AllocateId();
					state.builder.AddFunction(spv::OpSelect, TypeU32(state), emitted_vertex[component],
					    culled, ConstantU32(state, 0), vertex[component]);
				}
			} else {
				state.builder.AddFunction(spv::OpStore,
				    MeshElement(state, state.mesh_cull, spv::StorageClassOutput, TypeBool(state), index), culled);
			}
			const auto triangle = state.builder.AllocateId();
			state.builder.AddFunction(spv::OpCompositeConstruct, TypeU32Vector(state, 3), triangle,
			                          emitted_vertex[0], emitted_vertex[1], emitted_vertex[2]);
			const auto triangle_pointer =
			    MeshElement(state, state.mesh_primitives, spv::StorageClassOutput,
			                TypeU32Vector(state, 3), index);
			state.builder.AddFunction(spv::OpStore, triangle_pointer, triangle);
			EmitMeshPrimitiveFallback(state, index, vertex, vertices, culled);
			const auto emit_layers = [&] {
				for (const auto& output: state.outputs) {
					if (output.kind != IR::StageOutputKind::Layer) continue;
					const auto layer = MeshLoad(state, output.mesh_data_variable,
					    spv::StorageClassWorkgroup, TypeU32(state),
					    vertex[state.input_info.vertex->mesh.provoking_vertex]);
					const auto pointer = MeshElement(state, output.variable_id, spv::StorageClassOutput,
					    TypeU32(state), index);
					state.builder.AddFunction(spv::OpStore, pointer, layer);
				}
			};
			if (compact) {
				// Culled indices may not identify any guest vertex. Give their
				// non-rasterized primitive a defined layer without reading it.
				for (const auto& output: state.outputs) {
					if (output.kind == IR::StageOutputKind::Layer)
						state.builder.AddFunction(spv::OpStore,
						    MeshElement(state, output.variable_id, spv::StorageClassOutput, TypeU32(state), index),
						    ConstantU32(state, 0));
				}
				EmitIfCondition(state, MeshPrimitiveIsValid(state, vertex, vertices, culled), emit_layers);
			} else {
				emit_layers();
			}
		});
	}
	state.lane_half = 0;
	state.builder.AddFunction(spv::OpReturn);
	state.builder.AddFunction(spv::OpFunctionEnd);
}

} // namespace Libs::Graphics::ShaderRecompiler::Spirv::Emitter
