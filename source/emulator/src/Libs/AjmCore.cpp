#include "AjmCore.h"

#include <libatrac9.h>

#include <algorithm>
#include <array>
#include <climits>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

namespace Lumen::Libs::Ajm {

std::mutex& Atrac9LibraryMutex()
{
	// Decoder instances may outlive other function statics during shutdown.
	static auto* const mutex = new std::mutex;
	return *mutex;
}

namespace {

constexpr uint32_t CHUNK_JOB            = 0;
constexpr uint32_t CHUNK_INPUT_RUN      = 1;
constexpr uint32_t CHUNK_INPUT_CONTROL  = 2;
constexpr uint32_t CHUNK_CONTROL_FLAGS  = 3;
constexpr uint32_t CHUNK_RUN_FLAGS      = 4;
constexpr uint32_t CHUNK_RETURN_ADDRESS = 6;
constexpr uint32_t CHUNK_INLINE_BUFFER  = 7;
constexpr uint32_t CHUNK_OUTPUT_RUN     = 17;
constexpr uint32_t CHUNK_OUTPUT_CONTROL = 18;

constexpr uint32_t CHUNK_IDENT_MASK   = 0x0000003fu;
constexpr uint32_t CHUNK_PAYLOAD_MASK = 0x000fffffu;
constexpr uint32_t CHUNK_PAYLOAD_SHIFT = 6;
constexpr uint32_t CHUNK_RESERVED_SHIFT = 26;

constexpr uint64_t SERIALIZED_FLAGS_MASK = 0x000fffffffffffffull;
constexpr uint64_t CONTROL_SUPPORTED_FLAGS =
	AJM_FLAG_VERSION_MASK | AJM_FLAG_CONTROL_RESET | AJM_FLAG_CONTROL_INITIALIZE | AJM_FLAG_SIDEBAND_GAPLESS;
constexpr uint64_t RUN_SUPPORTED_FLAGS = AJM_FLAG_VERSION_MASK | AJM_FLAG_RUN_GET_CODEC_INFO | AJM_FLAG_RUN_MULTIPLE_FRAMES |
	                                         AJM_FLAG_SIDEBAND_STREAM;

constexpr uint32_t INSTANCE_INDEX_MASK       = 0x00003fffu;
constexpr uint32_t INSTANCE_CODEC_SHIFT      = 14;
constexpr uint64_t INSTANCE_CHANNELS_MASK    = 0x0000000000000078ull;
constexpr uint32_t INSTANCE_CHANNELS_SHIFT   = 3;
constexpr uint64_t INSTANCE_FORMAT_MASK      = 0x0000000000000380ull;
constexpr uint32_t INSTANCE_FORMAT_SHIFT     = 7;
constexpr uint64_t INSTANCE_GAPLESS_LOOP     = 0x0000000000000400ull;
constexpr uint64_t INSTANCE_BASE_FLAGS_MASK  = AJM_FLAG_VERSION_MASK | INSTANCE_CHANNELS_MASK | INSTANCE_FORMAT_MASK |
                                               INSTANCE_GAPLESS_LOOP;
constexpr uint32_t INSTANCE_CODEC_FLAGS_SHIFT = 32;

constexpr uint32_t FORMAT_S16 = 0;
constexpr uint32_t FORMAT_S32 = 1;
constexpr uint32_t FORMAT_F32 = 2;

constexpr size_t MAX_BATCH_SIZE = 64u * 1024u * 1024u;
constexpr size_t MAX_DATA_SIZE  = 64u * 1024u * 1024u;

uint32_t MakeChunkHeader(uint32_t ident, uint32_t payload)
{
	return (ident & CHUNK_IDENT_MASK) | ((payload & CHUNK_PAYLOAD_MASK) << CHUNK_PAYLOAD_SHIFT);
}

void WriteU32(uint8_t* dst, uint32_t value)
{
	std::memcpy(dst, &value, sizeof(value));
}

void WriteU64(uint8_t* dst, uint64_t value)
{
	std::memcpy(dst, &value, sizeof(value));
}

uint32_t ReadU32(const uint8_t* src)
{
	uint32_t value = 0;
	std::memcpy(&value, src, sizeof(value));
	return value;
}

uint64_t ReadU64(const uint8_t* src)
{
	uint64_t value = 0;
	std::memcpy(&value, src, sizeof(value));
	return value;
}

bool AddWouldOverflow(size_t a, size_t b)
{
	return b > std::numeric_limits<size_t>::max() - a;
}

bool MultiplyWouldOverflow(size_t a, size_t b)
{
	return a != 0 && b > std::numeric_limits<size_t>::max() / a;
}

bool AddressRangeWouldOverflow(uint64_t address, size_t size)
{
	const auto max_address = std::numeric_limits<uintptr_t>::max();
	if (address > static_cast<uint64_t>(max_address))
	{
		return true;
	}
	return size > static_cast<size_t>(max_address - static_cast<uintptr_t>(address));
}

bool PointerRangeWouldOverflow(const void* pointer, size_t size)
{
	return AddressRangeWouldOverflow(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(pointer)), size);
}

uint32_t BytesPerSample(uint32_t format)
{
	switch (format)
	{
		case FORMAT_S16: return 2;
		case FORMAT_S32:
		case FORMAT_F32: return 4;
		default: return 0;
	}
}

uint16_t SaturatingAddU16(uint16_t lhs, uint32_t rhs)
{
	const auto value = static_cast<uint32_t>(lhs) + rhs;
	return static_cast<uint16_t>(std::min(value, static_cast<uint32_t>(UINT16_MAX)));
}

bool ShouldLogTelemetry(uint64_t count)
{
	return count <= 3 || (count != 0 && (count & (count - 1u)) == 0);
}

class LibAtrac9Decoder final: public At9Decoder
{
public:
	LibAtrac9Decoder(uint32_t channels, uint32_t format, uint32_t codec_flags)
	    : m_channels(channels), m_format(format), m_codec_flags(codec_flags)
	{
	}

	~LibAtrac9Decoder() override
	{
		if (m_handle != nullptr)
		{
			std::lock_guard lock(Atrac9LibraryMutex());
			Atrac9ReleaseHandle(m_handle);
		}
	}

