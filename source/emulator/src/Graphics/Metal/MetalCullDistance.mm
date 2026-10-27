#include "MetalCullDistance.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <sstream>
#include <vector>
#include <spirv.hpp>

namespace Lumen::MetalVulkan {
namespace {

struct OutputField
{
	std::string type;
	std::string name;
	std::string attributes;
	uint32_t arraySize = 0;
};

std::string Trim(std::string value)
{
	const auto first = value.find_first_not_of(" \t\r\n");
	if (first == std::string::npos) return {};
	return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}

bool Identifier(const std::string& value)
{
	if (value.empty() || !(std::isalpha(static_cast<unsigned char>(value[0])) || value[0] == '_')) return false;
	return std::all_of(value.begin() + 1, value.end(), [](unsigned char c) { return std::isalnum(c) || c == '_'; });
}

bool ParseField(std::string declaration, OutputField* field)
{
	const auto attribute = declaration.find("[[");
	if (attribute != std::string::npos)
	{
		const auto end = declaration.find("]]", attribute);
		if (end == std::string::npos) return false;
		field->attributes = declaration.substr(attribute, end + 2 - attribute);
		declaration.erase(attribute, end + 2 - attribute);
	}
	const auto array = declaration.find('[');
	if (array != std::string::npos)
	{
		const auto end = declaration.find(']', array);
		if (end == std::string::npos || !Trim(declaration.substr(end + 1)).empty()) return false;
		const auto count = Trim(declaration.substr(array + 1, end - array - 1));
		if (count.empty()) return false;
		uint32_t value = 0;
		for (unsigned char c: count)
		{
			if (!std::isdigit(c) || value > 128) return false;
			value = value * 10 + c - '0';
		}
		if (value == 0 || value > 128) return false;
		field->arraySize = value;
		declaration.erase(array);
	}
	std::istringstream words(declaration);
	std::string extra;
	return bool(words >> field->type >> field->name) && !(words >> extra) &&
	       Identifier(field->type) && Identifier(field->name);
}

bool PrimitiveField(const OutputField& field)
{
	return field.attributes.find("render_target_array_index") != std::string::npos ||
	       field.attributes.find("viewport_array_index") != std::string::npos;
}

void Declare(std::ostringstream& source, const OutputField& field, bool attributes = true)
{
	source << "    " << field.type << " " << field.name;
	if (attributes && !field.attributes.empty()) source << " " << field.attributes;
	if (field.arraySize != 0) source << " [" << field.arraySize << "]";
	source << ";\n";
}

void Copy(std::ostringstream& source, const OutputField& field, const char* destination,
	const char* input)
{
	if (field.arraySize == 0)
		source << "    " << destination << "." << field.name << " = " << input << "." << field.name << ";\n";
	else
		for (uint32_t i = 0; i < field.arraySize; ++i)
			source << "    " << destination << "." << field.name << "[" << i << "] = " << input << "."
			       << field.name << "[" << i << "];\n";
}

VkResult BuildSource(const std::string& captureSource, const std::string& outputType,
	const std::vector<MetalCullOutputField>& outputFields, VkPrimitiveTopology topology,
	MetalCullMeshShader* result, std::string* diagnostic)
{
	const auto fail = [&](const char* reason) {
		if (diagnostic != nullptr) *diagnostic = reason;
		return VK_ERROR_FEATURE_NOT_PRESENT;
	};
	if (!Identifier(outputType)) return fail("invalid captured output type");
	const char* meshTopology = nullptr;
	uint32_t vertices = 0;
	switch (topology)
	{
		case VK_PRIMITIVE_TOPOLOGY_POINT_LIST: meshTopology = "point"; vertices = 1; break;
		case VK_PRIMITIVE_TOPOLOGY_LINE_LIST:
		case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP: meshTopology = "line"; vertices = 2; break;
		case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST:
		case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP:
		case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN: meshTopology = "triangle"; vertices = 3; break;
		default: return fail("unsupported cull-distance primitive topology");
	}
	const auto structure = captureSource.find("struct " + outputType + "\n");
	if (structure == std::string::npos) return fail("captured output struct is absent");
	const auto begin = captureSource.find('{', structure);
	const auto end = captureSource.find("};", begin);
	if (begin == std::string::npos || end == std::string::npos) return fail("invalid captured output struct");
	std::vector<OutputField> fields;
	std::string cullName;
	std::string positionName;
	uint32_t cullCount = 0, clipCount = 0;
	bool position = false;
	for (size_t cursor = begin + 1; cursor < end;)
	{
		const auto semicolon = captureSource.find(';', cursor);
		if (semicolon == std::string::npos || semicolon > end)
		{
			if (!Trim(captureSource.substr(cursor, end - cursor)).empty()) return fail("invalid output declaration");
			break;
		}
		OutputField field;
		if (!ParseField(Trim(captureSource.substr(cursor, semicolon - cursor)), &field))
			return fail("unsupported captured output field");
		if (!outputFields.empty())
		{
			const auto reflected = std::find_if(outputFields.begin(), outputFields.end(),
			    [&](const MetalCullOutputField& value) { return value.name == field.name; });
			if (reflected == outputFields.end()) return fail("captured output field lacks interface reflection");
			if ((reflected->key & 0x80000000u) != 0)
			{
				switch (static_cast<spv::BuiltIn>(reflected->key & 0x7fffffffu))
				{
					case spv::BuiltInPosition: field.attributes = "[[position]]"; break;
					case spv::BuiltInPointSize: field.attributes = "[[point_size]]"; break;
					case spv::BuiltInClipDistance: field.attributes = "[[clip_distance]]"; break;
					case spv::BuiltInCullDistance: field.attributes = "[[cull_distance]]"; break;
					case spv::BuiltInLayer: field.attributes = "[[render_target_array_index]]"; break;
					case spv::BuiltInViewportIndex: field.attributes = "[[viewport_array_index]]"; break;
					default: return fail("unsupported captured output builtin");
				}
			}
			else
			{
				field.attributes = "[[user(locn" + std::to_string(reflected->key) + ")";
				const auto flags = reflected->interpolationFlags;
				if ((flags & 1) != 0) field.attributes += ", flat";
				else if ((flags & 8) != 0) field.attributes += (flags & 2) ? ", sample_no_perspective" : ", sample_perspective";
				else if ((flags & 4) != 0) field.attributes += (flags & 2) ? ", centroid_no_perspective" : ", centroid_perspective";
				else if ((flags & 2) != 0) field.attributes += ", center_no_perspective";
				field.attributes += "]]";
			}
		}
		if (field.attributes.find("cull_distance") != std::string::npos)
		{
			if (cullCount != 0 || field.type != "float" || field.arraySize == 0 || field.arraySize > 8)
				return fail("invalid cull distance array");
			cullName = field.name;
			cullCount = field.arraySize;
		}
		if (field.attributes.find("clip_distance") != std::string::npos) clipCount += field.arraySize;
		position |= field.attributes.find("[[position]]") != std::string::npos;
		if (field.attributes.find("[[position]]") != std::string::npos) positionName = field.name;
		if (PrimitiveField(field) && field.arraySize != 0) return fail("invalid per-primitive output array");
		fields.push_back(std::move(field));
		cursor = semicolon + 1;
	}
	if (!position || cullCount == 0 || cullCount + clipCount > 8)
		return fail("missing position/cull distances or combined distance limit exceeded");
	std::ostringstream source;
	source << captureSource.substr(0, begin + 1) << "\n";
	// Keep captured records as plain data, independently of raster interface attributes.
	for (const auto& field: fields) Declare(source, field, false);
	source << captureSource.substr(end);
	source << "\nstruct LumenCullVertex {\n";
	for (const auto& field: fields)
	{
		if (field.name == cullName)
		{
			for (uint32_t i = 0; i < cullCount; ++i)
				source << "    float lumen_cull_value_" << i << " [[user(cull" << i << ")]];\n";
		}
		else if (!PrimitiveField(field)) Declare(source, field);
	}
	source << "};\nstruct LumenCullPrimitive {\n    uint lumen_primitive_id [[primitive_id]];\n";
	for (const auto& field: fields) if (PrimitiveField(field)) Declare(source, field);
	source << "};\nstruct LumenCullDrawParams {\n"
	       << "    uint vertexCount, primitiveCount, firstPrimitive, flags;\n"
	       << "    uint indexCount, indexOffset, vertexOffset, reserved;\n};\n"
	       << "[[mesh, max_total_threads_per_threadgroup(1)]]\n"
	       << "void lumen_cull_mesh(metal::mesh<LumenCullVertex, LumenCullPrimitive, " << vertices
	       << ", 1, topology::" << meshTopology << "> output,\n"
	       << "    device const " << outputType << "* captured [[buffer(" << kCullCaptureBufferIndex << ")]],\n"
	       << "    device const uint* indices [[buffer(" << kCullIndexBufferIndex << ")]],\n"
	       << "    device const uint4* patchCounts [[buffer(" << kCullPatchCountsBufferIndex << ")]],\n"
	       << "    constant LumenCullDrawParams& params [[buffer(" << kCullDrawParamsBufferIndex << ")]],\n"
	       << "    uint3 group [[threadgroup_position_in_grid]]) {\n"
	       << "    uint primitive = group.x;\n"
	       << "    if (primitive >= params.primitiveCount) { output.set_primitive_count(0); return; }\n"
	       << "    uint vertexBase = params.vertexOffset, indexBase = params.indexOffset;\n"
	       << "    uint vertexCount = params.vertexCount, indexCount = params.indexCount;\n"
	       << "    uint primitiveID = params.firstPrimitive + primitive;\n"
	       << "    if ((params.flags & 2) != 0) {\n"
	       << "        const uint slots = params.indexCount / 3;\n"
	       << "        if (slots == 0) { output.set_primitive_count(0); return; }\n"
	       << "        const uint patch = primitive / slots;\n"
	       << "        primitive %= slots;\n"
	       << "        if (patch >= params.reserved) { output.set_primitive_count(0); return; }\n"
	       << "        const uint4 counts = patchCounts[patch];\n"
	       << "        if (counts.z != 0 || counts.x > params.vertexCount || counts.y > params.indexCount ||"
	       << " primitive >= counts.y / 3) { output.set_primitive_count(0); return; }\n"
	       << "        vertexBase += patch * params.vertexCount; indexBase += patch * params.indexCount;\n"
	       << "        vertexCount = counts.x; indexCount = counts.y; primitiveID = params.firstPrimitive + patch;\n"
	       << "    }\n"
	       << "    uint ordinal[" << vertices << "];\n";
	switch (topology)
	{
		case VK_PRIMITIVE_TOPOLOGY_POINT_LIST: source << "    ordinal[0] = primitive;\n"; break;
		case VK_PRIMITIVE_TOPOLOGY_LINE_LIST: source << "    ordinal[0] = primitive * 2; ordinal[1] = ordinal[0] + 1;\n"; break;
		case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP: source << "    ordinal[0] = primitive; ordinal[1] = primitive + 1;\n"; break;
		case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST:
			source << "    ordinal[0] = primitive * 3; ordinal[1] = ordinal[0] + 1; ordinal[2] = ordinal[0] + 2;\n"; break;
		case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP:
			// A cyclic rotation preserves the first provoking vertex on odd triangles
			// while reversing their winding relative to the even triangles.
			source << "    ordinal[0] = primitive; ordinal[1] = primitive + ((primitive & 1) ? 2 : 1);"
			       << " ordinal[2] = primitive + ((primitive & 1) ? 1 : 2);\n"; break;
		case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN:
			source << "    ordinal[0] = primitive + 1; ordinal[1] = primitive + 2; ordinal[2] = 0;\n"; break;
		default: return fail("unreachable primitive topology");
	}
	source << "    for (uint v = 0; v < " << vertices << "; ++v) {\n"
	       << "        if ((params.flags & 1) != 0) {\n"
	       << "            if (ordinal[v] >= indexCount) { output.set_primitive_count(0); return; }\n"
	       << "            ordinal[v] = indices[indexBase + ordinal[v]];\n        }\n"
	       << "        if (ordinal[v] >= vertexCount) { output.set_primitive_count(0); return; }\n"
	       << "        ordinal[v] += vertexBase;\n    }\n"
	       << "    bool culled = false;\n"
	       << "    for (uint component = 0; component < " << cullCount << "; ++component) {\n"
	       << "        bool allNegative = true;\n"
	       << "        for (uint v = 0; v < " << vertices << "; ++v)\n"
	       << "            allNegative = allNegative && (captured[ordinal[v]]." << cullName << "[component] < 0.0f);\n"
	       << "        culled = culled || allNegative;\n    }\n"
	       << "    if (culled) { output.set_primitive_count(0); return; }\n"
	       << "    for (uint v = 0; v < " << vertices << "; ++v) {\n"
	       << "        const device " << outputType << "& input = captured[ordinal[v]];\n"
	       << "        LumenCullVertex vertexOutput;\n";
	for (const auto& field: fields)
	{
		if (field.name == cullName)
		{
			for (uint32_t i = 0; i < cullCount; ++i)
				source << "    vertexOutput.lumen_cull_value_" << i << " = input." << cullName << "[" << i << "];\n";
		}
		else if (!PrimitiveField(field)) Copy(source, field, "vertexOutput", "input");
	}
	source << "        if ((params.flags & 16) != 0) vertexOutput." << positionName << ".y = -vertexOutput." << positionName << ".y;\n"
	       << "        if ((params.flags & 32) != 0) vertexOutput." << positionName << ".z = (vertexOutput." << positionName << ".z + vertexOutput." << positionName << ".w) * 0.5f;\n"
	       << "        output.set_vertex(v, vertexOutput); output.set_index(v, v);\n    }\n"
	       << "    LumenCullPrimitive primitiveData;\n"
	       << "    primitiveData.lumen_primitive_id = primitiveID;\n"
	       << "    const device " << outputType << "& first = captured[ordinal[0]];\n";
	for (const auto& field: fields) if (PrimitiveField(field)) Copy(source, field, "primitiveData", "first");
	source << "    output.set_primitive(0, primitiveData);\n    output.set_primitive_count(1);\n}\n";
	result->source = source.str();
	result->cullDistanceCount = cullCount;
	result->verticesPerPrimitive = vertices;
	return VK_SUCCESS;
}

} // namespace

bool MetalCullDistanceSupported(id<MTLDevice> device)
{
	if (@available(macOS 13.0, iOS 16.0, *))
		return device != nil && [device supportsFamily:MTLGPUFamilyMetal3];
	return false;
}

uint32_t MetalCullPrimitiveCount(VkPrimitiveTopology topology, uint32_t vertexCount)
{
	switch (topology)
	{
		case VK_PRIMITIVE_TOPOLOGY_POINT_LIST: return vertexCount;
		case VK_PRIMITIVE_TOPOLOGY_LINE_LIST: return vertexCount / 2;
		case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP: return vertexCount > 1 ? vertexCount - 1 : 0;
		case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST: return vertexCount / 3;
		case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP:
		case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN: return vertexCount > 2 ? vertexCount - 2 : 0;
		default: return 0;
	}
}

VkResult CreateMetalCullMeshShader(id<MTLDevice> device, const std::string& captureSource,
	const std::string& outputType, const std::vector<MetalCullOutputField>& outputFields,
	VkPrimitiveTopology topology, MetalCullMeshShader* result,
	std::string* diagnostic)
{
	if (result == nullptr || !MetalCullDistanceSupported(device)) return VK_ERROR_FEATURE_NOT_PRESENT;
	*result = {};
	const auto built = BuildSource(captureSource, outputType, outputFields, topology, result, diagnostic);
	if (built != VK_SUCCESS) return built;
	if (@available(macOS 13.0, iOS 16.0, *))
	{
		MTLCompileOptions* options = [[MTLCompileOptions alloc] init];
		options.languageVersion = MTLLanguageVersion3_0;
		if (@available(macOS 15.0, iOS 18.0, *)) options.mathMode = MTLMathModeSafe;
		else
		{
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
			options.fastMathEnabled = NO;
#pragma clang diagnostic pop
		}
		NSString* source = [[NSString alloc] initWithBytes:result->source.data() length:result->source.size()
		                                         encoding:NSUTF8StringEncoding];
		NSError* error = nil;
		id<MTLLibrary> library = [device newLibraryWithSource:source options:options error:&error];
		if (library == nil)
		{
			if (diagnostic != nullptr) *diagnostic = error.localizedDescription.UTF8String ?: "Metal cull mesh compilation failed";
			return VK_ERROR_INVALID_SHADER_NV;
		}
		result->function = [library newFunctionWithName:@"lumen_cull_mesh"];
		if (result->function == nil) return VK_ERROR_INVALID_SHADER_NV;
		return VK_SUCCESS;
	}
	return VK_ERROR_FEATURE_NOT_PRESENT;
}

} // namespace Lumen::MetalVulkan
