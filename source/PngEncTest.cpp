#include "Lumen/Core/Compression.h"
#include "Lumen/Core/Threads.h"
#include "Lumen/Core/VirtualMemory.h"

#include "Emulator/Config.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Libs/PngDec.h"
#include "Emulator/Libs/PngEnc.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "Emulator/Log.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <thread>
#include <vector>
#if defined(LUMEN_FEXCORE_RUNTIME) && defined(__APPLE__)
#include <unistd.h>
#endif

namespace {

namespace Dec = Lumen::Libs::PngDec;
namespace Enc = Lumen::Libs::PngEnc;

constexpr uint8_t CANARY = 0xa5;
constexpr size_t GUARD = 16;
constexpr size_t PNG_CAPACITY = 4096;

constexpr std::array<uint8_t, 16> RGBA {{
	255, 0, 0, 255, 0, 255, 0, 128,
	0, 0, 255, 64, 255, 255, 255, 0,
}};
constexpr std::array<uint8_t, 16> BGRA {{
	0, 0, 255, 255, 0, 255, 0, 128,
	255, 0, 0, 64, 255, 255, 255, 0,
}};
constexpr std::array<uint8_t, 16> RGB_AS_RGBA {{
	255, 0, 0, 255, 0, 255, 0, 255,
	0, 0, 255, 255, 255, 255, 255, 255,
}};

struct alignas(8) GuardedWorkspace
{
	uint64_t before = 0x7f42d18b93a65ce0ull;
	std::array<uint8_t, 16> bytes {};
	uint64_t after = 0x7f42d18b93a65ce0ull;

	bool Intact() const
	{
		return before == 0x7f42d18b93a65ce0ull && after == 0x7f42d18b93a65ce0ull;
	}
};

struct GuardedOutput
{
	std::array<uint8_t, GUARD + PNG_CAPACITY + GUARD> bytes {};

	GuardedOutput() { bytes.fill(CANARY); }
	uint8_t* Data() { return bytes.data() + GUARD; }
	const uint8_t* Data() const { return bytes.data() + GUARD; }
	bool AllCanary() const
	{
		return std::all_of(bytes.begin(), bytes.end(), [](uint8_t value) { return value == CANARY; });
	}
	bool GuardsIntact() const
	{
		return std::all_of(bytes.begin(), bytes.begin() + GUARD, [](uint8_t value) { return value == CANARY; }) &&
		       std::all_of(bytes.end() - GUARD, bytes.end(), [](uint8_t value) { return value == CANARY; });
	}
};

struct GuardedInfo
{
	uint64_t before = 0xa1749c3e2d605bf8ull;
	Enc::OutputInfo info {0x31415926u, 0x27182818u};
	uint64_t after = 0xa1749c3e2d605bf8ull;

