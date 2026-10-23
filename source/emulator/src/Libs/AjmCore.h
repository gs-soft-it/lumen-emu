#ifndef EMULATOR_SRC_LIBS_AJM_CORE_H_
#define EMULATOR_SRC_LIBS_AJM_CORE_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>

namespace Lumen::Libs::Ajm {

// libatrac9 initializes shared process tables and is serialized across every
// local caller, including AJM and the opt-in NGS2 renderer.
std::mutex& Atrac9LibraryMutex();

constexpr int32_t AJM_OK = 0;

constexpr int32_t AJM_ERROR_INVALID_CONTEXT          = static_cast<int32_t>(0x80930002u);
constexpr int32_t AJM_ERROR_INVALID_INSTANCE         = static_cast<int32_t>(0x80930003u);
constexpr int32_t AJM_ERROR_INVALID_BATCH            = static_cast<int32_t>(0x80930004u);
constexpr int32_t AJM_ERROR_INVALID_PARAMETER        = static_cast<int32_t>(0x80930005u);
constexpr int32_t AJM_ERROR_OUT_OF_MEMORY            = static_cast<int32_t>(0x80930006u);
constexpr int32_t AJM_ERROR_OUT_OF_RESOURCES         = static_cast<int32_t>(0x80930007u);
constexpr int32_t AJM_ERROR_CODEC_NOT_SUPPORTED      = static_cast<int32_t>(0x80930008u);
constexpr int32_t AJM_ERROR_CODEC_ALREADY_REGISTERED = static_cast<int32_t>(0x80930009u);
constexpr int32_t AJM_ERROR_CODEC_NOT_REGISTERED     = static_cast<int32_t>(0x8093000au);
constexpr int32_t AJM_ERROR_WRONG_REVISION_FLAG      = static_cast<int32_t>(0x8093000bu);
constexpr int32_t AJM_ERROR_FLAG_NOT_SUPPORTED       = static_cast<int32_t>(0x8093000cu);
constexpr int32_t AJM_ERROR_BUSY                     = static_cast<int32_t>(0x8093000du);
constexpr int32_t AJM_ERROR_BAD_PRIORITY             = static_cast<int32_t>(0x8093000eu);
constexpr int32_t AJM_ERROR_IN_PROGRESS              = static_cast<int32_t>(0x8093000fu);
constexpr int32_t AJM_ERROR_RETRY                    = static_cast<int32_t>(0x80930010u);
constexpr int32_t AJM_ERROR_MALFORMED_BATCH          = static_cast<int32_t>(0x80930011u);
constexpr int32_t AJM_ERROR_JOB_CREATION             = static_cast<int32_t>(0x80930012u);
constexpr int32_t AJM_ERROR_INVALID_OPCODE           = static_cast<int32_t>(0x80930013u);
constexpr int32_t AJM_ERROR_PRIORITY_VIOLATION       = static_cast<int32_t>(0x80930014u);
constexpr int32_t AJM_ERROR_BUFFER_TOO_BIG           = static_cast<int32_t>(0x80930015u);
constexpr int32_t AJM_ERROR_INVALID_ADDRESS          = static_cast<int32_t>(0x80930016u);
constexpr int32_t AJM_ERROR_CANCELLED                = static_cast<int32_t>(0x80930017u);

constexpr int32_t AJM_JOB_NOT_INITIALIZED    = 0x00000001;
constexpr int32_t AJM_JOB_INVALID_DATA       = 0x00000002;
constexpr int32_t AJM_JOB_INVALID_PARAMETER  = 0x00000004;
constexpr int32_t AJM_JOB_PARTIAL_INPUT      = 0x00000008;
constexpr int32_t AJM_JOB_NOT_ENOUGH_ROOM    = 0x00000010;
constexpr int32_t AJM_JOB_STREAM_CHANGE      = 0x00000020;
constexpr int32_t AJM_JOB_TOO_MANY_CHANNELS  = 0x00000040;
constexpr int32_t AJM_JOB_UNSUPPORTED_FLAG   = 0x00000080;
constexpr int32_t AJM_JOB_SIDEBAND_TRUNCATED = 0x00000100;
constexpr int32_t AJM_JOB_PRIORITY_PASSED    = 0x00000200;
constexpr int32_t AJM_JOB_CODEC_ERROR        = 0x40000000;
constexpr int32_t AJM_JOB_FATAL              = static_cast<int32_t>(0x80000000u);

constexpr uint32_t AJM_CODEC_MP3 = 0;
constexpr uint32_t AJM_CODEC_AT9 = 1;
constexpr uint32_t AJM_CODEC_AAC = 2;

constexpr uint64_t AJM_FLAG_VERSION_MASK          = 0x0000000000000007ull;
constexpr uint64_t AJM_FLAG_RUN_GET_CODEC_INFO    = 0x0000000000000800ull;
constexpr uint64_t AJM_FLAG_RUN_MULTIPLE_FRAMES   = 0x0000000000001000ull;
constexpr uint64_t AJM_FLAG_CONTROL_RESET         = 0x0000000000002000ull;
constexpr uint64_t AJM_FLAG_CONTROL_INITIALIZE    = 0x0000000000004000ull;
constexpr uint64_t AJM_FLAG_CONTROL_RESAMPLE      = 0x0000000000008000ull;
constexpr uint64_t AJM_FLAG_SIDEBAND_GAPLESS      = 0x0000200000000000ull;
constexpr uint64_t AJM_FLAG_SIDEBAND_FORMAT       = 0x0000400000000000ull;
constexpr uint64_t AJM_FLAG_SIDEBAND_STREAM       = 0x0000800000000000ull;

struct BatchError
{
	int32_t     error_code = AJM_OK;
	uint32_t    reserved0  = 0;
	const void* job_addr   = nullptr;
	uint32_t    cmd_offset = 0;
	uint32_t    reserved1  = 0;
	const void* job_ra     = nullptr;
};

struct Result
{
	int32_t result          = AJM_OK;
	int32_t internal_result = AJM_OK;
};

struct Stream
{
	int32_t  input_consumed       = 0;
	int32_t  output_written       = 0;
	uint64_t total_decoded_samples = 0;
};

struct MultipleFrame
{
	uint32_t num_frames = 0;
	uint32_t reserved   = 0;
};

struct At9Info
{
	uint32_t superframe_size      = 0;
	uint32_t frames_in_superframe = 0;
	uint32_t next_frame_size      = 0;
	uint32_t frame_samples        = 0;
};

struct Format
{
	uint32_t channels    = 0;
	uint32_t channel_mask = 0;
	uint32_t sample_rate = 0;
	uint32_t encoding    = 0;
	uint32_t bitrate     = 0;
	uint32_t reserved    = 0;
};

struct Gapless
{
	uint32_t total_samples  = 0;
	uint16_t skip_samples   = 0;
	uint16_t skipped_samples = 0;
};

struct At9CodecInfo
{
	uint32_t channels             = 0;
	uint32_t channel_config_index = 0;
	uint32_t sampling_rate        = 0;
	uint32_t superframe_size      = 0;
	uint32_t frames_in_superframe = 0;
	uint32_t frame_samples        = 0;
	uint32_t window_length        = 0;
	std::array<uint8_t, 4> config {};
};

class At9Decoder
{
public:
	virtual ~At9Decoder() = default;

