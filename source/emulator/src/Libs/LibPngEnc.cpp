#include "Emulator/Libs/PngEnc.h"

#include "Emulator/Graphics/Objects/GpuMemory.h"

#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"

#include "Lumen/Core/Compression.h"
#include "Lumen/Core/VirtualMemory.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs {

LIB_VERSION("PngEnc", 1, "PngEnc", 1, 1);

namespace PngEnc {
namespace {

constexpr uint32_t WORKSPACE_SIZE = 16;
constexpr uint32_t MAX_WIDTH = 1000001;
constexpr size_t MAX_HANDLES_PER_SESSION = 256;
constexpr uint64_t MAX_HOST_WORK_BYTES = 256ull * 1024ull * 1024ull;
constexpr uint64_t CODEC_RESERVATION_BYTES = 1024ull * 1024ull;
constexpr uint64_t PNG_CONTAINER_BYTES = 57;
constexpr uint64_t WORKSPACE_TAG = 0x31434e45474e5050ull;

enum class SlotState: uint8_t
{
	Empty,
	Live,
	Closing,
	Retired,
};

struct EncoderSettings
{
	uint32_t max_width;
	uint32_t max_filters;
};

struct EncoderSlot
{
	std::mutex             mutex;
	std::atomic<SlotState> state {SlotState::Empty};
	Handle                 handle = nullptr;
	EncoderSettings        settings {};
	uint64_t               serial = 0;
};

struct WorkspaceRecord
{
	uint64_t tag;
	uint64_t serial;
};
static_assert(sizeof(WorkspaceRecord) == WORKSPACE_SIZE);

struct AddressRange
{
	uintptr_t begin;
	uintptr_t end;
};

struct FreeDeleter
{
	void operator()(uint8_t* pointer) const { std::free(pointer); }
};
using Buffer = std::unique_ptr<uint8_t, FreeDeleter>;

std::mutex g_registry_mutex;
std::array<EncoderSlot, MAX_HANDLES_PER_SESSION> g_encoders;
uint64_t g_next_serial = 1;
std::atomic<uint64_t> g_reserved_host_work {0};

#if defined(LUMEN_PNG_ENC_TEST_HOOK)
std::atomic<uint32_t> g_pause_point {0};
std::atomic<bool> g_encode_paused {false};
std::atomic<bool> g_resume_encode {false};
std::atomic<uint32_t> g_parallel_goal {0};
std::atomic<uint32_t> g_active_encodes {0};
std::atomic<uint32_t> g_max_parallel_encodes {0};
std::atomic<bool> g_parallel_released {true};
std::atomic<bool> g_fail_next_allocation {false};

void PauseForTest(EncodePausePoint point)
{
	uint32_t expected = static_cast<uint32_t>(point);
	if (!g_pause_point.compare_exchange_strong(expected, 0, std::memory_order_acq_rel)) return;
	g_encode_paused.store(true, std::memory_order_release);
	while (!g_resume_encode.load(std::memory_order_acquire)) std::this_thread::yield();
	g_encode_paused.store(false, std::memory_order_release);
	g_resume_encode.store(false, std::memory_order_release);
}

class ActiveEncodeForTest final
{
public:
	ActiveEncodeForTest()
	{
		const uint32_t active = g_active_encodes.fetch_add(1, std::memory_order_acq_rel) + 1;
		uint32_t maximum = g_max_parallel_encodes.load(std::memory_order_acquire);
		while (active > maximum && !g_max_parallel_encodes.compare_exchange_weak(
		           maximum, active, std::memory_order_acq_rel)) {}
		const uint32_t goal = g_parallel_goal.load(std::memory_order_acquire);
		if (goal != 0 && active >= goal) g_parallel_released.store(true, std::memory_order_release);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (goal != 0 && !g_parallel_released.load(std::memory_order_acquire) &&
		       std::chrono::steady_clock::now() < deadline)
			std::this_thread::yield();
	}

