#include "Emulator/Libs/PngDec.h"

#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "Lumen/Core/VirtualMemory.h"
#include "Lumen/Core/Threads.h"

#include "stb_image.h"

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <unordered_map>

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs {

LIB_VERSION("PngDec", 1, "PngDec", 1, 1);

namespace PngDec {
namespace {

struct DecoderSettings
{
	uint32_t max_width;
	uint32_t attributes;
};

// Workspace contents and handle representation are private to this HLE.
// Track live handles separately so a forged/stale pointer is never dereferenced.
struct WorkspaceRecord
{
	uint64_t tag;
	DecoderSettings settings;
};
static_assert(sizeof(WorkspaceRecord) == 16);

std::mutex g_decoders_mutex;
std::unordered_map<Handle, DecoderSettings> g_decoders;

void TraceWrite(const char* operation, const void* destination, uint64_t footprint,
                uint64_t pitch = 0, uint32_t width = 0, uint32_t height = 0)
{
	static const bool enabled = [] {
		const char* value = std::getenv("LUMEN_X64_HOST_ERROR_TRACE");
		return value != nullptr && value[0] == '1' && value[1] == '\0';
	}();
	if (!enabled) return;
	static std::atomic<uint32_t> count {0};
	uint32_t index = count.load(std::memory_order_relaxed);
	do {
		if (index >= 8192) return;
	} while (!count.compare_exchange_weak(index, index + 1, std::memory_order_relaxed));
	const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
	    std::chrono::steady_clock::now().time_since_epoch()).count();
	// Only host scalars and already validated addresses; no diagnostic guest reads.
	std::fprintf(stderr,
	    "[PNG_WRITE_TRACE] index=%u monotonicMs=%lld thread=%d operation=%s destination=0x%016" PRIx64
	    " footprint=%" PRIu64 " pitch=%" PRIu64 " width=%u height=%u\n",
	    index, static_cast<long long>(now_ms), Core::Thread::GetThreadIdUnique(), operation,
	    reinterpret_cast<uint64_t>(destination), footprint, pitch, width, height);
}

bool Borrow(const void* pointer, uint64_t size, Core::VirtualMemory::Mode mode,
	        Core::VirtualMemory::GuestDataLease* lease)
{
#if defined(LUMEN_FEXCORE_RUNTIME) && defined(__APPLE__)
	return Core::VirtualMemory::TryAcquireGuestDataLease(
	    reinterpret_cast<uint64_t>(pointer), size, mode, lease);
#else
	(void)pointer; (void)size; (void)mode; (void)lease;
	return true;
#endif
}

int32_t CheckCreateInput(const CreateInput* input)
{
	if (input == nullptr) return InvalidParam;
	if (input->size != sizeof(CreateInput) || (input->attributes & ~1u) != 0) return InvalidParam;
	if (input->max_width == 0 || input->max_width > static_cast<uint32_t>(std::numeric_limits<int>::max() / 4))
		return InvalidSize;
	return 0;
}

uint32_t ReadBigEndian32(const uint8_t* data)
{
	return (uint32_t(data[0]) << 24u) | (uint32_t(data[1]) << 16u) | (uint32_t(data[2]) << 8u) | data[3];
}

int32_t Inspect(const void* data, uint32_t size, ImageInfo* result)
{
	if (data == nullptr) return InvalidAddress;
	if (size > static_cast<uint32_t>(std::numeric_limits<int>::max())) return InvalidSize;
	constexpr uint8_t signature[] = {137, 80, 78, 71, 13, 10, 26, 10};
	const auto* bytes = static_cast<const uint8_t*>(data);
	if (size < 33 || std::memcmp(bytes, signature, sizeof(signature)) != 0 ||
	    ReadBigEndian32(bytes + 8) != 13 || std::memcmp(bytes + 12, "IHDR", 4) != 0)
		return InvalidData;

	int width = 0, height = 0, components = 0;
	if (!stbi_info_from_memory(bytes, static_cast<int>(size), &width, &height, &components) || width <= 0 || height <= 0)
		return InvalidData;

	ImageInfo info {};
	info.width = static_cast<uint32_t>(width);
	info.height = static_cast<uint32_t>(height);
	info.depth = bytes[24];
	switch (bytes[25])
	{
		case 0: info.color = 2; break;
		case 2: info.color = 3; break;
		case 3: info.color = 4; break;
		case 4: info.color = 18; break;
		case 6: info.color = 19; break;
		default: return InvalidData;
	}
	if (bytes[28] > 1) return InvalidData;
	info.flags = bytes[28] == 1 ? 1u : 0u;

	// Only inspect complete chunks preceding pixel data. Subtract before adding
	// the chunk length so malformed input cannot wrap the encoded-buffer bound.
	for (uint32_t offset = 33; offset <= size && size - offset >= 12;)
	{
		const uint32_t chunk_size = ReadBigEndian32(bytes + offset);
		if (chunk_size > size - offset - 12) return InvalidData;
		const auto* type = bytes + offset + 4;
		if (std::memcmp(type, "IDAT", 4) == 0 || std::memcmp(type, "IEND", 4) == 0) break;
		if (std::memcmp(type, "tRNS", 4) == 0) info.flags |= 2u;
		offset += chunk_size + 12;
	}
	std::memcpy(result, &info, sizeof(info));
	return 0;
}

} // namespace

int32_t LUMEN_SYSV_ABI ParseHeader(const ParseInput* input, ImageInfo* info)
{
	PRINT_NAME();
	if (input == nullptr) return InvalidParam;
	if (info == nullptr) return InvalidAddress;
	Core::VirtualMemory::GuestDataLease descriptor;
	Core::VirtualMemory::GuestDataLease data;
	Core::VirtualMemory::GuestDataLease output;
	if (!Borrow(input, sizeof(*input), Core::VirtualMemory::Mode::Read, &descriptor)) return InvalidAddress;
	ParseInput parameters {};
	std::memcpy(&parameters, input, sizeof(parameters));
	input = &parameters;
	if (input->reserved != 0) return InvalidParam;
	if (input->data == nullptr) return InvalidAddress;
	if (input->size > static_cast<uint32_t>(std::numeric_limits<int>::max())) return InvalidSize;
	if (input->size != 0 && !Borrow(input->data, input->size, Core::VirtualMemory::Mode::Read, &data)) return InvalidAddress;
	if (!Borrow(info, sizeof(*info), Core::VirtualMemory::Mode::Write, &output)) return InvalidAddress;
	const int32_t result = Inspect(input->data, input->size, info);
	if (result == 0) TraceWrite("parse-info", info, sizeof(*info));
	return result;
}

int32_t LUMEN_SYSV_ABI QueryMemorySize(const CreateInput* input)
{
	PRINT_NAME();
	if (input == nullptr) return InvalidParam;
	Core::VirtualMemory::GuestDataLease descriptor;
	if (!Borrow(input, sizeof(*input), Core::VirtualMemory::Mode::Read, &descriptor)) return InvalidAddress;
	CreateInput parameters {};
	std::memcpy(&parameters, input, sizeof(parameters));
	const int32_t error = CheckCreateInput(&parameters);
	return error != 0 ? error : static_cast<int32_t>(sizeof(WorkspaceRecord));
}

int32_t LUMEN_SYSV_ABI Create(const CreateInput* input, void* workspace, uint32_t workspace_size, Handle* handle)
{
	PRINT_NAME();
	if (workspace == nullptr || handle == nullptr) return InvalidAddress;
	if (input == nullptr) return InvalidParam;
	Core::VirtualMemory::GuestDataLease descriptor;
	Core::VirtualMemory::GuestDataLease storage;
	Core::VirtualMemory::GuestDataLease result;
	if (!Borrow(input, sizeof(*input), Core::VirtualMemory::Mode::Read, &descriptor)) return InvalidAddress;
	CreateInput parameters {};
	std::memcpy(&parameters, input, sizeof(parameters));
	input = &parameters;
	const int32_t error = CheckCreateInput(input);
	if (error != 0) return error;
	if (workspace_size < sizeof(WorkspaceRecord) || reinterpret_cast<uintptr_t>(workspace) % alignof(WorkspaceRecord) != 0)
		return InvalidWork;
	if (!Borrow(workspace, sizeof(WorkspaceRecord), Core::VirtualMemory::Mode::Write, &storage) ||
	    !Borrow(handle, sizeof(*handle), Core::VirtualMemory::Mode::Write, &result)) return InvalidAddress;

	std::lock_guard<std::mutex> lock(g_decoders_mutex);
	const DecoderSettings settings {input->max_width, input->attributes};
	if (!g_decoders.emplace(workspace, settings).second) return InvalidWork;
	const WorkspaceRecord record {0x31454c48474e50u, settings};
	std::memcpy(workspace, &record, sizeof(record));
	std::memcpy(handle, &workspace, sizeof(workspace));
	TraceWrite("create-workspace", workspace, sizeof(record));
	TraceWrite("create-handle", handle, sizeof(workspace));
	return 0;
}

int32_t LUMEN_SYSV_ABI Delete(Handle handle)
{
	PRINT_NAME();
	std::lock_guard<std::mutex> lock(g_decoders_mutex);
	return g_decoders.erase(handle) == 1 ? 0 : InvalidHandle;
}

void ResetSessionState()
{
	std::lock_guard<std::mutex> lock(g_decoders_mutex);
	g_decoders.clear();
}

int32_t LUMEN_SYSV_ABI Decode(Handle handle, const DecodeInput* input, ImageInfo* info)
{
	PRINT_NAME();
	DecoderSettings settings {};
	{
		std::lock_guard<std::mutex> lock(g_decoders_mutex);
		const auto it = g_decoders.find(handle);
		if (it == g_decoders.end()) return InvalidHandle;
		settings = it->second;
	}
	if (input == nullptr) return InvalidParam;
	Core::VirtualMemory::GuestDataLease descriptor;
	Core::VirtualMemory::GuestDataLease data;
	Core::VirtualMemory::GuestDataLease output;
	Core::VirtualMemory::GuestDataLease info_output;
	if (!Borrow(input, sizeof(*input), Core::VirtualMemory::Mode::Read, &descriptor)) return InvalidAddress;
	// Destination pixels may overlap the caller's parameter block. Keep the
	// validated settings stable while copying rows into guest memory.
	DecodeInput parameters {};
	std::memcpy(&parameters, input, sizeof(parameters));
	input = &parameters;
	if (input->output == nullptr) return InvalidAddress;
	if (input->format > 1) return InvalidParam;
	if (input->data == nullptr) return InvalidAddress;
	if (input->size > static_cast<uint32_t>(std::numeric_limits<int>::max())) return InvalidSize;
	if (input->size != 0 && !Borrow(input->data, input->size, Core::VirtualMemory::Mode::Read, &data)) return InvalidAddress;
	ImageInfo decoded_info {};
	const int32_t error = Inspect(input->data, input->size, &decoded_info);
	if (error != 0) return error;
	if (decoded_info.width > settings.max_width) return InvalidSize;
	const bool fill_alpha = decoded_info.color != 18 && decoded_info.color != 19 && (decoded_info.flags & 2u) == 0;
	// High alpha values have no established 8-bit fill contract. Explicit alpha
	// and PNG transparency remain authoritative and do not use the fill value.
	if (fill_alpha && input->alpha > 255) return Unsupported;

	const uint64_t row_size = uint64_t(decoded_info.width) * 4u;
	const uint64_t pitch = input->pitch == 0 ? row_size : input->pitch;
	const uint64_t required = pitch * (decoded_info.height - 1u) + row_size;
	if (pitch < row_size || required > input->output_size ||
	    row_size * decoded_info.height > static_cast<uint64_t>(std::numeric_limits<int>::max()))
		return InvalidSize;
	if (!Borrow(input->output, required, Core::VirtualMemory::Mode::Write, &output) ||
	    (info != nullptr && !Borrow(info, sizeof(*info), Core::VirtualMemory::Mode::Write, &info_output)))
		return InvalidAddress;

	int width = 0, height = 0, components = 0;
	std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> pixels(
	    stbi_load_from_memory(static_cast<const stbi_uc*>(input->data), static_cast<int>(input->size),
	                          &width, &height, &components, 4), &stbi_image_free);
	if (!pixels || width != static_cast<int>(decoded_info.width) || height != static_cast<int>(decoded_info.height))
		return DecodeFailure;
	if (fill_alpha)
	{
		const uint64_t byte_count = row_size * decoded_info.height;
		for (uint64_t offset = 3; offset < byte_count; offset += 4)
			pixels.get()[offset] = static_cast<uint8_t>(input->alpha);
	}

	// Decode into owned staging storage first. No guest output is touched on
	// decoding failure, and padding bytes between destination rows stay intact.
	auto* destination = static_cast<uint8_t*>(input->output);
	for (uint32_t row = 0; row < decoded_info.height; ++row)
	{
		const auto* source = pixels.get() + row_size * row;
		auto* output = destination + pitch * row;
		if (input->format == 0)
		{
			std::memcpy(output, source, static_cast<size_t>(row_size));
		} else
		{
			for (uint32_t column = 0; column < decoded_info.width; ++column)
			{
				output[0] = source[2]; output[1] = source[1]; output[2] = source[0]; output[3] = source[3];
				output += 4; source += 4;
			}
		}
	}
	if (info != nullptr) std::memcpy(info, &decoded_info, sizeof(decoded_info));
	TraceWrite("decode-pixels", destination, required, pitch, decoded_info.width, decoded_info.height);
	if (info != nullptr) TraceWrite("decode-info", info, sizeof(decoded_info));
	return decoded_info.width <= 32767 && decoded_info.height <= 32767
	           ? static_cast<int32_t>((decoded_info.width << 16u) | decoded_info.height) : 0;
}

} // namespace PngDec

LIB_DEFINE(InitPngDec_1)
{
	LIB_FUNC("U6h4e5JRPaQ", PngDec::ParseHeader);
	LIB_FUNC("-6srIGbLTIU", PngDec::QueryMemorySize);
	LIB_FUNC("m0uW+8pFyaw", PngDec::Create);
	LIB_FUNC("WC216DD3El4", PngDec::Decode);
	LIB_FUNC("QbD+eENEwo8", PngDec::Delete);
}

} // namespace Lumen::Libs

#endif