	int32_t Initialize(const std::array<uint8_t, 4>& config, At9CodecInfo* info) override
	{
		if (info == nullptr || config[0] != 0xfe || (config[1] & 1u) != 0 ||
		    ((config[1] >> 1u) & 7u) >= 6)
		{
			return -1;
		}

		Atrac9CodecInfo native_info {};
		const int32_t   result = InstallInitializedHandle(config, &native_info);
		if (result != 0)
		{
			return result;
		}

		info->channels             = static_cast<uint32_t>(native_info.channels);
		info->channel_config_index = static_cast<uint32_t>(native_info.channelConfigIndex);
		info->sampling_rate        = static_cast<uint32_t>(native_info.samplingRate);
		info->superframe_size      = static_cast<uint32_t>(native_info.superframeSize);
		info->frames_in_superframe = static_cast<uint32_t>(native_info.framesInSuperframe);
		info->frame_samples        = static_cast<uint32_t>(native_info.frameSamples);
		info->window_length        = static_cast<uint32_t>(native_info.wlength);
		std::copy(std::begin(native_info.configData), std::end(native_info.configData), info->config.begin());

		m_config      = config;
		m_initialized = true;
		m_pcm_size    = static_cast<size_t>(native_info.frameSamples) * static_cast<size_t>(native_info.channels) *
		                 BytesPerSample(m_format);
		return 0;
	}

	int32_t Reset() override
	{
		if (!m_initialized)
		{
			return 0;
		}

		Atrac9CodecInfo ignored {};
		return InstallInitializedHandle(m_config, &ignored);
	}

	int32_t DecodeFrame(const uint8_t* input, size_t input_size, void* pcm, size_t pcm_size, uint32_t format,
	                    bool non_interleaved, uint32_t* bytes_used) override
	{
		if (!m_initialized || input == nullptr || pcm == nullptr || bytes_used == nullptr || input_size > INT_MAX ||
		    format != m_format || m_channels == 0 || m_codec_flags != 0)
		{
			return -1;
		}

		const uint32_t sample_size = BytesPerSample(format);
		if (sample_size == 0 || m_pcm_size == 0 || pcm_size < m_pcm_size)
		{
			return -1;
		}

		int result      = -1;
		int native_used = 0;
		{
			std::lock_guard lock(Atrac9LibraryMutex());
			switch (format)
			{
				case FORMAT_S16:
					result = Atrac9Decode(m_handle, input, static_cast<int>(input_size), static_cast<short*>(pcm), &native_used,
					                       non_interleaved ? 1 : 0);
					break;
				case FORMAT_S32:
					result = Atrac9DecodeS32(m_handle, input, static_cast<int>(input_size), static_cast<int*>(pcm), &native_used,
					                          non_interleaved ? 1 : 0);
					break;
				case FORMAT_F32:
					result = Atrac9DecodeF32(m_handle, input, static_cast<int>(input_size), static_cast<float*>(pcm), &native_used,
					                          non_interleaved ? 1 : 0);
					break;
				default: break;
			}
		}

		if (result != 0)
		{
			return result;
		}
		if (native_used <= 0 || static_cast<size_t>(native_used) > input_size)
		{
			return -1;
		}

		*bytes_used = static_cast<uint32_t>(native_used);
		return 0;
	}

private:
	int32_t InstallInitializedHandle(const std::array<uint8_t, 4>& config, Atrac9CodecInfo* native_info)
	{
		std::lock_guard lock(Atrac9LibraryMutex());
		void*           candidate = Atrac9GetHandle();
		if (candidate == nullptr)
		{
			return -1;
		}

		auto      mutable_config = config;
		const int init_result    = Atrac9InitDecoder(candidate, mutable_config.data());
		const int info_result    = init_result == 0 ? Atrac9GetCodecInfo(candidate, native_info) : init_result;
		const uint32_t bytes_per_sample = BytesPerSample(m_format);
		const bool safe_sample_count = native_info->frameSamples > 0 && native_info->channels > 0 &&
		                               !MultiplyWouldOverflow(static_cast<size_t>(native_info->frameSamples),
		                                                      static_cast<size_t>(native_info->channels)) &&
		                               !MultiplyWouldOverflow(static_cast<size_t>(native_info->frameSamples) *
		                                                          static_cast<size_t>(native_info->channels),
		                                                      bytes_per_sample);
		const bool valid_info = info_result == 0 && native_info->channels > 0 && native_info->channels <= 8 &&
		                        native_info->channels == static_cast<int>(m_channels) && native_info->channelConfigIndex >= 0 &&
		                        native_info->channelConfigIndex < 6 && native_info->samplingRate > 0 &&
		                        native_info->superframeSize > 0 && native_info->superframeSize <= static_cast<int>(MAX_DATA_SIZE) &&
		                        native_info->framesInSuperframe > 0 && native_info->framesInSuperframe <= 16 &&
		                        native_info->frameSamples > 0 && native_info->frameSamples <= 4096 && native_info->wlength > 0 &&
		                        bytes_per_sample != 0 && safe_sample_count;
		if (!valid_info)
		{
			Atrac9ReleaseHandle(candidate);
			return info_result != 0 ? info_result : -1;
		}

		void* old_handle = m_handle;
		m_handle         = candidate;
		if (old_handle != nullptr)
		{
			Atrac9ReleaseHandle(old_handle);
		}
		return 0;
	}

	uint32_t               m_channels    = 0;
	uint32_t               m_format      = 0;
	uint32_t               m_codec_flags = 0;
	void*                  m_handle      = nullptr;
	std::array<uint8_t, 4> m_config {};
	bool                   m_initialized = false;
	size_t                 m_pcm_size    = 0;
};

class LibAtrac9Factory final: public At9DecoderFactory
{
public:
	bool Available() const override { return true; }

	std::unique_ptr<At9Decoder> Create(uint32_t channels, uint32_t format, uint32_t codec_flags) override
	{
		if (codec_flags != 0)
		{
			return {};
		}
		return std::make_unique<LibAtrac9Decoder>(channels, format, codec_flags);
	}
};

struct BufferRef
{
	uint8_t* data = nullptr;
	size_t   size = 0;
};

struct ParsedJob
{
	size_t                 job_offset     = 0;
	size_t                 return_address_offset = 0;
	const void*            return_address = nullptr;
	uint32_t               instance_id    = 0;
	uint64_t               flags          = 0;
	bool                   control        = false;
	bool                   has_flags      = false;
	bool                   has_return_address = false;
	bool                   has_side_input = false;
	bool                   has_side_output = false;
	BufferRef              side_input {};
	BufferRef              side_output {};
	std::vector<BufferRef> inputs;
	std::vector<BufferRef> outputs;
};

struct ParseFailure
{
	int32_t     result      = AJM_ERROR_MALFORMED_BATCH;
	int32_t     detail      = AJM_ERROR_MALFORMED_BATCH;
	size_t      job_offset  = 0;
	size_t      cmd_offset  = 0;
	const void* return_addr = nullptr;
};

class OutputCursor
{
public:
	explicit OutputCursor(const std::vector<BufferRef>& buffers): m_buffers(buffers) {}

	size_t Remaining() const
	{
		size_t total = 0;
		for (size_t i = m_index; i < m_buffers.size(); i++)
		{
			const size_t start = (i == m_index ? m_offset : 0);
			if (m_buffers[i].size >= start)
			{
				total += m_buffers[i].size - start;
			}
		}
		return total;
	}