	~ActiveEncodeForTest() { g_active_encodes.fetch_sub(1, std::memory_order_acq_rel); }
};
#endif

class HostWorkReservation final
{
public:
	HostWorkReservation() = default;

	bool Acquire(uint64_t staging_bytes)
	{
		uint64_t requested = 0;
		if (!CheckedAddForReservation(staging_bytes, CODEC_RESERVATION_BYTES, &requested) ||
		    requested > MAX_HOST_WORK_BYTES) return false;
		uint64_t current = g_reserved_host_work.load(std::memory_order_acquire);
		for (;;)
		{
			if (current > MAX_HOST_WORK_BYTES - requested) return false;
			if (g_reserved_host_work.compare_exchange_weak(current, current + requested,
			                                               std::memory_order_acq_rel))
			{
				m_bytes = requested;
				return true;
			}
		}
	}

	~HostWorkReservation()
	{
		if (m_bytes != 0) g_reserved_host_work.fetch_sub(m_bytes, std::memory_order_acq_rel);
	}

	LUMEN_CLASS_NO_COPY(HostWorkReservation);

private:
	static bool CheckedAddForReservation(uint64_t a, uint64_t b, uint64_t* result)
	{
		if (a > std::numeric_limits<uint64_t>::max() - b) return false;
		*result = a + b;
		return true;
	}