	bool Unchanged() const
	{
		return before == 0xa1749c3e2d605bf8ull && after == before &&
		       info.data_size == 0x31415926u && info.processed_height == 0x27182818u;
	}
};

bool Check(bool condition, const char* message)
{
	if (!condition) std::fprintf(stderr, "PngEncTest: %s\n", message);
	return condition;
}

uint32_t ReadBigEndian32ForTest(const uint8_t* data)
{
	return (uint32_t(data[0]) << 24u) | (uint32_t(data[1]) << 16u) |
	       (uint32_t(data[2]) << 8u) | uint32_t(data[3]);
}

uint32_t Crc32ForTest(const uint8_t* data, size_t size)
{
	std::array<uint32_t, 256> table {};
	for (uint32_t value = 0; value < table.size(); ++value)
	{
		uint32_t entry = value;
		for (uint32_t bit = 0; bit < 8; ++bit)
			entry = (entry >> 1u) ^ ((entry & 1u) != 0 ? 0xedb88320u : 0u);
		table[value] = entry;
	}

	uint32_t crc = 0xffffffffu;
	for (size_t index = 0; index < size; ++index)
		crc = (crc >> 8u) ^ table[(crc ^ data[index]) & 0xffu];
	return ~crc;
}

bool ValidatePngChunks(const uint8_t* png, uint32_t png_size)
{
	constexpr std::array<uint8_t, 8> signature {{137, 80, 78, 71, 13, 10, 26, 10}};
	if (!Check(png != nullptr && png_size >= signature.size() + 12,
	           "encoded PNG is too short for its container") ||
	    !Check(std::equal(signature.begin(), signature.end(), png),
	           "encoded PNG signature is incorrect"))
	{
		return false;
	}

	size_t offset = signature.size();
	bool saw_ihdr = false;
	bool saw_idat = false;
	bool saw_iend = false;
	while (offset < png_size)
	{
		const size_t remaining = static_cast<size_t>(png_size) - offset;
		if (!Check(remaining >= 12, "encoded PNG has a truncated chunk header")) return false;
		const uint32_t data_size = ReadBigEndian32ForTest(png + offset);
		const uint64_t chunk_size = uint64_t(data_size) + 12u;
		if (!Check(chunk_size <= remaining, "encoded PNG chunk exceeds the container")) return false;

		const uint8_t* type = png + offset + 4;
		const bool is_ihdr = std::memcmp(type, "IHDR", 4) == 0;
		const bool is_idat = std::memcmp(type, "IDAT", 4) == 0;
		const bool is_iend = std::memcmp(type, "IEND", 4) == 0;
		if (!saw_ihdr)
		{
			if (!Check(is_ihdr && data_size == 13, "IHDR is absent, misplaced or malformed")) return false;
			saw_ihdr = true;
		} else if (is_idat && !saw_iend)
		{
			saw_idat = true;
		} else if (is_iend && !saw_iend)
		{
			if (!Check(saw_idat && data_size == 0, "IEND precedes IDAT or carries data")) return false;
			saw_iend = true;
		} else
		{
			return Check(false, "encoded PNG has an unexpected or misplaced chunk");
		}

		const size_t crc_offset = offset + 8u + data_size;
		const uint32_t stored_crc = ReadBigEndian32ForTest(png + crc_offset);
		const uint32_t computed_crc = Crc32ForTest(type, static_cast<size_t>(data_size) + 4u);
		if (!Check(stored_crc == computed_crc, "encoded PNG chunk CRC is incorrect")) return false;

		offset += static_cast<size_t>(chunk_size);
		if (saw_iend) break;
	}
	return Check(saw_ihdr && saw_idat && saw_iend && offset == png_size,
	             "encoded PNG chunk sequence is incomplete or has trailing data");
}

Lumen::Loader::SymbolResolve Exact(const char* nid)
{
	Lumen::Loader::SymbolResolve query {};
	query.name = nid;
	query.library = "libScePngEnc";
	query.library_version = 1;
	query.module = "libScePngEnc";
	query.module_version_major = 1;
	query.module_version_minor = 1;
	query.type = Lumen::Loader::SymbolType::Func;
	return query;
}

bool TestBindings()
{
	Lumen::Loader::SymbolDatabase symbols;
	if (!Check(Lumen::Libs::Init(U"libPngEnc_1", &symbols), "libPngEnc_1 registration failed")) return false;
	struct Binding { const char* nid; uint64_t target; };
	const Binding bindings[] = {
		{"9030RnBDoh4", reinterpret_cast<uint64_t>(&Enc::QueryMemorySize)},
		{"7aGTPfrqT9s", reinterpret_cast<uint64_t>(&Enc::Create)},
		{"xgDjJKpcyHo", reinterpret_cast<uint64_t>(&Enc::Encode)},
		{"RUrWdwTWZy8", reinterpret_cast<uint64_t>(&Enc::Delete)},
	};
	for (const auto& binding: bindings)
	{
		const auto* found = symbols.Find(Exact(binding.nid));
		if (!Check(found != nullptr && found->vaddr == binding.target, "exact PngEnc binding is absent or incorrect"))
			return false;
		for (int version: {0, 2})
		{
			auto wrong = Exact(binding.nid);
			wrong.library_version = version;
			if (!Check(symbols.Find(wrong) == nullptr, "binding accepted wrong library version")) return false;
			wrong = Exact(binding.nid);
			wrong.module_version_major = version;
			if (!Check(symbols.Find(wrong) == nullptr, "binding accepted wrong module major")) return false;
			wrong = Exact(binding.nid);
			wrong.module_version_minor = version;
			if (!Check(symbols.Find(wrong) == nullptr, "binding accepted wrong module minor")) return false;
		}
		auto wrong = Exact(binding.nid);
		wrong.library = "libScePngDec";
		if (!Check(symbols.Find(wrong) == nullptr, "binding escaped the PngEnc library")) return false;
		wrong = Exact(binding.nid);
		wrong.module = "libScePngDec";
		if (!Check(symbols.Find(wrong) == nullptr, "binding escaped the PngEnc module")) return false;
	}
	return Check(symbols.Find(Exact("unknown-pngenc-nid")) == nullptr, "unknown PngEnc NID resolved");
}

bool CreateEncoder(GuardedWorkspace& workspace, Enc::Handle& handle, uint32_t max_width = 2)
{
	Enc::CreateInput input {sizeof(input), 0, max_width, 5};
	return Check(Enc::QueryMemorySize(&input) == 16, "valid memory query did not return 16") &&
	       Check(Enc::Create(&input, workspace.bytes.data(), workspace.bytes.size(), &handle) == 0,
	             "valid encoder creation failed") &&
	       Check(handle == workspace.bytes.data(), "encoder handle does not equal workspace") &&
	       Check([&]
	       {
		       uint64_t words[2] {};
		       std::memcpy(words, workspace.bytes.data(), sizeof(words));
		       return words[0] == 0x31434e45474e5050ull && words[1] != 0 &&
		              words[0] != reinterpret_cast<uintptr_t>(handle) &&
		              words[1] != reinterpret_cast<uintptr_t>(handle);
	       }(), "workspace contains neither the deterministic record nor pointer-free state") &&
	       Check(workspace.Intact(), "create wrote outside the workspace");
}

Enc::EncodeInput RgbaInput(GuardedOutput& output, uint32_t capacity = PNG_CAPACITY)
{
	return Enc::EncodeInput {RGBA.data(), output.Data(), static_cast<uint32_t>(RGBA.size()), capacity,
	                         2, 2, 8, 0, 19, 8, 0, 0, 6};
}

bool DecodeMatches(const uint8_t* png, uint32_t png_size, uint16_t color,
	               const std::array<uint8_t, 16>& expected)
{
	if (!ValidatePngChunks(png, png_size)) return false;
	Dec::ParseInput parse {png, png_size, 0};
	Dec::ImageInfo parsed {};
	if (!Check(Dec::ParseHeader(&parse, &parsed) == 0, "encoded PNG header was rejected") ||
	    !Check(parsed.width == 2 && parsed.height == 2 && parsed.color == color && parsed.depth == 8,
	           "encoded PNG metadata is incorrect")) return false;

	Dec::CreateInput create {sizeof(create), 0, 2};
	alignas(8) std::array<uint8_t, 16> workspace {};
	Dec::Handle decoder = nullptr;
	if (!Check(Dec::Create(&create, workspace.data(), workspace.size(), &decoder) == 0,
	           "round-trip decoder creation failed")) return false;
	std::array<uint8_t, 16> pixels {};
	Dec::DecodeInput decode {png, pixels.data(), png_size, pixels.size(), 0, 255, 0};
	Dec::ImageInfo decoded {};
	const bool passed = Check(Dec::Decode(decoder, &decode, &decoded) == 0x00020002,
	                          "encoded PNG did not round-trip") &&
	                    Check(pixels == expected, "round-trip pixels differ") &&
	                    Check(Dec::Delete(decoder) == 0, "round-trip decoder delete failed");
	return passed;
}

bool TestQueryCreateAndLifecycle()
{
	Enc::ResetSessionState();
	Enc::CreateInput valid {sizeof(valid), 0, 2, 5};
	if (!Check(Enc::QueryMemorySize(nullptr) == Enc::InvalidAddress, "null query input precedence changed")) return false;
	auto invalid = valid;
	invalid.size = 0;
	if (!Check(Enc::QueryMemorySize(&invalid) == Enc::InvalidParam, "invalid size marker was accepted")) return false;
	invalid = valid;
	invalid.attributes = 1;
	if (!Check(Enc::QueryMemorySize(&invalid) == Enc::InvalidParam, "unknown attribute was accepted")) return false;
	invalid = valid;
	invalid.max_filters = 6;
	if (!Check(Enc::QueryMemorySize(&invalid) == Enc::InvalidParam, "excess filter count was accepted")) return false;
	invalid = valid;
	invalid.max_width = 0;
	if (!Check(Enc::QueryMemorySize(&invalid) == Enc::InvalidSize, "zero maximum width was accepted")) return false;
	invalid.max_width = 1000002;
	if (!Check(Enc::QueryMemorySize(&invalid) == Enc::InvalidSize, "excess maximum width was accepted")) return false;

	GuardedWorkspace workspace;
	workspace.bytes.fill(CANARY);
	Enc::Handle handle = reinterpret_cast<Enc::Handle>(uintptr_t {0x1234});
	if (!Check(Enc::Create(nullptr, workspace.bytes.data(), 16, &handle) == Enc::InvalidAddress,
	           "null create input was not rejected") ||
	    !Check(handle == reinterpret_cast<Enc::Handle>(uintptr_t {0x1234}) &&
	           std::all_of(workspace.bytes.begin(), workspace.bytes.end(), [](uint8_t value) { return value == CANARY; }),
	           "null-input create changed workspace or handle")) return false;
	// The deliberately explicit checks below keep both outputs unchanged on every normal error.
	workspace.bytes.fill(CANARY);
	handle = reinterpret_cast<Enc::Handle>(uintptr_t {0x1234});
	if (!Check(Enc::Create(&valid, nullptr, 16, &handle) == Enc::InvalidAddress, "null workspace was not rejected") ||
	    !Check(Enc::Create(&valid, workspace.bytes.data(), 16, nullptr) == Enc::InvalidAddress,
	           "null handle slot was not rejected") ||
	    !Check(Enc::Create(&valid, workspace.bytes.data(), 15, &handle) == Enc::InvalidSize,
	           "short workspace was accepted") ||
	    !Check(handle == reinterpret_cast<Enc::Handle>(uintptr_t {0x1234}) &&
	           std::all_of(workspace.bytes.begin(), workspace.bytes.end(), [](uint8_t value) { return value == CANARY; }),
	           "failed create changed workspace or handle")) return false;

	if (!CreateEncoder(workspace, handle)) return false;
	Enc::Handle duplicate = nullptr;
	if (!Check(Enc::Create(&valid, workspace.bytes.data(), 16, &duplicate) == Enc::InvalidHandle,
	           "live workspace was reused") ||
	    !Check(duplicate == nullptr, "duplicate create published an handle") ||
	    !Check(Enc::Delete(handle) == 0, "delete failed") ||
	    !Check(Enc::Delete(handle) == Enc::InvalidHandle, "double delete succeeded") ||
	    !Check(Enc::Create(&valid, workspace.bytes.data(), 16, &duplicate) == Enc::InvalidHandle,
	           "tombstoned workspace was reused")) return false;
	Enc::ResetSessionState();
	return Check(Enc::Delete(handle) == Enc::InvalidHandle, "reset retained an old handle") &&
	       Check(CreateEncoder(workspace, handle), "workspace was not reusable after session reset") &&
	       Check(Enc::Delete(handle) == 0, "recreated handle could not be deleted") &&
	       Check(workspace.Intact(), "lifecycle wrote outside workspace guards");
}

bool TestFormatsFiltersAndCapacity()
{
	Enc::ResetSessionState();
	Dec::ResetSessionState();
	constexpr std::array<uint8_t, 9> crc_fixture {{'1', '2', '3', '4', '5', '6', '7', '8', '9'}};
	if (!Check(Crc32ForTest(crc_fixture.data(), crc_fixture.size()) == 0xcbf43926u,
	           "independent CRC32 oracle rejected its check vector")) return false;
	GuardedWorkspace workspace;
	Enc::Handle handle = nullptr;
	if (!CreateEncoder(workspace, handle)) return false;

	std::vector<uint8_t> sub_png;
	for (uint16_t filter: {uint16_t {0}, uint16_t {1}, uint16_t {2}, uint16_t {4}, uint16_t {8}, uint16_t {15}})
	{
		GuardedOutput output;
		GuardedInfo info;
		auto input = RgbaInput(output);
		input.filter = filter;
		const int32_t size = Enc::Encode(handle, &input, &info.info);
		if (!Check(size > 0 && info.info.data_size == static_cast<uint32_t>(size) &&
		           info.info.processed_height == 2, "RGBA encode result fields are inconsistent") ||
		    !Check(output.GuardsIntact(), "RGBA encode changed output guards") ||
		    !DecodeMatches(output.Data(), static_cast<uint32_t>(size), 19, RGBA)) return false;
		if (filter == 1)
			sub_png.assign(output.Data(), output.Data() + size);
		if (filter == 15 &&
		    !Check(sub_png.size() == static_cast<size_t>(size) &&
		           std::equal(sub_png.begin(), sub_png.end(), output.Data()),
		           "multi-filter mask did not select the first enabled Sub filter")) return false;
	}

	for (uint16_t format: {uint16_t {0}, uint16_t {1}})
	{
		std::array<uint8_t, 24> padded {};
		const auto& source = format == 0 ? RGBA : BGRA;
		std::copy_n(source.data(), 8, padded.data());
		std::copy_n(source.data() + 8, 8, padded.data() + 12);
		GuardedOutput output;
		GuardedInfo info;
		Enc::EncodeInput input {padded.data(), output.Data(), padded.size(), PNG_CAPACITY,
		                        2, 2, 12, format, 3, 8, 0, 15, 9};
		const int32_t size = Enc::Encode(handle, &input, &info.info);
		if (!Check(size > 0, "padded RGB encode failed") ||
		    !DecodeMatches(output.Data(), static_cast<uint32_t>(size), 3, RGB_AS_RGBA)) return false;
	}

	GuardedOutput bgra_rgba_output;
	GuardedInfo bgra_rgba_info;
	Enc::EncodeInput bgra_rgba {BGRA.data(), bgra_rgba_output.Data(), static_cast<uint32_t>(BGRA.size()), PNG_CAPACITY,
	                            2, 2, 8, 1, 19, 8, 0, 0, 6};
	const int32_t bgra_rgba_size = Enc::Encode(handle, &bgra_rgba, &bgra_rgba_info.info);
	if (!Check(bgra_rgba_size > 0 && bgra_rgba_info.info.data_size == static_cast<uint32_t>(bgra_rgba_size) &&
	           bgra_rgba_info.info.processed_height == 2, "BGRA-to-RGBA encode result fields are inconsistent") ||
	    !Check(bgra_rgba_output.GuardsIntact(), "BGRA-to-RGBA encode changed output guards") ||
	    !DecodeMatches(bgra_rgba_output.Data(), static_cast<uint32_t>(bgra_rgba_size), 19, RGBA)) return false;

	GuardedOutput first;
	GuardedInfo first_info;
	auto input = RgbaInput(first);
	const int32_t exact_size = Enc::Encode(handle, &input, &first_info.info);
	if (!Check(exact_size > 1, "baseline PNG has no useful capacity boundary")) return false;
	std::vector<uint8_t> exact(GUARD + exact_size + GUARD, CANARY);
	Enc::OutputInfo exact_info {};
	input.png = exact.data() + GUARD;
	input.png_size = static_cast<uint32_t>(exact_size);
	if (!Check(Enc::Encode(handle, &input, &exact_info) == exact_size, "exact output capacity failed") ||
	    !Check(std::equal(first.Data(), first.Data() + exact_size, exact.data() + GUARD),
	           "exact-capacity output is not deterministic") ||
	    !Check(std::all_of(exact.begin(), exact.begin() + GUARD, [](uint8_t value) { return value == CANARY; }) &&
	           std::all_of(exact.end() - GUARD, exact.end(), [](uint8_t value) { return value == CANARY; }),
	           "exact-capacity encode changed guards")) return false;

	std::vector<uint8_t> short_output(GUARD + exact_size - 1 + GUARD, CANARY);
	GuardedInfo short_info;
	input.png = short_output.data() + GUARD;
	input.png_size = static_cast<uint32_t>(exact_size - 1);
	if (!Check(Enc::Encode(handle, &input, &short_info.info) == Enc::DataOverflow,
	           "one-byte-short output did not overflow") ||
	    !Check(std::all_of(short_output.begin(), short_output.end(), [](uint8_t value) { return value == CANARY; }),
	           "overflow published a partial PNG") || !Check(short_info.Unchanged(), "overflow changed output info"))
		return false;

	GuardedOutput optional;
	input = RgbaInput(optional);
	return Check(Enc::Encode(handle, &input, nullptr) > 0, "optional output-info was required") &&
	       Check(Enc::Delete(handle) == 0, "format-test handle delete failed");
}

bool TestErrorsAndAtomicity()
{
	Enc::ResetSessionState();
	GuardedWorkspace workspace;
	Enc::Handle handle = nullptr;
	if (!CreateEncoder(workspace, handle)) return false;
	GuardedOutput output;
	GuardedInfo info;
	auto input = RgbaInput(output);
	if (!Check(Enc::Encode(nullptr, nullptr, &info.info) == Enc::InvalidHandle,
	           "handle did not have first precedence") ||
	    !Check(Enc::Encode(handle, nullptr, &info.info) == Enc::InvalidParam, "null record was not invalid-param"))
		return false;

	auto rejected = [&](const Enc::EncodeInput& candidate, int32_t expected, const char* message)
	{
		output.bytes.fill(CANARY);
		info = GuardedInfo {};
		return Check(Enc::Encode(handle, &candidate, &info.info) == expected, message) &&
		       Check(output.AllCanary(), "rejected encode changed PNG bytes") &&
		       Check(info.Unchanged(), "rejected encode changed output-info");
	};

	auto invalid = input;
	invalid.image = nullptr;
	if (!rejected(invalid, Enc::InvalidAddress, "null image was not rejected")) return false;
	invalid = input;
	invalid.png = nullptr;
	if (!rejected(invalid, Enc::InvalidAddress, "null PNG output was not rejected")) return false;
	invalid = input;
	invalid.image = reinterpret_cast<const void*>(std::numeric_limits<uintptr_t>::max() - 1u);
	invalid.width = 0;
	if (!rejected(invalid, Enc::InvalidAddress, "wrapping image range lost precedence over zero width")) return false;
	invalid = input;
	invalid.png = reinterpret_cast<void*>(std::numeric_limits<uintptr_t>::max() - 1u);
	invalid.height = 0;
	if (!rejected(invalid, Enc::InvalidAddress, "wrapping PNG range lost precedence over zero height")) return false;
	invalid = input;
	invalid.image_size = 0;
	if (!rejected(invalid, Enc::InvalidSize, "zero image size was not rejected")) return false;
	invalid = input;
	invalid.png_size = 0;
	if (!rejected(invalid, Enc::InvalidSize, "zero PNG capacity was not rejected")) return false;
	invalid = input;
	invalid.width = 3;
	if (!rejected(invalid, Enc::InvalidSize, "width over Create maximum was accepted")) return false;
	invalid = input;
	invalid.pitch = 9;
	if (!rejected(invalid, Enc::InvalidSize, "RGBA non-exact pitch was accepted")) return false;
	invalid = input;
	invalid.image_size = 15;
	if (!rejected(invalid, Enc::InvalidSize, "short image buffer was accepted")) return false;
	invalid = input;
	invalid.format = 2;
	if (!rejected(invalid, Enc::InvalidParam, "unknown pixel format was accepted")) return false;
	invalid = input;
	invalid.color = 4;
	if (!rejected(invalid, Enc::InvalidParam, "unknown color was accepted")) return false;
	invalid = input;
	invalid.depth = 16;
	if (!rejected(invalid, Enc::InvalidParam, "unqualified depth was accepted")) return false;
	invalid = input;
	invalid.clut_count = 1;
	if (!rejected(invalid, Enc::InvalidParam, "nonzero CLUT was accepted")) return false;
	invalid = input;
	invalid.compression = 10;
	if (!rejected(invalid, Enc::InvalidParam, "excess compression level was accepted")) return false;
	invalid = input;
	invalid.filter = 16;
	if (!rejected(invalid, Enc::InvalidParam, "unknown filter bit was accepted")) return false;

	invalid = input;
	invalid.png_size = 1;
	if (!rejected(invalid, Enc::DataOverflow, "tiny output was not data-overflow")) return false;
	Enc::FailNextAllocationForTest();
	if (!rejected(input, Enc::Fatal, "staging OOM was not fatal and atomic")) return false;

	invalid = input;
	invalid.png = const_cast<uint8_t*>(RGBA.data());
	invalid.png_size = RGBA.size();
	if (!Check(Enc::Encode(handle, &invalid, &info.info) == Enc::InvalidParam,
	           "image/output alias was accepted")) return false;
	invalid = input;
	if (!Check(Enc::Encode(handle, &invalid, reinterpret_cast<Enc::OutputInfo*>(output.Data())) == Enc::InvalidParam,
	           "output/output-info alias was accepted")) return false;
	invalid = input;
	invalid.image = workspace.bytes.data();
	invalid.image_size = workspace.bytes.size();
	if (!Check(Enc::Encode(handle, &invalid, &info.info) == Enc::InvalidParam,
	           "image/workspace alias was accepted")) return false;
	if (!Check(Enc::Delete(handle) == 0, "error-test handle delete failed")) return false;

	Enc::ResetSessionState();
	GuardedWorkspace wide_workspace;
	if (!CreateEncoder(wide_workspace, handle, 1000001)) return false;
	output.bytes.fill(CANARY);
	info = GuardedInfo {};
	Enc::EncodeInput over_budget {RGBA.data(), output.Data(), 200000000u, UINT32_MAX,
	                              1000000u, 50u, 4000000u, 0, 19, 8, 0, 0, 6};
	return Check(Enc::Encode(handle, &over_budget, &info.info) == Enc::InvalidSize,
	             "process-wide host budget was not enforced before allocation") &&
	       Check(output.AllCanary() && info.Unchanged(), "budget rejection changed guest outputs") &&
	       Check(Enc::Delete(handle) == 0, "wide encoder handle delete failed");
}

bool TestAliasMatrix()
{
	Enc::ResetSessionState();
	GuardedWorkspace workspace;
	Enc::Handle handle = nullptr;
	if (!CreateEncoder(workspace, handle)) return false;
	GuardedOutput output;
	GuardedInfo info;
	auto base = RgbaInput(output);
	auto check = [&](Enc::EncodeInput* candidate, Enc::OutputInfo* result, const char* message)
	{
		output.bytes.fill(CANARY);
		info = GuardedInfo {};
		return Check(Enc::Encode(handle, candidate, result) == Enc::InvalidParam, message);
	};

	auto candidate = base;
	candidate.image = &candidate;
	candidate.image_size = sizeof(candidate);
	candidate.width = 1;
	candidate.height = 1;
	candidate.pitch = 4;
	if (!check(&candidate, &info.info, "record/image alias was accepted")) return false;
	candidate = base;
	candidate.png = &candidate;
	candidate.png_size = sizeof(candidate);
	if (!check(&candidate, &info.info, "record/output alias was accepted")) return false;
	candidate = base;
	if (!check(&candidate, reinterpret_cast<Enc::OutputInfo*>(&candidate), "record/output-info alias was accepted"))
		return false;
	candidate = base;
	candidate.png = const_cast<uint8_t*>(RGBA.data());
	candidate.png_size = RGBA.size();
	if (!check(&candidate, &info.info, "image/output alias was accepted")) return false;
	candidate = base;
	if (!check(&candidate, reinterpret_cast<Enc::OutputInfo*>(const_cast<uint8_t*>(RGBA.data())),
	           "image/output-info alias was accepted")) return false;
	candidate = base;
	candidate.image = workspace.bytes.data();
	candidate.image_size = workspace.bytes.size();
	if (!check(&candidate, &info.info, "image/workspace alias was accepted")) return false;
	candidate = base;
	if (!check(&candidate, reinterpret_cast<Enc::OutputInfo*>(output.Data()), "output/output-info alias was accepted"))
		return false;
	candidate = base;
	candidate.png = workspace.bytes.data();
	candidate.png_size = workspace.bytes.size();
	if (!check(&candidate, &info.info, "output/workspace alias was accepted")) return false;
	candidate = base;
	if (!check(&candidate, reinterpret_cast<Enc::OutputInfo*>(workspace.bytes.data()),
	           "output-info/workspace alias was accepted")) return false;
	if (!Check(Enc::Delete(handle) == 0, "alias-test handle delete failed")) return false;

	Enc::ResetSessionState();
	alignas(8) std::array<uint8_t, 64> workspace_record_arena {};
	Enc::CreateInput create {sizeof(create), 0, 2, 5};
	handle = nullptr;
	if (!Check(Enc::Create(&create, workspace_record_arena.data(), 16, &handle) == 0,
	           "record/workspace alias fixture could not create an encoder")) return false;
	candidate = RgbaInput(output);
	std::memcpy(workspace_record_arena.data(), &candidate, sizeof(candidate));
	if (!Check(Enc::Encode(handle, reinterpret_cast<Enc::EncodeInput*>(workspace_record_arena.data()), &info.info) ==
	               Enc::InvalidParam,
	           "record/workspace alias was accepted")) return false;
	return Check(Enc::Delete(handle) == 0, "record/workspace alias handle delete failed");
}

bool TestRegistryLimit()
{
	Enc::ResetSessionState();
	Enc::CreateInput create {sizeof(create), 0, 1, 0};
	alignas(8) std::array<std::array<uint64_t, 2>, 257> workspaces {};
	std::array<Enc::Handle, 257> handles {};
	for (size_t index = 0; index < 256; ++index)
	{
		if (!Check(Enc::Create(&create, workspaces[index].data(), 16, &handles[index]) == 0,
		           "registry rejected an admitted slot before its fixed limit")) return false;
	}
	handles[256] = reinterpret_cast<Enc::Handle>(uintptr_t {0x1234});
	if (!Check(Enc::Create(&create, workspaces[256].data(), 16, &handles[256]) == Enc::Fatal,
	           "registry did not fail closed at its fixed limit") ||
	    !Check(handles[256] == reinterpret_cast<Enc::Handle>(uintptr_t {0x1234}) &&
	           workspaces[256][0] == 0 && workspaces[256][1] == 0,
	           "registry exhaustion mutated guest outputs")) return false;
	Enc::ResetSessionState();
	return Check(Enc::Delete(handles[0]) == Enc::InvalidHandle, "registry reset retained a full-table handle") &&
	       Check(Enc::Create(&create, workspaces[256].data(), 16, &handles[256]) == 0,
	             "registry did not recover after session reset") &&
	       Check(Enc::Delete(handles[256]) == 0, "post-reset registry handle could not be deleted");
}

bool WaitUntilPaused()
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (!Enc::EncodePausedForTest() && std::chrono::steady_clock::now() < deadline)
		std::this_thread::yield();
	return Enc::EncodePausedForTest();
}