	bool Write(const uint8_t* source, size_t size)
	{
		if (source == nullptr || Remaining() < size)
		{
			return false;
		}
		while (size != 0)
		{
			while (m_index < m_buffers.size() && m_offset == m_buffers[m_index].size)
			{
				m_index++;
				m_offset = 0;
			}
			if (m_index >= m_buffers.size())
			{
				return false;
			}
			const size_t chunk = std::min(size, m_buffers[m_index].size - m_offset);
			std::memcpy(m_buffers[m_index].data + m_offset, source, chunk);
			source += chunk;
			size -= chunk;
			m_offset += chunk;
		}
		return true;
	}

private:
	const std::vector<BufferRef>& m_buffers;
	size_t                        m_index  = 0;
	size_t                        m_offset = 0;
};

template <typename T>
bool ReadSideband(const BufferRef& buffer, size_t offset, T* value)
{
	if (value == nullptr || buffer.data == nullptr || offset > buffer.size || sizeof(T) > buffer.size - offset)
	{
		return false;
	}
	std::memcpy(value, buffer.data + offset, sizeof(T));
	return true;
}

template <typename T>
bool WriteSideband(const BufferRef& buffer, size_t offset, const T& value)
{
	if (buffer.data == nullptr || offset > buffer.size || sizeof(T) > buffer.size - offset)
	{
		return false;
	}
	std::memcpy(buffer.data + offset, &value, sizeof(T));
	return true;
}

} // namespace

class Runtime::Impl
{
public:
	explicit Impl(At9DecoderFactory* factory): m_factory(factory != nullptr ? factory : &DefaultAt9DecoderFactory()) {}

	int32_t Initialize(int64_t reserved, uint32_t* context)
	{
		if (reserved != 0 || context == nullptr)
		{
			return AJM_ERROR_INVALID_PARAMETER;
		}

		std::lock_guard lock(m_mutex);
		for (uint64_t attempts = 0; attempts < UINT32_MAX; attempts++)
		{
			const uint32_t id = m_next_context++;
			if (id == 0 || m_contexts.find(id) != m_contexts.end())
			{
				continue;
			}
			m_contexts.emplace(id, Context {});
			*context = id;
			return AJM_OK;
		}
		return AJM_ERROR_OUT_OF_RESOURCES;
	}

	int32_t Finalize(uint32_t context)
	{
		std::lock_guard lock(m_mutex);
		const auto      it = m_contexts.find(context);
		if (it == m_contexts.end())
		{
			return AJM_ERROR_INVALID_CONTEXT;
		}
		m_contexts.erase(it);
		return AJM_OK;
	}

	int32_t ModuleRegister(uint32_t context, uint32_t codec, int64_t reserved)
	{
		if (reserved != 0)
		{
			return AJM_ERROR_INVALID_PARAMETER;
		}

		std::lock_guard lock(m_mutex);
		auto*           ctx = FindContext(context);
		if (ctx == nullptr)
		{
			return AJM_ERROR_INVALID_CONTEXT;
		}
		if (codec != AJM_CODEC_AT9 || !m_factory->Available())
		{
			return AJM_ERROR_CODEC_NOT_SUPPORTED;
		}
		if (ctx->at9_registered)
		{
			return AJM_ERROR_CODEC_ALREADY_REGISTERED;
		}
		ctx->at9_registered = true;
		return AJM_OK;
	}

	int32_t ModuleUnregister(uint32_t context, uint32_t codec)
	{
		std::lock_guard lock(m_mutex);
		auto*           ctx = FindContext(context);
		if (ctx == nullptr)
		{
			return AJM_ERROR_INVALID_CONTEXT;
		}
		if (codec != AJM_CODEC_AT9)
		{
			return AJM_ERROR_CODEC_NOT_SUPPORTED;
		}
		if (!ctx->at9_registered)
		{
			return AJM_ERROR_CODEC_NOT_REGISTERED;
		}
		if (!ctx->instances.empty())
		{
			return AJM_ERROR_BUSY;
		}
		ctx->at9_registered = false;
		return AJM_OK;
	}

	int32_t InstanceCreate(uint32_t context, uint32_t codec, uint64_t flags, uint32_t* instance_id)
	{
		if (instance_id == nullptr)
		{
			return AJM_ERROR_INVALID_PARAMETER;
		}
		const uint32_t revision    = static_cast<uint32_t>(flags & AJM_FLAG_VERSION_MASK);
		const uint32_t channels    = static_cast<uint32_t>((flags & INSTANCE_CHANNELS_MASK) >> INSTANCE_CHANNELS_SHIFT);
		const uint32_t format      = static_cast<uint32_t>((flags & INSTANCE_FORMAT_MASK) >> INSTANCE_FORMAT_SHIFT);
		const uint32_t codec_flags = static_cast<uint32_t>(flags >> INSTANCE_CODEC_FLAGS_SHIFT);
		if (revision != 1)
		{
			return AJM_ERROR_WRONG_REVISION_FLAG;
		}
		if (channels == 0 || channels > 8 || BytesPerSample(format) == 0)
		{
			return AJM_ERROR_INVALID_PARAMETER;
		}
		if ((flags & ~(INSTANCE_BASE_FLAGS_MASK | (0xffffffffull << INSTANCE_CODEC_FLAGS_SHIFT))) != 0 || codec_flags != 0)
		{
			return AJM_ERROR_FLAG_NOT_SUPPORTED;
		}

		std::lock_guard lock(m_mutex);
		auto*           ctx = FindContext(context);
		if (ctx == nullptr)
		{
			return AJM_ERROR_INVALID_CONTEXT;
		}
		if (codec != AJM_CODEC_AT9 || !m_factory->Available())
		{
			return AJM_ERROR_CODEC_NOT_SUPPORTED;
		}
		if (!ctx->at9_registered)
		{
			return AJM_ERROR_CODEC_NOT_REGISTERED;
		}

		auto decoder = m_factory->Create(channels, format, codec_flags);
		if (decoder == nullptr)
		{
			return AJM_ERROR_OUT_OF_MEMORY;
		}

		for (uint32_t attempts = 0; attempts <= INSTANCE_INDEX_MASK; attempts++)
		{
			const uint32_t index = ctx->next_instance++ & INSTANCE_INDEX_MASK;
			const uint32_t id    = (codec << INSTANCE_CODEC_SHIFT) | index;
			if (ctx->instances.find(id) != ctx->instances.end())
			{
				continue;
			}

			Instance instance {};
			instance.id            = id;
			instance.flags         = flags;
			instance.channels      = channels;
			instance.format        = format;
			instance.gapless_loop  = (flags & INSTANCE_GAPLESS_LOOP) != 0;
			instance.decoder       = std::move(decoder);
			ctx->instances.emplace(id, std::move(instance));
			*instance_id = id;
			return AJM_OK;
		}
		return AJM_ERROR_OUT_OF_RESOURCES;
	}

