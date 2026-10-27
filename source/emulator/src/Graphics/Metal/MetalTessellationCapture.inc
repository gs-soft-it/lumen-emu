// Included by the Metal command adapter. The domain generator supplies unique
// coordinates; this stage evaluates the original TES once for each coordinate.
// Primitive assembly/cull distance is a separate mesh pass, before rasterization.
struct MetalTessellationCapture
{
	__strong id<MTLComputePipelineState> pipeline = nil;
	std::string source;
	uint32_t outputStride = 0;
};

struct MetalTessellationCaptureParams
{
	uint32_t pointStride, patchCount, instanceIndex, baseInstance;
};
static_assert(sizeof(MetalTessellationCaptureParams) == 16);

VkResult CreateMetalTessellationCapture(id<MTLDevice> device, const std::string& original,
	const std::string& entry, const std::string& outputType, bool triangles,
	MetalTessellationCapture* result)
{
	if (result == nullptr || entry.empty() || outputType.empty()) return VK_ERROR_INVALID_SHADER_NV;
	const auto fail = [](const char* reason) {
		fprintf(stderr, "[Metal] TES capture: %s\n", reason);
		return VK_ERROR_FEATURE_NOT_PRESENT;
	};
	const auto trim = [](const std::string& value) {
		const auto first = value.find_first_not_of(" \t\r\n");
		return first == std::string::npos ? std::string() :
		    value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
	};
	const auto name = original.find(" " + entry + "(");
	if (name == std::string::npos) return fail("entry point is absent");
	const auto qualifier = original.rfind("vertex ", name);
	if (qualifier == std::string::npos) return fail("entry point is not a vertex function");
	const bool capturedOutput = trim(original.substr(qualifier + 7, name - qualifier - 7)) == "void";
	const auto parameters = original.find('(', name);
	size_t end = parameters + 1;
	uint32_t depth = 1;
	for (; end < original.size() && depth != 0; ++end)
	{
		if (original[end] == '(') ++depth;
		else if (original[end] == ')') --depth;
	}
	if (depth != 0) return fail("unbalanced entry point parameters");
	const auto parameterEnd = end - 1;
	std::vector<std::string> arguments;
	size_t argumentStart = parameters + 1;
	uint32_t parens = 0, brackets = 0, angles = 0;
	for (size_t cursor = argumentStart; cursor <= parameterEnd; ++cursor)
	{
		const char c = original[cursor];
		if (cursor == parameterEnd || (c == ',' && parens == 0 && brackets == 0 && angles == 0))
		{
			auto argument = trim(original.substr(argumentStart, cursor - argumentStart));
			if (!argument.empty()) arguments.push_back(std::move(argument));
			argumentStart = cursor + 1;
		}
		else if (c == '(') ++parens;
		else if (c == ')') { if (parens == 0) return fail("unbalanced parameter"); --parens; }
		else if (c == '[') ++brackets;
		else if (c == ']') { if (brackets == 0) return fail("unbalanced attribute"); --brackets; }
		else if (c == '<') ++angles;
		else if (c == '>') { if (angles == 0) return fail("unbalanced template"); --angles; }
	}
	std::string helperParameters, kernelParameters, callArguments, outputBuffer, indirectParameters;
	for (const auto& argument: arguments)
	{
		const auto attributeStart = argument.find("[[");
		const auto attributeEnd = argument.find("]]", attributeStart);
		if (attributeStart == std::string::npos || attributeEnd == std::string::npos ||
		    !trim(argument.substr(attributeEnd + 2)).empty()) return fail("unsupported TES parameter declaration");
		const auto declaration = trim(argument.substr(0, attributeStart));
		const auto lastSpace = declaration.find_last_of(" \t&*");
		if (lastSpace == std::string::npos) return fail("unnamed TES parameter");
		const auto variable = declaration.substr(lastSpace + 1);
		if (variable.empty() || variable.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") != std::string::npos)
			return fail("unsupported TES parameter name");
		const auto attribute = trim(argument.substr(attributeStart + 2, attributeEnd - attributeStart - 2));
		std::string value;
		if (capturedOutput && attribute == "buffer(27)")
		{
			outputBuffer = variable;
			value = "&lumen_output[lumen_ordinal]";
		}
		else if (capturedOutput && attribute == "buffer(24)")
		{
			indirectParameters = variable;
			continue;
		}
		else if (attribute == "patch_id" || attribute == "primitive_id") value = "lumen_patch";
		else if (attribute == "position_in_patch") value = triangles ?
		    "float3(lumen_uv.x, lumen_uv.y, 1.0f - lumen_uv.x - lumen_uv.y)" : "lumen_uv";
		else if (attribute == "instance_id") value = "lumen_config.instanceIndex";
		else if (attribute == "base_instance") value = "lumen_config.baseInstance";
		else if (attribute.rfind("buffer(", 0) == 0 || attribute.rfind("texture(", 0) == 0 ||
		         attribute.rfind("sampler(", 0) == 0)
		{
			kernelParameters += argument + ",\n";
			value = variable;
		}
		else return fail("unsupported TES builtin parameter");
		if (!helperParameters.empty()) helperParameters += ", ";
		helperParameters += declaration;
		if (!callArguments.empty()) callArguments += ", ";
		callArguments += value;
	}
	// SPIRV-Cross prefixes a TES with [[patch(domain, controlPoints)]]. Remove
	// only that entry-point annotation; output field attributes remain available
	// to the subsequent mesh interface builder.
	size_t declarationStart = qualifier;
	const auto patch = original.rfind("[[", qualifier);
	if (patch != std::string::npos)
	{
		const auto close = original.find("]]", patch);
		if (close != std::string::npos && close < qualifier &&
		    trim(original.substr(patch + 2, close - patch - 2)).rfind("patch(", 0) == 0 &&
		    trim(original.substr(close + 2, qualifier - close - 2)).empty())
			declarationStart = patch;
	}
	std::string body = original.substr(end);
	if (capturedOutput)
	{
		// SPIRV-Cross's documented capture mode preserves builtin arrays and
		// their device address space. Its generic VS invocation index has no
		// meaning for TES: the wrapper supplies one unique output record instead.
		if (outputBuffer.empty() || indirectParameters.empty()) return fail("missing TES capture parameters");
		const auto alias = body.find("device " + outputType + "& ");
		const auto bufferIndex = body.find(" = " + outputBuffer + "[", alias);
		if (alias == std::string::npos || bufferIndex == std::string::npos ||
		    body.find(';', alias) < bufferIndex) return fail("missing captured output alias");
		const auto bracket = body.find('[', bufferIndex);
		size_t close = bracket + 1;
		uint32_t nesting = 1;
		for (; close < body.size() && nesting != 0; ++close)
		{
			if (body[close] == '[') ++nesting;
			else if (body[close] == ']') --nesting;
		}
		if (nesting != 0 || close >= body.size() || body[close] != ';') return fail("invalid captured output alias");
		body.replace(bracket, close - bracket, "[0]");
		if (body.find(indirectParameters) != std::string::npos) return fail("unexpected indirect TES parameter use");
	}
	std::string source = original.substr(0, declarationStart) + "inline " +
	    (capturedOutput ? "void" : outputType) + " " + entry + "(" + helperParameters + ")" + body;
	source += "\nstruct LumenTessCaptureParams { uint pointStride, patchCount, instanceIndex, baseInstance; };\n";
	source += "kernel void lumen_capture_tessellation(\n" + kernelParameters +
	    "device const float2* lumen_coordinates [[buffer(21)]],\n"
	    "device const uint4* lumen_counts [[buffer(24)]],\n"
	    "constant LumenTessCaptureParams& lumen_config [[buffer(26)]],\n"
	    "device " + outputType + "* lumen_output [[buffer(27)]],\n"
	    "uint lumen_ordinal [[thread_position_in_grid]]) {\n"
	    "if (lumen_config.pointStride == 0) return;\n"
	    "const uint lumen_patch = lumen_ordinal / lumen_config.pointStride;\n"
	    "const uint lumen_point = lumen_ordinal % lumen_config.pointStride;\n"
	    "if (lumen_patch >= lumen_config.patchCount) return;\n"
	    "const uint4 lumen_count = lumen_counts[lumen_patch];\n"
	    "if (lumen_count.z != 0 || lumen_point >= lumen_count.x || lumen_count.x > lumen_config.pointStride) return;\n"
	    "const float2 lumen_uv = lumen_coordinates[lumen_ordinal];\n";
	if (!capturedOutput) source += "lumen_output[lumen_ordinal] = ";
	source += entry + "(" + callArguments + ");\n}\n";
	result->source = source;
	// CullDistance has no MSL builtin. The plain captured member survives; mesh
	// assembly reads it using reflected SPIR-V metadata before rasterization.
	for (size_t cursor = 0; (cursor = source.find("[[cull_distance]]", cursor)) != std::string::npos;)
		source.erase(cursor, sizeof("[[cull_distance]]") - 1);
	if (@available(macOS 13.0, iOS 16.0, *))
	{
		MTLCompileOptions* options = [MTLCompileOptions new];
		options.languageVersion = MTLLanguageVersion3_0;
		if (@available(macOS 15.0, iOS 18.0, *)) options.mathMode = MTLMathModeSafe;
		else
		{
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
			options.fastMathEnabled = NO;
#pragma clang diagnostic pop
		}
		NSError* error = nil;
		id<MTLLibrary> library = [device newLibraryWithSource:[[NSString alloc] initWithBytes:source.data()
		    length:source.size() encoding:NSUTF8StringEncoding] options:options error:&error];
		if (library == nil)
		{
			fprintf(stderr, "[Metal] TES capture compilation: %s\n", error.localizedDescription.UTF8String);
			return VK_ERROR_INVALID_SHADER_NV;
		}
		MTLComputePipelineReflection* reflection = nil;
		result->pipeline = [device newComputePipelineStateWithFunction:[library newFunctionWithName:@"lumen_capture_tessellation"]
		    options:MTLPipelineOptionBindingInfo | MTLPipelineOptionBufferTypeInfo reflection:&reflection error:&error];
		if (result->pipeline == nil)
		{
			fprintf(stderr, "[Metal] TES capture pipeline: %s\n", error.localizedDescription.UTF8String);
			return VK_ERROR_INVALID_SHADER_NV;
		}
		for (id<MTLBinding> argument in reflection.bindings)
		{
			if (argument.type == MTLBindingTypeBuffer && argument.index == 27)
				result->outputStride = static_cast<uint32_t>(((id<MTLBufferBinding>)argument).bufferDataSize);
		}
		if (result->outputStride == 0) return fail("missing captured output stride");
		return VK_SUCCESS;
	}
	return VK_ERROR_FEATURE_NOT_PRESENT;
}