bool TestConcurrency()
{
	Enc::ResetSessionState();
	GuardedWorkspace first_workspace;
	Enc::Handle first_handle = nullptr;
	if (!CreateEncoder(first_workspace, first_handle)) return false;
	GuardedOutput first_output;
	GuardedInfo first_info;
	auto first_input = RgbaInput(first_output);
	std::atomic<int32_t> encode_result {0};
	Enc::ArmEncodePauseForTest(Enc::EncodePausePoint::AfterAdmission);
	std::thread admitted([&] { encode_result.store(Enc::Encode(first_handle, &first_input, &first_info.info)); });
	if (!Check(WaitUntilPaused(), "Encode did not pause after admission"))
	{
		Enc::ResumeEncodeForTest();
		admitted.join();
		return false;
	}
	const int32_t delete_before_lock = Enc::Delete(first_handle);
	Enc::ResumeEncodeForTest();
	admitted.join();
	if (!Check(delete_before_lock == 0 && encode_result.load() == Enc::InvalidHandle,
	           "Delete did not close a pre-acquired Encode before slot entry") ||
	    !Check(first_output.AllCanary() && first_info.Unchanged(), "closed admission changed guest outputs")) return false;

	GuardedWorkspace second_workspace;
	Enc::Handle second_handle = nullptr;
	if (!CreateEncoder(second_workspace, second_handle)) return false;
	GuardedOutput second_output;
	GuardedInfo second_info;
	auto second_input = RgbaInput(second_output);
	encode_result.store(0);
	std::atomic<int32_t> delete_result {Enc::InvalidHandle};
	Enc::ArmEncodePauseForTest(Enc::EncodePausePoint::AfterSlotLock);
	std::thread entered([&] { encode_result.store(Enc::Encode(second_handle, &second_input, &second_info.info)); });
	if (!Check(WaitUntilPaused(), "Encode did not pause after slot entry"))
	{
		Enc::ResumeEncodeForTest();
		entered.join();
		return false;
	}
	std::thread deleting([&] { delete_result.store(Enc::Delete(second_handle)); });
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (!Enc::IsHandleClosingForTest(second_handle) && std::chrono::steady_clock::now() < deadline)
		std::this_thread::yield();
	const bool closing = Enc::IsHandleClosingForTest(second_handle);
	GuardedOutput late_output;
	GuardedInfo late_info;
	auto late_input = RgbaInput(late_output);
	const int32_t late_result = Enc::Encode(second_handle, &late_input, &late_info.info);
	Enc::ResumeEncodeForTest();
	entered.join();
	deleting.join();
	if (!Check(closing && encode_result.load() > 0 && delete_result.load() == 0,
	           "Delete did not drain an Encode already inside the slot") ||
	    !Check(late_result == Enc::InvalidHandle && late_output.AllCanary() && late_info.Unchanged(),
	           "Closing admitted a new Encode or changed its outputs")) return false;

	GuardedWorkspace parallel_workspaces[2];
	Enc::Handle parallel_handles[2] {};
	GuardedOutput parallel_outputs[2];
	GuardedInfo parallel_infos[2];
	Enc::EncodeInput parallel_inputs[2] {RgbaInput(parallel_outputs[0]), RgbaInput(parallel_outputs[1])};
	if (!CreateEncoder(parallel_workspaces[0], parallel_handles[0]) ||
	    !CreateEncoder(parallel_workspaces[1], parallel_handles[1])) return false;
	std::atomic<int32_t> parallel_results[2] {};
	Enc::SetParallelEncodeBarrierForTest(2);
	std::thread left([&] { parallel_results[0].store(Enc::Encode(parallel_handles[0], &parallel_inputs[0], &parallel_infos[0].info)); });
	std::thread right([&] { parallel_results[1].store(Enc::Encode(parallel_handles[1], &parallel_inputs[1], &parallel_infos[1].info)); });
	left.join();
	right.join();
	Enc::SetParallelEncodeBarrierForTest(0);
	return Check(parallel_results[0].load() > 0 && parallel_results[1].load() > 0,
	             "parallel encodes failed") &&
	       Check(Enc::MaxParallelEncodesForTest() >= 2, "distinct handles were globally serialized") &&
	       Check(Enc::Delete(parallel_handles[0]) == 0 && Enc::Delete(parallel_handles[1]) == 0,
	             "parallel handles could not be deleted");
}