	uint64_t m_bytes = 0;
};

bool CheckedAdd(uint64_t a, uint64_t b, uint64_t* result)
{
	if (result == nullptr || a > std::numeric_limits<uint64_t>::max() - b) return false;
	*result = a + b;
	return true;
}

bool CheckedMultiply(uint64_t a, uint64_t b, uint64_t* result)
{
	if (result == nullptr || (a != 0 && b > std::numeric_limits<uint64_t>::max() / a)) return false;
	*result = a * b;
	return true;
}

bool MakeRange(const void* pointer, uint64_t size, AddressRange* range)
{
	if (pointer == nullptr || range == nullptr || size == 0 ||
	    size > std::numeric_limits<uintptr_t>::max()) return false;
	const uintptr_t begin = reinterpret_cast<uintptr_t>(pointer);
	if (begin > std::numeric_limits<uintptr_t>::max() - static_cast<uintptr_t>(size)) return false;
	*range = AddressRange {begin, begin + static_cast<uintptr_t>(size)};
	return true;
}

bool Overlaps(const AddressRange& left, const AddressRange& right)
{
	return left.begin < right.end && right.begin < left.end;
}

bool PairwiseDisjoint(const AddressRange* ranges, size_t count)
{
	for (size_t left = 0; left < count; ++left)
	{
		for (size_t right = left + 1; right < count; ++right)
		{
			if (Overlaps(ranges[left], ranges[right])) return false;
		}
	}
	return true;
}

bool BorrowTrackedRange(const AddressRange& range, Core::VirtualMemory::Mode required,
	                    Core::VirtualMemory::GuestDataLease* lease)
{
#if defined(LUMEN_FEXCORE_RUNTIME) && defined(__APPLE__)
	return Core::VirtualMemory::TryAcquireGuestDataLease(
	    range.begin, range.end - range.begin, required, lease);
#else
	(void)lease;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_IOS || \
    LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
	uint64_t current = range.begin;
	while (current < range.end)
	{
		Core::VirtualMemory::ManagedRegion region {};
		const auto state = Core::VirtualMemory::QueryManagedRegion(current, &region);
		if (state == Core::VirtualMemory::ManagedRegionState::Unmanaged) return true;
		const auto mode = static_cast<uint32_t>(region.mode);
		const auto need = static_cast<uint32_t>(required);
		if (state != Core::VirtualMemory::ManagedRegionState::Visible ||
		    region.begin > current || region.end <= current || (mode & need) != need)
		{
			return false;
		}
		current = (std::min)(static_cast<uint64_t>(range.end), region.end);
	}
#else
	(void)range;
	(void)required;
#endif
	return true;
#endif
}

Buffer Allocate(uint64_t size)
{
	if (size == 0 || size > std::numeric_limits<size_t>::max()) return {};
#if defined(LUMEN_PNG_ENC_TEST_HOOK)
	if (g_fail_next_allocation.exchange(false, std::memory_order_acq_rel)) return {};
#endif
	return Buffer(static_cast<uint8_t*>(std::malloc(static_cast<size_t>(size))));
}

int32_t CheckCreateInput(const CreateInput& input)
{
	if (input.size != sizeof(CreateInput)) return InvalidParam;
	if (input.attributes != 0 || input.max_filters > 5) return InvalidParam;
	if (input.max_width == 0 || input.max_width > MAX_WIDTH) return InvalidSize;
	return 0;
}

EncoderSlot* FindLiveSlot(Handle handle)
{
	for (auto& encoder: g_encoders)
	{
		if (encoder.state.load(std::memory_order_acquire) == SlotState::Live && encoder.handle == handle)
			return &encoder;
	}
	return nullptr;
}

uint8_t Paeth(uint8_t left, uint8_t up, uint8_t upper_left)
{
	const int estimate = int(left) + int(up) - int(upper_left);
	const int left_distance = std::abs(estimate - int(left));
	const int up_distance = std::abs(estimate - int(up));
	const int upper_left_distance = std::abs(estimate - int(upper_left));
	if (left_distance <= up_distance && left_distance <= upper_left_distance) return left;
	return up_distance <= upper_left_distance ? up : upper_left;
}

uint8_t FilterType(uint16_t filter)
{
	if ((filter & 1u) != 0) return 1;
	if ((filter & 2u) != 0) return 2;
	if ((filter & 4u) != 0) return 3;
	if ((filter & 8u) != 0) return 4;
	return 0;
}

void WriteBigEndian32(uint8_t* output, uint32_t value)
{
	output[0] = static_cast<uint8_t>(value >> 24u);
	output[1] = static_cast<uint8_t>(value >> 16u);
	output[2] = static_cast<uint8_t>(value >> 8u);
	output[3] = static_cast<uint8_t>(value);
}

uint32_t Crc32(const uint8_t* data, size_t size)
{
	uint32_t crc = 0xffffffffu;
	for (size_t index = 0; index < size; ++index)
	{
		crc ^= data[index];
		for (uint32_t bit = 0; bit < 8; ++bit)
		{
			const uint32_t mask = 0u - (crc & 1u);
			crc = (crc >> 1u) ^ (0xedb88320u & mask);
		}
	}
	return ~crc;
}

void WriteChunk(uint8_t* png, size_t* offset, const char type[4],
	            const uint8_t* data, uint32_t size)
{
	WriteBigEndian32(png + *offset, size);
	*offset += 4;
	std::memcpy(png + *offset, type, 4);
	*offset += 4;
	if (size != 0)
	{
		std::memcpy(png + *offset, data, size);
		*offset += size;
	}
	const uint32_t crc = Crc32(png + *offset - size - 4, static_cast<size_t>(size) + 4);
	WriteBigEndian32(png + *offset, crc);
	*offset += 4;
}

void TraceEncodedImage(const EncodeInput& parameters, const uint8_t* image, uint64_t image_bytes,
	                  const uint8_t* png, uint64_t png_bytes)
{
	static const bool enabled = []()
	{
		const char* value = std::getenv("LUMEN_PNG_ENC_TRACE");
		return value != nullptr && std::strcmp(value, "1") == 0;
	}();
	if (!enabled) return;
	static std::atomic<uint32_t> records {0};
	uint32_t record = records.load(std::memory_order_relaxed);
	for (;;)
	{
		if (record >= 16) return;
		if (records.compare_exchange_weak(record, record + 1, std::memory_order_relaxed)) break;
	}

	// Both buffers are already-owned, validated staging snapshots. Count and
	// hash active pixels only: pitch padding must not make a black image nonzero.
	constexpr uint64_t fnv_offset = 14695981039346656037ull;
	constexpr uint64_t fnv_prime = 1099511628211ull;
	uint64_t input_hash = fnv_offset;
	uint64_t png_hash = fnv_offset;
	uint64_t rgb_nonzero_pixels = 0;
	uint64_t alpha_nonzero_pixels = 0;
	for (uint32_t row = 0; row < parameters.height; ++row)
	{
		const auto* pixel = image + uint64_t(row) * parameters.pitch;
		for (uint32_t column = 0; column < parameters.width; ++column, pixel += 4)
		{
			rgb_nonzero_pixels += (pixel[0] | pixel[1] | pixel[2]) != 0;
			alpha_nonzero_pixels += pixel[3] != 0;
			for (uint32_t channel = 0; channel < 4; ++channel)
			{
				input_hash = (input_hash ^ pixel[channel]) * fnv_prime;
			}
		}
	}
	for (uint64_t byte = 0; byte < png_bytes; ++byte)
	{
		png_hash = (png_hash ^ png[byte]) * fnv_prime;
	}
	std::fprintf(stderr,
	             "[PNG_ENC_TRACE] index=%u image=0x%016" PRIxPTR " image_size=%u snapshot_bytes=%" PRIu64
	             " width=%u height=%u pitch=%u format=%u color=%u filter=%u compression=%u"
	             " rgb_nonzero_pixels=%" PRIu64 " alpha_nonzero_pixels=%" PRIu64
	             " input_pixels_fnv1a64=%016" PRIx64 " png_bytes=%" PRIu64 " png_fnv1a64=%016" PRIx64 "\n",
	             record + 1, reinterpret_cast<uintptr_t>(parameters.image), parameters.image_size, image_bytes,
	             parameters.width, parameters.height, parameters.pitch, static_cast<unsigned>(parameters.format),
	             static_cast<unsigned>(parameters.color), static_cast<unsigned>(parameters.filter),
	             static_cast<unsigned>(parameters.compression), rgb_nonzero_pixels, alpha_nonzero_pixels,
	             input_hash, png_bytes, png_hash);
	Graphics::GpuMemoryDumpCpuReadbackTrace(record + 1, reinterpret_cast<uint64_t>(parameters.image), image_bytes);
}

} // namespace

int32_t LUMEN_SYSV_ABI QueryMemorySize(const CreateInput* input)
{
	PRINT_NAME();
	if (input == nullptr) return InvalidAddress;
	AddressRange input_range {};
	Core::VirtualMemory::GuestDataLease input_lease;
	if (!MakeRange(input, sizeof(*input), &input_range) ||
	    !BorrowTrackedRange(input_range, Core::VirtualMemory::Mode::Read, &input_lease)) return InvalidAddress;
	CreateInput parameters {};
	std::memcpy(&parameters, input, sizeof(parameters));
	const int32_t error = CheckCreateInput(parameters);
	return error == 0 ? static_cast<int32_t>(WORKSPACE_SIZE) : error;
}

int32_t LUMEN_SYSV_ABI Create(const CreateInput* input, void* workspace,
	                          uint32_t workspace_size, Handle* handle)
{
	PRINT_NAME();
	if (input == nullptr || workspace == nullptr || handle == nullptr) return InvalidAddress;
	AddressRange ranges[3] {};
	Core::VirtualMemory::GuestDataLease input_lease;
	Core::VirtualMemory::GuestDataLease workspace_lease;
	Core::VirtualMemory::GuestDataLease handle_lease;
	if (!MakeRange(input, sizeof(*input), &ranges[0]) ||
	    !MakeRange(workspace, WORKSPACE_SIZE, &ranges[1]) ||
	    !MakeRange(handle, sizeof(*handle), &ranges[2]) ||
	    !BorrowTrackedRange(ranges[0], Core::VirtualMemory::Mode::Read, &input_lease))
	{
		return InvalidAddress;
	}
	CreateInput parameters {};
	std::memcpy(&parameters, input, sizeof(parameters));
	const int32_t error = CheckCreateInput(parameters);
	if (error != 0) return error;
	if (workspace_size < WORKSPACE_SIZE) return InvalidSize;
	if ((reinterpret_cast<uintptr_t>(workspace) % 8u) != 0 ||
	    (reinterpret_cast<uintptr_t>(handle) % 8u) != 0 || !PairwiseDisjoint(ranges, std::size(ranges)))
	{
		return InvalidParam;
	}
	if (!BorrowTrackedRange(ranges[1], Core::VirtualMemory::Mode::Write, &workspace_lease) ||
	    !BorrowTrackedRange(ranges[2], Core::VirtualMemory::Mode::Write, &handle_lease)) return InvalidAddress;

	std::lock_guard<std::mutex> registry_lock(g_registry_mutex);
	EncoderSlot* empty = nullptr;
	for (auto& encoder: g_encoders)
	{
		const SlotState state = encoder.state.load(std::memory_order_acquire);
		if (state != SlotState::Empty && encoder.handle == workspace) return InvalidHandle;
		if (state == SlotState::Empty && empty == nullptr) empty = &encoder;
	}
	if (empty == nullptr) return Fatal;

	const uint64_t serial = g_next_serial++;
	const WorkspaceRecord record {WORKSPACE_TAG, serial};
	std::memcpy(workspace, &record, sizeof(record));
	Handle published = workspace;
	std::memcpy(handle, &published, sizeof(published));
	empty->handle = workspace;
	empty->settings = EncoderSettings {parameters.max_width, parameters.max_filters};
	empty->serial = serial;
	empty->state.store(SlotState::Live, std::memory_order_release);
	return 0;
}

int32_t LUMEN_SYSV_ABI Encode(Handle handle, const EncodeInput* input, OutputInfo* info)
{
	PRINT_NAME();
	EncoderSlot* encoder = nullptr;
	{
		std::lock_guard<std::mutex> registry_lock(g_registry_mutex);
		encoder = FindLiveSlot(handle);
		if (encoder == nullptr) return InvalidHandle;
	}
#if defined(LUMEN_PNG_ENC_TEST_HOOK)
	PauseForTest(EncodePausePoint::AfterAdmission);
#endif
	std::lock_guard<std::mutex> encoder_lock(encoder->mutex);
	if (encoder->state.load(std::memory_order_acquire) != SlotState::Live || encoder->handle != handle)
		return InvalidHandle;
#if defined(LUMEN_PNG_ENC_TEST_HOOK)
	PauseForTest(EncodePausePoint::AfterSlotLock);
#endif
	if (input == nullptr) return InvalidParam;

	AddressRange input_record_range {};
	Core::VirtualMemory::GuestDataLease input_lease;
	if (!MakeRange(input, sizeof(*input), &input_record_range) ||
	    !BorrowTrackedRange(input_record_range, Core::VirtualMemory::Mode::Read, &input_lease)) return InvalidAddress;
	EncodeInput parameters {};
	std::memcpy(&parameters, input, sizeof(parameters));
	if (parameters.image == nullptr || parameters.png == nullptr) return InvalidAddress;

	AddressRange declared_image_range {};
	AddressRange png_range {};
	if ((parameters.image_size != 0 && !MakeRange(parameters.image, parameters.image_size, &declared_image_range)) ||
	    (parameters.png_size != 0 && !MakeRange(parameters.png, parameters.png_size, &png_range))) return InvalidAddress;
	if (parameters.image_size == 0 || parameters.png_size == 0 ||
	    parameters.width == 0 || parameters.height == 0) return InvalidSize;
	if (parameters.format > 1 || (parameters.color != 3 && parameters.color != 19) ||
	    parameters.depth != 8 || parameters.clut_count != 0 || parameters.compression > 9 ||
	    (parameters.filter & ~uint16_t {0x0f}) != 0)
	{
		return InvalidParam;
	}

	uint64_t source_row = 0;
	uint64_t output_row = 0;
	uint64_t required_input = 0;
	uint64_t filtered_row = 0;
	uint64_t filtered_size = 0;
	if (parameters.width > encoder->settings.max_width ||
	    !CheckedMultiply(parameters.width, 4, &source_row) ||
	    !CheckedMultiply(parameters.width, parameters.color == 19 ? 4 : 3, &output_row) ||
	    (parameters.color == 19 && parameters.pitch != source_row) ||
	    (parameters.color == 3 && parameters.pitch < source_row))
	{
		return InvalidSize;
	}
	if (parameters.color == 19)
	{
		if (!CheckedMultiply(source_row, parameters.height, &required_input)) return InvalidSize;
	} else
	{
		uint64_t row_offsets = 0;
		if (!CheckedMultiply(parameters.height - 1u, parameters.pitch, &row_offsets) ||
		    !CheckedAdd(row_offsets, source_row, &required_input)) return InvalidSize;
	}
	if (required_input > parameters.image_size ||
	    !CheckedAdd(output_row, 1, &filtered_row) ||
	    !CheckedMultiply(filtered_row, parameters.height, &filtered_size)) return InvalidSize;

	uint64_t compression_bound = 0;
	uint64_t row_scratch = 0;
	if (!CheckedAdd(filtered_size, filtered_size / 128u, &compression_bound) ||
	    !CheckedAdd(compression_bound, 65536, &compression_bound) ||
	    compression_bound > std::numeric_limits<uint64_t>::max() - PNG_CONTAINER_BYTES ||
	    !CheckedMultiply(output_row, 2, &row_scratch)) return InvalidSize;
	const uint64_t compressed_capacity = parameters.png_size > PNG_CONTAINER_BYTES
	                                               ? (std::min)(compression_bound,
	                                                            uint64_t(parameters.png_size) - PNG_CONTAINER_BYTES)
	                                               : 0;
	uint64_t host_work = 0;
	for (const uint64_t allocation: {required_input, filtered_size, row_scratch,
	                                compressed_capacity, compressed_capacity + PNG_CONTAINER_BYTES})
	{
		if (!CheckedAdd(host_work, allocation, &host_work)) return InvalidSize;
	}
	if (host_work > MAX_HOST_WORK_BYTES || required_input > std::numeric_limits<size_t>::max() ||
	    filtered_size > std::numeric_limits<size_t>::max() || output_row > std::numeric_limits<size_t>::max())
	{
		return InvalidSize;
	}
	HostWorkReservation host_work_reservation;
	if (!host_work_reservation.Acquire(host_work)) return InvalidSize;

	AddressRange image_range {};
	Core::VirtualMemory::GuestDataLease image_lease;
	Core::VirtualMemory::GuestDataLease png_lease;
	Core::VirtualMemory::GuestDataLease info_lease;
	if (!MakeRange(parameters.image, required_input, &image_range)) return InvalidAddress;
	AddressRange ranges[5] {input_record_range, image_range, png_range,
	                        AddressRange {}, AddressRange {}};
	size_t range_count = 3;
	if (info != nullptr)
	{
		if (!MakeRange(info, sizeof(*info), &ranges[range_count])) return InvalidAddress;
		range_count++;
	}
	if (!MakeRange(handle, WORKSPACE_SIZE, &ranges[range_count])) return InvalidHandle;
	range_count++;
	if (!PairwiseDisjoint(ranges, range_count)) return InvalidParam;
	// Preserve the API's alias rejection before probing write permissions:
	// aliased output may point into an otherwise valid read-only input image.
	if (!BorrowTrackedRange(image_range, Core::VirtualMemory::Mode::Read, &image_lease) ||
	    !BorrowTrackedRange(png_range, Core::VirtualMemory::Mode::Write, &png_lease) ||
	    (info != nullptr && !BorrowTrackedRange(ranges[3], Core::VirtualMemory::Mode::Write, &info_lease)))
		return InvalidAddress;
	if (compressed_capacity == 0) return DataOverflow;

#if defined(LUMEN_PNG_ENC_TEST_HOOK)
	PauseForTest(EncodePausePoint::AfterMemoryBorrow);
	ActiveEncodeForTest active_encode_for_test;
#endif

	Buffer image = Allocate(required_input);
	Buffer filtered = Allocate(filtered_size);
	Buffer previous = Allocate(output_row);
	Buffer current = Allocate(output_row);
	Buffer compressed = Allocate(compressed_capacity);
	if (!image || !filtered || !previous || !current || !compressed) return Fatal;
	std::memcpy(image.get(), parameters.image, static_cast<size_t>(required_input));
	std::memset(previous.get(), 0, static_cast<size_t>(output_row));

	const uint8_t filter_type = FilterType(parameters.filter);
	const uint32_t channels = parameters.color == 19 ? 4 : 3;
	for (uint32_t row = 0; row < parameters.height; ++row)
	{
		const auto* source = image.get() + uint64_t(row) * parameters.pitch;
		for (uint32_t column = 0; column < parameters.width; ++column)
		{
			const uint8_t red = source[parameters.format == 0 ? 0 : 2];
			const uint8_t green = source[1];
			const uint8_t blue = source[parameters.format == 0 ? 2 : 0];
			const size_t destination = size_t(column) * channels;
			current.get()[destination] = red;
			current.get()[destination + 1] = green;
			current.get()[destination + 2] = blue;
			if (channels == 4) current.get()[destination + 3] = source[3];
			source += 4;
		}

		auto* destination = filtered.get() + uint64_t(row) * filtered_row;
		*destination++ = filter_type;
		for (size_t byte = 0; byte < static_cast<size_t>(output_row); ++byte)
		{
			const uint8_t value = current.get()[byte];
			const uint8_t left = byte >= channels ? current.get()[byte - channels] : 0;
			const uint8_t up = row != 0 ? previous.get()[byte] : 0;
			const uint8_t upper_left = row != 0 && byte >= channels ? previous.get()[byte - channels] : 0;
			switch (filter_type)
			{
				case 1: destination[byte] = static_cast<uint8_t>(value - left); break;
				case 2: destination[byte] = static_cast<uint8_t>(value - up); break;
				case 3: destination[byte] = static_cast<uint8_t>(value - uint8_t((uint16_t(left) + up) / 2u)); break;
				case 4: destination[byte] = static_cast<uint8_t>(value - Paeth(left, up, upper_left)); break;
				default: destination[byte] = value; break;
			}
		}
		std::swap(previous, current);
	}

	size_t compressed_size = 0;
	const auto compression = Core::TryCompressZlib(filtered.get(), static_cast<size_t>(filtered_size),
	                                                compressed.get(), static_cast<size_t>(compressed_capacity),
	                                                parameters.compression, &compressed_size);
	if (compression == Core::ZlibCompressResult::OutputTooSmall)
		return compressed_capacity == uint64_t(parameters.png_size) - PNG_CONTAINER_BYTES ? DataOverflow : Fatal;
	if (compression != Core::ZlibCompressResult::Ok) return Fatal;
	uint64_t png_size = 0;
	if (!CheckedAdd(compressed_size, PNG_CONTAINER_BYTES, &png_size) ||
	    png_size > parameters.png_size || png_size > static_cast<uint64_t>(std::numeric_limits<int32_t>::max()))
	{
		return DataOverflow;
	}
	Buffer png = Allocate(png_size);
	if (!png) return Fatal;

	constexpr uint8_t signature[] = {137, 80, 78, 71, 13, 10, 26, 10};
	std::memcpy(png.get(), signature, sizeof(signature));
	size_t offset = sizeof(signature);
	uint8_t ihdr[13] {};
	WriteBigEndian32(ihdr, parameters.width);
	WriteBigEndian32(ihdr + 4, parameters.height);
	ihdr[8] = 8;
	ihdr[9] = parameters.color == 19 ? 6 : 2;
	WriteChunk(png.get(), &offset, "IHDR", ihdr, sizeof(ihdr));
	WriteChunk(png.get(), &offset, "IDAT", compressed.get(), static_cast<uint32_t>(compressed_size));
	WriteChunk(png.get(), &offset, "IEND", nullptr, 0);
	if (offset != png_size) return Fatal;

	std::memcpy(parameters.png, png.get(), static_cast<size_t>(png_size));
	if (info != nullptr)
	{
		const OutputInfo result {static_cast<uint32_t>(png_size), parameters.height};
		std::memcpy(info, &result, sizeof(result));
	}
	TraceEncodedImage(parameters, image.get(), required_input, png.get(), png_size);
	return static_cast<int32_t>(png_size);
}

int32_t LUMEN_SYSV_ABI Delete(Handle handle)
{
	PRINT_NAME();
	EncoderSlot* encoder = nullptr;
	{
		std::lock_guard<std::mutex> registry_lock(g_registry_mutex);
		encoder = FindLiveSlot(handle);
		if (encoder == nullptr) return InvalidHandle;
		encoder->state.store(SlotState::Closing, std::memory_order_release);
	}
	std::lock_guard<std::mutex> encoder_lock(encoder->mutex);
	encoder->state.store(SlotState::Retired, std::memory_order_release);
	return 0;
}

void ResetSessionState()
{
	std::lock_guard<std::mutex> registry_lock(g_registry_mutex);
	for (auto& encoder: g_encoders)
	{
		encoder.handle = nullptr;
		encoder.settings = {};
		encoder.serial = 0;
		encoder.state.store(SlotState::Empty, std::memory_order_release);
	}
	g_next_serial = 1;
}

#if defined(LUMEN_PNG_ENC_TEST_HOOK)
void ArmEncodePauseForTest(EncodePausePoint point)
{
	g_resume_encode.store(false, std::memory_order_release);
	g_encode_paused.store(false, std::memory_order_release);
	g_pause_point.store(static_cast<uint32_t>(point), std::memory_order_release);
}

bool EncodePausedForTest()
{
	return g_encode_paused.load(std::memory_order_acquire);
}

void ResumeEncodeForTest()
{
	g_resume_encode.store(true, std::memory_order_release);
}

bool IsHandleClosingForTest(Handle handle)
{
	std::lock_guard<std::mutex> registry_lock(g_registry_mutex);
	for (auto& encoder: g_encoders)
	{
		if (encoder.handle == handle && encoder.state.load(std::memory_order_acquire) == SlotState::Closing)
			return true;
	}
	return false;
}

void SetParallelEncodeBarrierForTest(uint32_t participants)
{
	if (participants != 0)
	{
		g_max_parallel_encodes.store(0, std::memory_order_release);
		g_parallel_released.store(false, std::memory_order_release);
	} else
	{
		g_parallel_released.store(true, std::memory_order_release);
	}
	g_parallel_goal.store(participants, std::memory_order_release);
}

uint32_t MaxParallelEncodesForTest()
{
	return g_max_parallel_encodes.load(std::memory_order_acquire);
}

void FailNextAllocationForTest()
{
	g_fail_next_allocation.store(true, std::memory_order_release);
}
#endif

} // namespace PngEnc

LIB_DEFINE(InitPngEnc_1)
{
	LIB_FUNC("9030RnBDoh4", PngEnc::QueryMemorySize);
	LIB_FUNC("7aGTPfrqT9s", PngEnc::Create);
	LIB_FUNC("xgDjJKpcyHo", PngEnc::Encode);
	LIB_FUNC("RUrWdwTWZy8", PngEnc::Delete);
}

} // namespace Lumen::Libs

#endif