	int32_t InstanceDestroy(uint32_t context, uint32_t instance_id)
	{
		std::lock_guard lock(m_mutex);
		auto*           ctx = FindContext(context);
		if (ctx == nullptr)
		{
			return AJM_ERROR_INVALID_CONTEXT;
		}
		const auto it = ctx->instances.find(instance_id);
		if (it == ctx->instances.end())
		{
			return AJM_ERROR_INVALID_INSTANCE;
		}
		ctx->instances.erase(it);
		return AJM_OK;
	}

	int32_t BatchStart(uint32_t context, uint8_t* batch, uint32_t batch_size, int32_t priority, BatchError* batch_error,
	                   uint32_t* batch_id)
	{
		ClearBatchError(batch_error);
		if (batch_id == nullptr || (batch == nullptr && batch_size != 0))
		{
			SetBatchError(batch_error, AJM_ERROR_INVALID_PARAMETER, batch, 0, nullptr);
			return AJM_ERROR_INVALID_PARAMETER;
		}
		if (batch_size != 0 && PointerRangeWouldOverflow(batch, batch_size))
		{
			SetBatchError(batch_error, AJM_ERROR_INVALID_ADDRESS, batch, 0, nullptr);
			return AJM_ERROR_INVALID_ADDRESS;
		}
		if (batch_size > MAX_BATCH_SIZE)
		{
			SetBatchError(batch_error, AJM_ERROR_BUFFER_TOO_BIG, batch, 0, nullptr);
			return AJM_ERROR_BUFFER_TOO_BIG;
		}
		if (priority < 0 || priority > 255)
		{
			SetBatchError(batch_error, AJM_ERROR_BAD_PRIORITY, batch, 0, nullptr);
			return AJM_ERROR_BAD_PRIORITY;
		}

		std::lock_guard lock(m_mutex);
		auto*           ctx = FindContext(context);
		if (ctx == nullptr)
		{
			SetBatchError(batch_error, AJM_ERROR_INVALID_CONTEXT, batch, 0, nullptr);
			return AJM_ERROR_INVALID_CONTEXT;
		}

		std::vector<ParsedJob> jobs;
		ParseFailure           failure {};
		const int32_t          parse_result = ParseBatch(*ctx, batch, batch_size, &jobs, &failure);
		if (parse_result != AJM_OK)
		{
			const void* job_addr = batch != nullptr && failure.job_offset <= batch_size ? batch + failure.job_offset : batch;
			SetBatchError(batch_error, failure.detail, job_addr, failure.cmd_offset, failure.return_addr);
			return parse_result;
		}

		for (auto& job: jobs)
		{
			auto instance_it = ctx->instances.find(job.instance_id);
			if (instance_it == ctx->instances.end())
			{
				SetBatchError(batch_error, AJM_ERROR_INVALID_INSTANCE, batch + job.job_offset, 0, job.return_address);
				return AJM_ERROR_INVALID_INSTANCE;
			}
			if (job.control)
			{
				ProcessControl(instance_it->second, job);
			} else
			{
				ProcessRun(instance_it->second, job);
			}
		}

		for (uint64_t attempts = 0; attempts < UINT32_MAX; attempts++)
		{
			const uint32_t id = ctx->next_batch++;
			if (id == 0 || ctx->batches.find(id) != ctx->batches.end())
			{
				continue;
			}
			ctx->batches.emplace(id, true);
			*batch_id = id;
			return AJM_OK;
		}
		return AJM_ERROR_OUT_OF_RESOURCES;
	}

	int32_t BatchWait(uint32_t context, uint32_t batch_id, uint32_t /*timeout*/, BatchError* batch_error)
	{
		ClearBatchError(batch_error);
		std::lock_guard lock(m_mutex);
		auto*           ctx = FindContext(context);
		if (ctx == nullptr)
		{
			SetBatchError(batch_error, AJM_ERROR_INVALID_CONTEXT, nullptr, 0, nullptr);
			return AJM_ERROR_INVALID_CONTEXT;
		}
		const auto it = ctx->batches.find(batch_id);
		if (it == ctx->batches.end())
		{
			SetBatchError(batch_error, AJM_ERROR_INVALID_BATCH, nullptr, 0, nullptr);
			return AJM_ERROR_INVALID_BATCH;
		}
		ctx->batches.erase(it);
		return AJM_OK;
	}

	bool ResetForNextSession()
	{
		// Move the ownership graph out while serialized, then release decoder
		// resources without holding the runtime mutex. Guest execution is idle,
		// so a newly initialized context cannot race this reset contract.
		std::map<uint32_t, Context> retired_contexts;
		{
			std::lock_guard lock(m_mutex);
			retired_contexts.swap(m_contexts);
			m_next_context = 1;
		}
		retired_contexts.clear();

		std::lock_guard lock(m_mutex);
		return m_contexts.empty() && m_next_context == 1;
	}

private:
	struct Instance
	{
		uint32_t                    id             = 0;
		uint64_t                    flags          = 0;
		uint32_t                    channels       = 0;
		uint32_t                    format         = 0;
		bool                        gapless_loop   = false;
		bool                        initialized    = false;
		bool                        gapless_active = false;
		std::array<uint8_t, 4>      config {};
		At9CodecInfo                info {};
		uint32_t                    superframe_remaining = 0;
		uint32_t                    frame_in_superframe  = 0;
		uint64_t                    total_decoded_samples = 0;
		uint64_t                    run_jobs                = 0;
		Gapless                     gapless_config {};
		Gapless                     gapless_current {};
		std::unique_ptr<At9Decoder> decoder;
	};

	struct Context
	{
		bool                         at9_registered = false;
		uint32_t                     next_instance  = 0;
		uint32_t                     next_batch     = 1;
		std::map<uint32_t, Instance> instances;
		std::map<uint32_t, bool>     batches;
	};

	Context* FindContext(uint32_t context)
	{
		const auto it = m_contexts.find(context);
		return it == m_contexts.end() ? nullptr : &it->second;
	}

	static void ClearBatchError(BatchError* error)
	{
		if (error != nullptr)
		{
			*error = {};
		}
	}

	static void SetBatchError(BatchError* error, int32_t code, const void* job, size_t offset, const void* return_addr)
	{
		if (error == nullptr)
		{
			return;
		}
		error->error_code = code;
		error->job_addr   = job;
		error->cmd_offset = static_cast<uint32_t>(std::min(offset, static_cast<size_t>(UINT32_MAX)));
		error->job_ra     = return_addr;
	}