	virtual int32_t Initialize(const std::array<uint8_t, 4>& config, At9CodecInfo* info) = 0;
	virtual int32_t Reset()                                                               = 0;
	virtual int32_t DecodeFrame(const uint8_t* input, size_t input_size, void* pcm, size_t pcm_size,
	                            uint32_t format, bool non_interleaved, uint32_t* bytes_used) = 0;
};

class At9DecoderFactory
{
public:
	virtual ~At9DecoderFactory() = default;

	virtual bool Available() const = 0;
	virtual std::unique_ptr<At9Decoder> Create(uint32_t channels, uint32_t format, uint32_t codec_flags) = 0;
};

class Runtime
{
public:
	explicit Runtime(At9DecoderFactory* factory = nullptr);
	~Runtime();

	Runtime(const Runtime&)            = delete;
	Runtime& operator=(const Runtime&) = delete;
	Runtime(Runtime&&)                 = delete;
	Runtime& operator=(Runtime&&)      = delete;

	int32_t Initialize(int64_t reserved, uint32_t* context);
	int32_t Finalize(uint32_t context);
	int32_t ModuleRegister(uint32_t context, uint32_t codec, int64_t reserved);
	int32_t ModuleUnregister(uint32_t context, uint32_t codec);
	int32_t InstanceCreate(uint32_t context, uint32_t codec, uint64_t flags, uint32_t* instance_id);
	int32_t InstanceDestroy(uint32_t context, uint32_t instance_id);
	int32_t BatchStart(uint32_t context, uint8_t* batch, uint32_t batch_size, int32_t priority, BatchError* batch_error,
	                   uint32_t* batch_id);
	int32_t BatchWait(uint32_t context, uint32_t batch_id, uint32_t timeout, BatchError* batch_error);

	// Drops every guest-owned context, batch and decoder while preserving the
	// process-owned factory. The caller must have stopped all guest execution.
	[[nodiscard]] bool ResetForNextSession();

private:
	class Impl;
	std::unique_ptr<Impl> m_impl;
};

void* BatchJobControlBufferRa(void* batch, uint32_t instance_id, uint64_t flags, const void* sideband_input,
                              size_t sideband_input_size, void* sideband_output, size_t sideband_output_size,
                              const void* return_address);
void* BatchJobRunBufferRa(void* batch, uint32_t instance_id, uint64_t flags, const void* input, size_t input_size, void* output,
                          size_t output_size, void* sideband_output, size_t sideband_output_size, const void* return_address);

void BatchErrorDump(const void* batch, const BatchError* error);

At9DecoderFactory& DefaultAt9DecoderFactory();
Runtime&            GlobalRuntime();
[[nodiscard]] bool  ResetForNextSession();

#if defined(LUMEN_RUNTIME_LIFECYCLE_TEST_HOOK)
[[nodiscard]] bool SessionResetContractTest();
#endif

static_assert(sizeof(BatchError) == 0x20);
static_assert(sizeof(Result) == 0x08);
static_assert(sizeof(Stream) == 0x10);
static_assert(sizeof(MultipleFrame) == 0x08);
static_assert(sizeof(At9Info) == 0x10);
static_assert(sizeof(Format) == 0x18);
static_assert(sizeof(Gapless) == 0x08);

} // namespace Lumen::Libs::Ajm

#endif // EMULATOR_SRC_LIBS_AJM_CORE_H_