#if defined(LUMEN_FEXCORE_RUNTIME) && defined(__APPLE__)
bool TestGuestMemoryBorrows()
{
	namespace VM = Lumen::Core::VirtualMemory;
	Enc::ResetSessionState();
	const long host_page = sysconf(_SC_PAGESIZE);
	if (!Check(host_page > 0, "borrow host page size")) return false;
	const uint64_t page = static_cast<uint64_t>(host_page);
	const uint64_t mapping = VM::Alloc(0, page * 3, VM::Mode::ReadWrite);
	const uint64_t tiny = VM::Alloc(0, 1, VM::Mode::ReadWrite);
	if (!Check(mapping != 0 && tiny != 0, "borrow mappings")) return false;
	struct Cleanup
	{
		uint64_t mapping;
		uint64_t tiny;
		~Cleanup() { VM::Free(mapping); VM::Free(tiny); }
	} cleanup {mapping, tiny};
	auto* bytes = reinterpret_cast<uint8_t*>(mapping);
	std::memset(bytes, CANARY, static_cast<size_t>(page * 3));
	std::memcpy(bytes + page, RGBA.data(), RGBA.size());
	if (!Check(VM::Protect(mapping + page, page, VM::Mode::Read) &&
	           VM::Protect(mapping + page * 2, page, VM::Mode::NoAccess), "borrow protections")) return false;
	GuardedWorkspace workspace;
	Enc::Handle handle {};
	if (!CreateEncoder(workspace, handle)) return false;
	bool ok = true;
	GuardedOutput output;
	GuardedInfo info;
	auto input = RgbaInput(output);
	input.image = bytes + page;
	ok = Check(Enc::Encode(handle, &input, &info.info) > 0 && output.GuardsIntact(),
	           "read-only guest image succeeds") && ok;
	output = GuardedOutput {};
	info = GuardedInfo {};
	const auto denied = [&](const Enc::EncodeInput* parameters, Enc::OutputInfo* result)
	{
		return Check(Enc::Encode(handle, parameters, result) == Enc::InvalidAddress &&
		             output.AllCanary() && info.Unchanged(), "denied borrow publishes no PNG or info");
	};
	for (const void* invalid: {static_cast<void*>(bytes + page * 2), static_cast<void*>(bytes + page * 2 - 4),
	                          reinterpret_cast<void*>(UINTPTR_MAX - 3), reinterpret_cast<void*>(tiny)})
	{
		input = RgbaInput(output);
		input.image = invalid;
		ok = denied(&input, &info.info) && ok;
		ok = denied(static_cast<const Enc::EncodeInput*>(invalid), &info.info) && ok;
	}
	for (void* invalid: {static_cast<void*>(bytes + page), static_cast<void*>(bytes + page * 2),
	                    static_cast<void*>(bytes + page - 4), reinterpret_cast<void*>(UINTPTR_MAX - 3),
	                    reinterpret_cast<void*>(tiny)})
	{
		input = RgbaInput(output);
		input.png = invalid;
		ok = denied(&input, &info.info) && ok;
		input = RgbaInput(output);
		ok = denied(&input, static_cast<Enc::OutputInfo*>(invalid)) && ok;
	}
	Enc::CreateInput create {sizeof(create), 0, 2, 5};
	GuardedWorkspace unpublished;
	Enc::Handle absent = nullptr;
	ok = Check(Enc::QueryMemorySize(reinterpret_cast<const Enc::CreateInput*>(bytes + page * 2)) == Enc::InvalidAddress &&
	           Enc::Create(&create, bytes + page, 16, &absent) == Enc::InvalidAddress && absent == nullptr &&
	           Enc::Create(&create, unpublished.bytes.data(), 16, reinterpret_cast<Enc::Handle*>(bytes + page)) ==
	               Enc::InvalidAddress &&
	           std::all_of(unpublished.bytes.begin(), unpublished.bytes.end(), [](uint8_t value) { return value == 0; }),
	           "create preflights descriptor, workspace and handle before publication") && ok;
	ok = Check(std::all_of(bytes + page - 128, bytes + page, [](uint8_t value) { return value == CANARY; }),
	           "cross-page failures preserve writable prefix") && ok;

	for (const bool release: {false, true})
	{
		const uint64_t active = VM::Alloc(0, page, VM::Mode::ReadWrite);
		if (!Check(active != 0, "concurrent borrow allocation")) return false;
		GuardedOutput concurrent_output;
		GuardedInfo concurrent_info;
		auto concurrent_input = RgbaInput(concurrent_output);
		concurrent_input.png = reinterpret_cast<void*>(active);
		std::atomic<int32_t> result {0};
		Enc::ArmEncodePauseForTest(Enc::EncodePausePoint::AfterMemoryBorrow);
		std::thread encode([&] { result.store(Enc::Encode(handle, &concurrent_input, &concurrent_info.info)); });
		if (!Check(WaitUntilPaused(), "encode pause with active guest borrows"))
		{
			Enc::ResumeEncodeForTest(); encode.join(); VM::Free(active); return false;
		}
		std::atomic<bool> started {false};
		std::atomic<bool> complete {false};
		bool mutated = false;
		std::thread writer([&]
		{
			started.store(true, std::memory_order_release);
			mutated = release ? VM::Free(active) : VM::Protect(active, page, VM::Mode::Read);
			complete.store(true, std::memory_order_release);
		});
		while (!started.load(std::memory_order_acquire)) std::this_thread::yield();
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(30);
		while (!complete.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
			std::this_thread::yield();
		const bool waited = !complete.load(std::memory_order_acquire);
		Enc::ResumeEncodeForTest();
		encode.join(); writer.join();
		ok = Check(waited && mutated && result.load() > 0 && concurrent_info.info.data_size == static_cast<uint32_t>(result.load()),
		           "Protect and Free wait through PNG output publication") && ok;
		if (!release) ok = VM::Free(active) && ok;
	}
	return Check(Enc::Delete(handle) == 0, "borrow handle delete") && ok;
}
#endif

bool TestBoundedZlib()
{
	auto exercise = [](size_t input_size)
	{
		std::vector<uint8_t> source(input_size);
		const uint8_t empty_source = 0;
		for (size_t index = 0; index < source.size(); ++index)
			source[index] = static_cast<uint8_t>((index * 131u + index / 7u) & 0xffu);
		std::vector<uint8_t> large(input_size * 2 + 65536, CANARY);
		size_t size = 0;
		const uint8_t* source_data = source.empty() ? &empty_source : source.data();
		if (!Check(Lumen::Core::TryCompressZlib(source_data, source.size(), large.data(), large.size(), 6, &size) ==
		           Lumen::Core::ZlibCompressResult::Ok && size > 0, "bounded zlib baseline failed")) return false;
		std::vector<uint8_t> exact(size + 2, CANARY);
		size_t exact_size = 0;
		if (!Check(Lumen::Core::TryCompressZlib(source_data, source.size(), exact.data() + 1, size, 6, &exact_size) ==
		               Lumen::Core::ZlibCompressResult::Ok && exact_size == size,
		           "bounded zlib exact capacity failed") ||
		    !Check(exact.front() == CANARY && exact.back() == CANARY,
		           "bounded zlib changed exact-capacity canaries")) return false;
		std::vector<uint8_t> short_buffer(size + 1, CANARY);
		size_t short_size = 99;
		return Check(Lumen::Core::TryCompressZlib(source_data, source.size(), short_buffer.data() + 1,
		                                          size - 1, 6, &short_size) ==
		                 Lumen::Core::ZlibCompressResult::OutputTooSmall && short_size == 0,
		             "bounded zlib one-byte-short contract failed") &&
		       Check(short_buffer.front() == CANARY && short_buffer.back() == CANARY,
		             "bounded zlib wrote outside short capacity");
	};
	if (!exercise(0) || !exercise(1) || !exercise(16384)) return false;

	std::array<uint8_t, 4096> source {};
	for (size_t index = 0; index < source.size(); ++index)
		source[index] = static_cast<uint8_t>((index * 131u + index / 7u) & 0xffu);
	std::array<uint8_t, 8192> large {};
	size_t size = 0;
	if (!Check(Lumen::Core::TryCompressZlib(nullptr, 1, large.data(), large.size(), 6, &size) ==
	               Lumen::Core::ZlibCompressResult::InvalidArgument,
	           "bounded zlib accepted a null nonempty source") ||
	    !Check(Lumen::Core::TryCompressZlib(source.data(), source.size(), nullptr, large.size(), 6, &size) ==
	               Lumen::Core::ZlibCompressResult::InvalidArgument,
	           "bounded zlib accepted a null destination") ||
	    !Check(Lumen::Core::TryCompressZlib(source.data(), source.size(), large.data(), large.size(), 10, &size) ==
	               Lumen::Core::ZlibCompressResult::InvalidArgument,
	           "bounded zlib accepted an invalid level") ||
	    !Check(Lumen::Core::TryCompressZlib(source.data(), size_t {UINT32_MAX} + 1, large.data(), large.size(), 6, &size) ==
	               Lumen::Core::ZlibCompressResult::InvalidArgument,
	           "bounded zlib accepted an unrepresentable input size") ||
	    !Check(Lumen::Core::TryCompressZlib(source.data(), source.size(), large.data(), large.size(), 6, nullptr) ==
	               Lumen::Core::ZlibCompressResult::InvalidArgument,
	           "bounded zlib accepted a null written slot")) return false;

	std::array<std::array<uint8_t, 8192>, 4> parallel_output {};
	std::array<size_t, 4> parallel_size {};
	std::array<Lumen::Core::ZlibCompressResult, 4> parallel_result {};
	std::array<std::thread, 4> workers;
	for (size_t index = 0; index < workers.size(); ++index)
	{
		workers[index] = std::thread([&, index]
		{
			parallel_result[index] = Lumen::Core::TryCompressZlib(
			    source.data(), source.size(), parallel_output[index].data(), parallel_output[index].size(),
			    6, &parallel_size[index]);
		});
	}
	for (auto& worker: workers) worker.join();
	for (size_t index = 0; index < workers.size(); ++index)
	{
		if (!Check(parallel_result[index] == Lumen::Core::ZlibCompressResult::Ok &&
		           parallel_size[index] == parallel_size[0] &&
		           std::equal(parallel_output[0].begin(), parallel_output[0].begin() + parallel_size[0],
		                      parallel_output[index].begin()),
		           "parallel bounded zlib output was not deterministic")) return false;
	}
	Lumen::Core::FailNextZlibAllocationForTest();
	return Check(Lumen::Core::TryCompressZlib(source.data(), source.size(), large.data(), large.size(), 6, &size) ==
	                 Lumen::Core::ZlibCompressResult::OutOfMemory,
	             "bounded zlib allocator failure was not recoverable");
}

} // namespace

int main()
{
	Lumen::Core::ThreadsSubsystem::Instance()->Init(nullptr);
	Lumen::Config::ConfigSubsystem::Instance()->Init(nullptr);
	Lumen::Log::LogSubsystem::Instance()->Init(nullptr);
	Lumen::Log::SetDirection(Lumen::Log::Direction::Silent);
	if (!TestBindings() || !TestBoundedZlib() || !TestQueryCreateAndLifecycle() ||
	    !TestFormatsFiltersAndCapacity() || !TestErrorsAndAtomicity() || !TestAliasMatrix() ||
	    !TestRegistryLimit() || !TestConcurrency()) return 1;
#if defined(LUMEN_FEXCORE_RUNTIME) && defined(__APPLE__)
	if (!TestGuestMemoryBorrows()) return 1;
#endif
	std::puts("PngEncTest: PASS");
	return 0;
}