	static bool DecodeBufferChunk(const uint8_t* batch, size_t cursor, size_t end, uint32_t header, BufferRef* buffer,
	                              ParseFailure* failure, size_t job_offset, const void* return_addr)
	{
		if (end - cursor < 16 || (header >> CHUNK_RESERVED_SHIFT) != 0 || ((header >> CHUNK_PAYLOAD_SHIFT) & CHUNK_PAYLOAD_MASK) != 0)
		{
			failure->job_offset  = job_offset;
			failure->cmd_offset  = cursor - job_offset;
			failure->return_addr = return_addr;
			return false;
		}
		const uint32_t size       = ReadU32(batch + cursor + 4);
		const uint64_t raw_pointer = ReadU64(batch + cursor + 8);
		if (size != 0 && (raw_pointer == 0 || AddressRangeWouldOverflow(raw_pointer, size)))
		{
			failure->result      = AJM_ERROR_INVALID_ADDRESS;
			failure->detail      = AJM_ERROR_INVALID_ADDRESS;
			failure->job_offset  = job_offset;
			failure->cmd_offset  = cursor - job_offset;
			failure->return_addr = return_addr;
			return false;
		}
		buffer->data = reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(raw_pointer));
		buffer->size = size;
		return true;
	}

	static int32_t ParseBatch(const Context& ctx, const uint8_t* batch, size_t batch_size, std::vector<ParsedJob>* jobs,
	                          ParseFailure* failure)
	{
		if ((batch_size & 7u) != 0)
		{
			return AJM_ERROR_MALFORMED_BATCH;
		}

		size_t cursor = 0;
		while (cursor < batch_size)
		{
			const size_t job_offset = cursor;
			if (batch_size - cursor < 8)
			{
				failure->job_offset = job_offset;
				return AJM_ERROR_MALFORMED_BATCH;
			}
			const uint32_t job_header = ReadU32(batch + cursor);
			const uint32_t body_size  = ReadU32(batch + cursor + 4);
			if ((job_header & CHUNK_IDENT_MASK) != CHUNK_JOB || (job_header >> CHUNK_RESERVED_SHIFT) != 0 ||
			    (body_size & 7u) != 0 || body_size > batch_size - cursor - 8)
			{
				failure->job_offset = job_offset;
				return AJM_ERROR_MALFORMED_BATCH;
			}

			ParsedJob job {};
			job.job_offset  = job_offset;
			job.instance_id = (job_header >> CHUNK_PAYLOAD_SHIFT) & CHUNK_PAYLOAD_MASK;
			cursor += 8;
			const size_t job_end = cursor + body_size;
			while (cursor < job_end)
			{
				if (job_end - cursor < 4)
				{
					failure->job_offset  = job_offset;
					failure->cmd_offset  = cursor - job_offset;
					failure->return_addr = job.return_address;
					return AJM_ERROR_MALFORMED_BATCH;
				}
				const uint32_t header = ReadU32(batch + cursor);
				const uint32_t ident  = header & CHUNK_IDENT_MASK;
				switch (ident)
				{
					case CHUNK_CONTROL_FLAGS:
					case CHUNK_RUN_FLAGS:
						if (job.has_flags || job_end - cursor < 8 || (header >> CHUNK_RESERVED_SHIFT) != 0)
						{
							failure->job_offset  = job_offset;
							failure->cmd_offset  = cursor - job_offset;
							failure->return_addr = job.return_address;
							return AJM_ERROR_MALFORMED_BATCH;
						}
						job.flags = (static_cast<uint64_t>((header >> CHUNK_PAYLOAD_SHIFT) & CHUNK_PAYLOAD_MASK) << 32u) |
						            ReadU32(batch + cursor + 4);
						job.has_flags = true;
						job.control   = ident == CHUNK_CONTROL_FLAGS;
						cursor += 8;
						break;
					case CHUNK_RETURN_ADDRESS:
					{
						if (job.has_return_address)
						{
							failure->job_offset  = job_offset;
							failure->cmd_offset  = cursor - job_offset;
							failure->return_addr = job.return_address;
							return AJM_ERROR_MALFORMED_BATCH;
						}
						BufferRef address {};
						if (!DecodeBufferChunk(batch, cursor, job_end, header, &address, failure, job_offset, job.return_address))
						{
							return failure->result;
						}
						if (address.size != 0)
						{
							failure->job_offset  = job_offset;
							failure->cmd_offset  = cursor - job_offset;
							failure->return_addr = job.return_address;
							return AJM_ERROR_MALFORMED_BATCH;
						}
						job.has_return_address    = true;
						job.return_address        = address.data;
						job.return_address_offset = cursor - job_offset;
						cursor += 16;
						break;
					}
					case CHUNK_INPUT_CONTROL:
					case CHUNK_OUTPUT_CONTROL:
					case CHUNK_INPUT_RUN:
					case CHUNK_OUTPUT_RUN:
					{
						BufferRef buffer {};
						if (!DecodeBufferChunk(batch, cursor, job_end, header, &buffer, failure, job_offset, job.return_address))
						{
							return failure->result;
						}
						if (ident == CHUNK_INPUT_CONTROL)
						{
							if (job.has_side_input)
							{
								failure->job_offset  = job_offset;
								failure->cmd_offset  = cursor - job_offset;
								failure->return_addr = job.return_address;
								return AJM_ERROR_MALFORMED_BATCH;
							}
							job.has_side_input = true;
							job.side_input     = buffer;
						} else if (ident == CHUNK_OUTPUT_CONTROL)
						{
							if (job.has_side_output)
							{
								failure->job_offset  = job_offset;
								failure->cmd_offset  = cursor - job_offset;
								failure->return_addr = job.return_address;
								return AJM_ERROR_MALFORMED_BATCH;
							}
							job.has_side_output = true;
							job.side_output     = buffer;
						} else if (ident == CHUNK_INPUT_RUN)
						{
							job.inputs.push_back(buffer);
						} else
						{
							job.outputs.push_back(buffer);
						}
						cursor += 16;
						break;
					}
					case CHUNK_INLINE_BUFFER:
					default:
						failure->detail      = AJM_ERROR_INVALID_OPCODE;
						failure->job_offset  = job_offset;
						failure->cmd_offset  = cursor - job_offset;
						failure->return_addr = job.return_address;
						return AJM_ERROR_MALFORMED_BATCH;
				}
			}

			if (cursor != job_end || !job.has_flags || (job.control && (!job.inputs.empty() || !job.outputs.empty())) ||
			    (!job.control && job.has_side_input))
			{
				failure->job_offset  = job_offset;
				failure->cmd_offset  = cursor - job_offset;
				failure->return_addr = job.return_address;
				return AJM_ERROR_MALFORMED_BATCH;
			}
			if (ctx.instances.find(job.instance_id) == ctx.instances.end())
			{
				failure->result      = AJM_ERROR_INVALID_INSTANCE;
				failure->detail      = AJM_ERROR_INVALID_INSTANCE;
				failure->job_offset  = job_offset;
				failure->cmd_offset  = 0;
				failure->return_addr = job.return_address;
				return AJM_ERROR_INVALID_INSTANCE;
			}
			jobs->push_back(std::move(job));
		}
		return AJM_OK;
	}

	static void SetResult(const ParsedJob& job, Result result)
	{
		WriteSideband(job.side_output, 0, result);
	}

	static void ResetStreamState(Instance& instance)
	{
		instance.superframe_remaining = instance.info.superframe_size;
		instance.frame_in_superframe  = 0;
		instance.total_decoded_samples = 0;
		instance.gapless_current       = instance.gapless_config;
	}

	static bool ValidateCodecInfo(const Instance& instance, const At9CodecInfo& info)
	{
		if (info.channels == 0 || info.channels > 8 || info.channels != instance.channels || info.sampling_rate == 0 ||
		    info.superframe_size == 0 || info.frames_in_superframe == 0 || info.frames_in_superframe > 16 ||
		    info.frame_samples == 0 || info.frame_samples > 4096)
		{
			return false;
		}
		const size_t bytes_per_sample = BytesPerSample(instance.format);
		return !MultiplyWouldOverflow(info.frame_samples, info.channels) &&
		       !MultiplyWouldOverflow(static_cast<size_t>(info.frame_samples) * info.channels, bytes_per_sample);
	}

	static void ProcessControl(Instance& instance, const ParsedJob& job)
	{
		struct InitializeData
		{
			std::array<uint8_t, 4> config {};
			uint32_t               reserved = 0;
		};

		Result result {};
		if ((job.flags & AJM_FLAG_VERSION_MASK) != 1 || (job.flags & ~CONTROL_SUPPORTED_FLAGS) != 0)
		{
			result.result |= AJM_JOB_UNSUPPORTED_FLAG;
		}
		if ((job.flags & (AJM_FLAG_CONTROL_RESAMPLE | AJM_FLAG_SIDEBAND_FORMAT)) != 0)
		{
			result.result |= AJM_JOB_UNSUPPORTED_FLAG;
		}
		if ((result.result & AJM_JOB_UNSUPPORTED_FLAG) != 0)
		{
			SetResult(job, result);
			return;
		}

		const bool wants_gapless   = (job.flags & AJM_FLAG_SIDEBAND_GAPLESS) != 0;
		const bool wants_initialize = (job.flags & AJM_FLAG_CONTROL_INITIALIZE) != 0;
		size_t     input_offset     = 0;
		Gapless    gapless {};
		InitializeData init {};
		if (wants_gapless)
		{
			if (!ReadSideband(job.side_input, input_offset, &gapless))
			{
				result.result |= AJM_JOB_INVALID_PARAMETER;
			}
			input_offset += sizeof(Gapless);
		}
		if (wants_initialize)
		{
			if (!ReadSideband(job.side_input, input_offset, &init) || init.reserved != 0 || init.config[0] != 0xfe ||
			    (init.config[1] & 1u) != 0 || ((init.config[1] >> 1u) & 7u) >= 6)
			{
				result.result |= AJM_JOB_INVALID_DATA;
			}
		}
		if (result.result != AJM_OK)
		{
			SetResult(job, result);
			return;
		}

		if ((job.flags & AJM_FLAG_CONTROL_RESET) != 0)
		{
			if (instance.initialized)
			{
				const int32_t reset_result = instance.decoder->Reset();
				if (reset_result != 0)
				{
					result.result |= AJM_JOB_CODEC_ERROR;
					result.internal_result = reset_result;
					instance.initialized = false;
					SetResult(job, result);
					return;
				}
			}
			ResetStreamState(instance);
		}

		if (wants_gapless)
		{
			instance.gapless_config  = gapless;
			instance.gapless_current = gapless;
			instance.gapless_active  = true;
		}

		if (wants_initialize)
		{
			At9CodecInfo info {};
			const int32_t init_result = instance.decoder->Initialize(init.config, &info);
			if (init_result != 0)
			{
				result.result |= AJM_JOB_CODEC_ERROR;
				result.internal_result = init_result;
			} else if (!ValidateCodecInfo(instance, info))
			{
				result.result |= AJM_JOB_INVALID_DATA;
			} else
			{
				instance.config      = init.config;
				instance.info        = info;
				instance.initialized = true;
				ResetStreamState(instance);
			}
		}

		SetResult(job, result);
	}

	static bool GatherInput(const ParsedJob& job, std::vector<uint8_t>* input)
	{
		size_t total = 0;
		for (const auto& buffer: job.inputs)
		{
			if (buffer.size > MAX_DATA_SIZE || AddWouldOverflow(total, buffer.size) || total + buffer.size > MAX_DATA_SIZE)
			{
				return false;
			}
			total += buffer.size;
		}
		input->reserve(total);
		for (const auto& buffer: job.inputs)
		{
			if (buffer.size != 0)
			{
				input->insert(input->end(), buffer.data, buffer.data + buffer.size);
			}
		}
		return true;
	}

	static bool ValidateOutputSize(const ParsedJob& job)
	{
		size_t total = 0;
		for (const auto& buffer: job.outputs)
		{
			if (buffer.size > MAX_DATA_SIZE || AddWouldOverflow(total, buffer.size) || total + buffer.size > MAX_DATA_SIZE)
			{
				return false;
			}
			total += buffer.size;
		}
		return true;
	}

	static void ProcessRun(Instance& instance, const ParsedJob& job)
	{
		Result        result {};
		Stream        stream {};
		MultipleFrame multiple {};
		At9Info       at9_info {};

		if ((job.flags & AJM_FLAG_VERSION_MASK) != 1 || (job.flags & ~RUN_SUPPORTED_FLAGS) != 0 ||
		    (job.flags & (AJM_FLAG_SIDEBAND_FORMAT | AJM_FLAG_SIDEBAND_GAPLESS)) != 0)
		{
			result.result |= AJM_JOB_UNSUPPORTED_FLAG;
		}

		std::vector<uint8_t> input;
		if (!GatherInput(job, &input) || !ValidateOutputSize(job))
		{
			result.result |= AJM_JOB_INVALID_PARAMETER;
		}

		if (!instance.initialized)
		{
			result.result |= AJM_JOB_NOT_INITIALIZED;
		} else
		{
			at9_info.superframe_size      = instance.info.superframe_size;
			at9_info.frames_in_superframe = instance.info.frames_in_superframe;
			at9_info.next_frame_size      = instance.superframe_remaining;
			at9_info.frame_samples        = instance.info.frame_samples;
		}

		const bool wants_decode = !input.empty() && (result.result & (AJM_JOB_NOT_INITIALIZED | AJM_JOB_UNSUPPORTED_FLAG |
		                                                             AJM_JOB_INVALID_PARAMETER)) == 0;
		if (wants_decode)
		{
			const size_t bytes_per_sample = BytesPerSample(instance.format);
			const size_t frame_bytes = static_cast<size_t>(instance.info.frame_samples) * instance.info.channels * bytes_per_sample;
			std::vector<uint8_t> pcm(frame_bytes);
			OutputCursor         output(job.outputs);
			size_t               input_offset = 0;

			while (input_offset < input.size())
			{
				if (instance.superframe_remaining == 0 || instance.superframe_remaining > instance.info.superframe_size)
				{
					result.result |= AJM_JOB_FATAL;
					break;
				}
				const size_t input_remaining = input.size() - input_offset;
				if (input_remaining < instance.superframe_remaining)
				{
					result.result |= AJM_JOB_PARTIAL_INPUT;
					break;
				}

				uint32_t skip_samples = 0;
				uint32_t write_samples = instance.info.frame_samples;
				if (instance.gapless_active)
				{
					skip_samples = std::min<uint32_t>(instance.gapless_current.skip_samples, instance.info.frame_samples);
					write_samples -= skip_samples;
					if (instance.gapless_config.total_samples != 0)
					{
						write_samples = std::min(write_samples, instance.gapless_current.total_samples);
					}
				}
				const size_t write_bytes = static_cast<size_t>(write_samples) * instance.info.channels * bytes_per_sample;
				if (output.Remaining() < write_bytes)
				{
					result.result |= AJM_JOB_NOT_ENOUGH_ROOM;
					break;
				}

				uint32_t bytes_used = 0;
				const int32_t decode_result = instance.decoder->DecodeFrame(input.data() + input_offset, input_remaining, pcm.data(),
				                                                               pcm.size(), instance.format, false, &bytes_used);
				if (decode_result != 0 || bytes_used == 0 || bytes_used > instance.superframe_remaining)
				{
					result.result |= AJM_JOB_CODEC_ERROR;
					result.internal_result = decode_result != 0 ? decode_result : -1;
					break;
				}

				size_t consumed = bytes_used;
				instance.superframe_remaining -= bytes_used;
				instance.frame_in_superframe++;
				if (instance.frame_in_superframe == instance.info.frames_in_superframe)
				{
					consumed += instance.superframe_remaining;
					instance.superframe_remaining = instance.info.superframe_size;
					instance.frame_in_superframe  = 0;
				}
				if (consumed > input_remaining)
				{
					result.result |= AJM_JOB_FATAL;
					break;
				}

				if (write_bytes != 0)
				{
					const size_t pcm_offset = static_cast<size_t>(skip_samples) * instance.info.channels * bytes_per_sample;
					if (!output.Write(pcm.data() + pcm_offset, write_bytes))
					{
						result.result |= AJM_JOB_FATAL;
						break;
					}
				}

				if (instance.gapless_active)
				{
					instance.gapless_current.skip_samples -= static_cast<uint16_t>(skip_samples);
					const uint32_t discarded = instance.info.frame_samples - write_samples;
					instance.gapless_current.skipped_samples =
					    SaturatingAddU16(instance.gapless_current.skipped_samples, discarded);
					if (instance.gapless_config.total_samples != 0)
					{
						instance.gapless_current.total_samples -= write_samples;
					}
				}

				input_offset += consumed;
				stream.input_consumed += static_cast<int32_t>(consumed);
				stream.output_written += static_cast<int32_t>(write_bytes);
				instance.total_decoded_samples += write_samples;
				multiple.num_frames++;

				const bool end_of_gapless = instance.gapless_active && instance.gapless_config.total_samples != 0 &&
				                             instance.gapless_current.total_samples == 0;
				if (end_of_gapless)
				{
					if (!instance.gapless_loop)
					{
						break;
					}
					if (instance.frame_in_superframe == 0)
					{
						const int32_t reset_result = instance.decoder->Reset();
						if (reset_result != 0)
						{
							result.result |= AJM_JOB_CODEC_ERROR;
							result.internal_result = reset_result;
							break;
						}
						ResetStreamState(instance);
					}
				}

				if ((job.flags & AJM_FLAG_RUN_MULTIPLE_FRAMES) == 0)
				{
					break;
				}
			}
		}

		stream.total_decoded_samples = instance.total_decoded_samples;
		if (instance.initialized)
		{
			at9_info.next_frame_size = instance.superframe_remaining;
		}

		size_t side_offset = sizeof(Result);
		if ((job.flags & AJM_FLAG_SIDEBAND_STREAM) != 0)
		{
			if (!WriteSideband(job.side_output, side_offset, stream))
			{
				result.result |= AJM_JOB_SIDEBAND_TRUNCATED;
			}
			side_offset += sizeof(Stream);
		}
		if ((job.flags & AJM_FLAG_RUN_MULTIPLE_FRAMES) != 0)
		{
			if (!WriteSideband(job.side_output, side_offset, multiple))
			{
				result.result |= AJM_JOB_SIDEBAND_TRUNCATED;
			}
			side_offset += sizeof(MultipleFrame);
		}
		if ((job.flags & AJM_FLAG_RUN_GET_CODEC_INFO) != 0)
		{
			if (!WriteSideband(job.side_output, side_offset, at9_info))
			{
				result.result |= AJM_JOB_SIDEBAND_TRUNCATED;
			}
		}
		SetResult(job, result);

		const uint64_t run_number = ++instance.run_jobs;
		if (std::getenv("LUMEN_CUSTOM_LOG") != nullptr && ShouldLogTelemetry(run_number))
		{
			std::fprintf(stderr,
			             "AJM AT9 instance=0x%08x run=%llu input_consumed=%d pcm_written=%d frames=%u result=0x%08x internal=0x%08x\n",
			             instance.id, static_cast<unsigned long long>(run_number), stream.input_consumed, stream.output_written,
			             multiple.num_frames, static_cast<uint32_t>(result.result), static_cast<uint32_t>(result.internal_result));
		}
	}

	At9DecoderFactory*         m_factory = nullptr;
	std::mutex                 m_mutex;
	uint32_t                   m_next_context = 1;
	std::map<uint32_t, Context> m_contexts;
};

Runtime::Runtime(At9DecoderFactory* factory): m_impl(std::make_unique<Impl>(factory)) {}
Runtime::~Runtime() = default;

int32_t Runtime::Initialize(int64_t reserved, uint32_t* context)
{
	return m_impl->Initialize(reserved, context);
}

int32_t Runtime::Finalize(uint32_t context)
{
	return m_impl->Finalize(context);
}

int32_t Runtime::ModuleRegister(uint32_t context, uint32_t codec, int64_t reserved)
{
	return m_impl->ModuleRegister(context, codec, reserved);
}

int32_t Runtime::ModuleUnregister(uint32_t context, uint32_t codec)
{
	return m_impl->ModuleUnregister(context, codec);
}

int32_t Runtime::InstanceCreate(uint32_t context, uint32_t codec, uint64_t flags, uint32_t* instance_id)
{
	return m_impl->InstanceCreate(context, codec, flags, instance_id);
}

int32_t Runtime::InstanceDestroy(uint32_t context, uint32_t instance_id)
{
	return m_impl->InstanceDestroy(context, instance_id);
}

int32_t Runtime::BatchStart(uint32_t context, uint8_t* batch, uint32_t batch_size, int32_t priority, BatchError* batch_error,
	                        uint32_t* batch_id)
{
	return m_impl->BatchStart(context, batch, batch_size, priority, batch_error, batch_id);
}

int32_t Runtime::BatchWait(uint32_t context, uint32_t batch_id, uint32_t timeout, BatchError* batch_error)
{
	return m_impl->BatchWait(context, batch_id, timeout, batch_error);
}

bool Runtime::ResetForNextSession()
{
	return m_impl->ResetForNextSession();
}

void* BatchJobControlBufferRa(void* batch, uint32_t instance_id, uint64_t flags, const void* sideband_input,
                              size_t sideband_input_size, void* sideband_output, size_t sideband_output_size,
                              const void* return_address)
{
	if (batch == nullptr || PointerRangeWouldOverflow(batch, 0x40) || instance_id > CHUNK_PAYLOAD_MASK ||
	    sideband_input_size > UINT32_MAX || sideband_output_size > UINT32_MAX ||
	    (sideband_input == nullptr && sideband_input_size != 0) ||
	    (sideband_output == nullptr && sideband_output_size != 0) ||
	    PointerRangeWouldOverflow(sideband_input, sideband_input_size) ||
	    PointerRangeWouldOverflow(sideband_output, sideband_output_size))
	{
		return nullptr;
	}

	auto*       start  = static_cast<uint8_t*>(batch);
	auto*       cursor = start + 8;
	const auto append_buffer = [&cursor](uint32_t ident, const void* pointer, size_t size) {
		WriteU32(cursor, MakeChunkHeader(ident, 0));
		WriteU32(cursor + 4, static_cast<uint32_t>(size));
		WriteU64(cursor + 8, reinterpret_cast<uintptr_t>(pointer));
		cursor += 16;
	};

	append_buffer(CHUNK_RETURN_ADDRESS, return_address, 0);
	append_buffer(CHUNK_INPUT_CONTROL, sideband_input, sideband_input_size);
	flags &= SERIALIZED_FLAGS_MASK;
	WriteU32(cursor, MakeChunkHeader(CHUNK_CONTROL_FLAGS, static_cast<uint32_t>(flags >> 32u)));
	WriteU32(cursor + 4, static_cast<uint32_t>(flags));
	cursor += 8;
	append_buffer(CHUNK_OUTPUT_CONTROL, sideband_output, sideband_output_size);

	WriteU32(start, MakeChunkHeader(CHUNK_JOB, instance_id));
	WriteU32(start + 4, static_cast<uint32_t>(cursor - start - 8));
	return cursor;
}

void* BatchJobRunBufferRa(void* batch, uint32_t instance_id, uint64_t flags, const void* input, size_t input_size, void* output,
                          size_t output_size, void* sideband_output, size_t sideband_output_size, const void* return_address)
{
	if (batch == nullptr || PointerRangeWouldOverflow(batch, 0x50) || instance_id > CHUNK_PAYLOAD_MASK || input_size > UINT32_MAX ||
	    output_size > UINT32_MAX || sideband_output_size > UINT32_MAX || (input == nullptr && input_size != 0) ||
	    (output == nullptr && output_size != 0) ||
	    (sideband_output == nullptr && sideband_output_size != 0) || PointerRangeWouldOverflow(input, input_size) ||
	    PointerRangeWouldOverflow(output, output_size) || PointerRangeWouldOverflow(sideband_output, sideband_output_size))
	{
		return nullptr;
	}

	auto*       start  = static_cast<uint8_t*>(batch);
	auto*       cursor = start + 8;
	const auto append_buffer = [&cursor](uint32_t ident, const void* pointer, size_t size) {
		WriteU32(cursor, MakeChunkHeader(ident, 0));
		WriteU32(cursor + 4, static_cast<uint32_t>(size));
		WriteU64(cursor + 8, reinterpret_cast<uintptr_t>(pointer));
		cursor += 16;
	};

	append_buffer(CHUNK_RETURN_ADDRESS, return_address, 0);
	append_buffer(CHUNK_INPUT_RUN, input, input_size);
	flags &= SERIALIZED_FLAGS_MASK;
	WriteU32(cursor, MakeChunkHeader(CHUNK_RUN_FLAGS, static_cast<uint32_t>(flags >> 32u)));
	WriteU32(cursor + 4, static_cast<uint32_t>(flags));
	cursor += 8;
	append_buffer(CHUNK_OUTPUT_RUN, output, output_size);
	append_buffer(CHUNK_OUTPUT_CONTROL, sideband_output, sideband_output_size);

	WriteU32(start, MakeChunkHeader(CHUNK_JOB, instance_id));
	WriteU32(start + 4, static_cast<uint32_t>(cursor - start - 8));
	return cursor;
}

void BatchErrorDump(const void* batch, const BatchError* error)
{
	if (error == nullptr)
	{
		std::printf("AJM batch %p: no error record\n", batch);
		return;
	}
	std::printf("AJM batch %p: error=0x%08x job=%p command_offset=0x%x return_address=%p\n", batch,
	            static_cast<uint32_t>(error->error_code), error->job_addr, error->cmd_offset, error->job_ra);
}

At9DecoderFactory& DefaultAt9DecoderFactory()
{
	static LibAtrac9Factory factory;
	return factory;
}

Runtime& GlobalRuntime()
{
	static Runtime runtime;
	return runtime;
}

bool ResetForNextSession()
{
	return GlobalRuntime().ResetForNextSession();
}

#if defined(LUMEN_RUNTIME_LIFECYCLE_TEST_HOOK)
bool SessionResetContractTest()
{
	auto& runtime = GlobalRuntime();
	if (!ResetForNextSession())
	{
		return false;
	}

	uint32_t first_context = 0;
	uint32_t first_instance = 0;
	const bool first_session_ready = runtime.Initialize(0, &first_context) == AJM_OK && first_context == 1 &&
	                                 runtime.ModuleRegister(first_context, AJM_CODEC_AT9, 0) == AJM_OK &&
	                                 runtime.InstanceCreate(first_context, AJM_CODEC_AT9, 0x411, &first_instance) == AJM_OK;
	if (!first_session_ready)
	{
		(void)ResetForNextSession();
		return false;
	}

	if (!ResetForNextSession() || runtime.Finalize(first_context) != AJM_ERROR_INVALID_CONTEXT)
	{
		(void)ResetForNextSession();
		return false;
	}

	uint32_t second_context = 0;
	const bool second_session_ready = runtime.Initialize(0, &second_context) == AJM_OK && second_context == 1;
	return ResetForNextSession() && second_session_ready;
}
#endif

} // namespace Lumen::Libs::Ajm
