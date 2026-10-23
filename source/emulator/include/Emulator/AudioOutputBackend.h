#ifndef EMULATOR_INCLUDE_EMULATOR_AUDIOOUTPUTBACKEND_H_
#define EMULATOR_INCLUDE_EMULATOR_AUDIOOUTPUTBACKEND_H_

#include "Emulator/Common.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#if defined(LUMEN_AUDIO_OUT_TEST_HOOK)
#include <vector>
#endif

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs::Audio::Host {

enum class SampleFormat
{
	Signed16,
	Float32,
};

struct StreamSpec
{
	uint32_t     sample_rate      = 0;
	uint32_t     frames_per_block = 0;
	uint32_t     channels         = 0;
	SampleFormat sample_format    = SampleFormat::Signed16;
	bool         standard_8ch     = false;
};

[[nodiscard]] size_t InputBlockBytes(const StreamSpec& spec);

[[nodiscard]] bool ConvertBlockToFloat(const StreamSpec& spec, const void* input, const int canonical_volumes[8], float* output,
                                       size_t output_samples);

[[nodiscard]] bool DownmixBlockToStereo(const StreamSpec& spec, const float* canonical_input, size_t input_samples, float* output,
                                        size_t output_samples);

struct PacingState
{
	uint64_t deadline_us    = 0;
	uint64_t remainder      = 0;
	uint32_t sample_rate    = 0;
	bool     refill_pending = false;
};

[[nodiscard]] uint64_t AdvancePacingDeadline(uint64_t previous_deadline_us, uint64_t now_us, uint64_t period_us);
[[nodiscard]] uint64_t AdvancePacingDeadline(PacingState* state, uint64_t now_us, uint32_t frames, uint32_t sample_rate);
[[nodiscard]] size_t   BufferedQueueTargetFrames(uint32_t frames_per_block, uint32_t sample_rate);
[[nodiscard]] bool     ShouldRefillBufferedBatch(const size_t* queued_frames, const size_t* target_frames,
                                                 size_t submitted_count, size_t total_count);
// Apply queue recovery after one nominal deadline advance, without advancing it again.
[[nodiscard]] uint64_t ApplyBufferedPacingDeadline(PacingState* state, uint64_t now_us, uint32_t frames,
                                                   uint32_t sample_rate, size_t queued_frames, bool allow_refill);
[[nodiscard]] uint64_t AdvanceBufferedPacingDeadline(PacingState* state, uint64_t now_us, uint32_t frames,
                                                     uint32_t sample_rate, size_t queued_frames, bool allow_refill);

namespace Internal {

struct RingEnqueuePlan
{
	bool   valid              = false;
	size_t read_index         = 0;
	size_t retained_samples   = 0;
	size_t write_index        = 0;
	size_t first_copy_samples = 0;
	size_t queued_samples     = 0;
	size_t dropped_samples    = 0;
};

[[nodiscard]] RingEnqueuePlan PlanRingEnqueue(size_t capacity_samples, size_t read_index, size_t queued_samples,
                                              size_t incoming_samples, size_t sample_alignment);

[[nodiscard]] size_t StereoRingCapacitySamples(size_t frames_per_block, size_t minimum_queued_frames);

} // namespace Internal

struct StreamStats
{
	uint64_t submitted_blocks       = 0;
	uint64_t inaudible_blocks       = 0;
	uint64_t exact_silent_blocks    = 0;
	uint64_t input_bytes            = 0;
	uint64_t output_bytes           = 0;
	uint64_t consumed_output_bytes  = 0;
	uint64_t queued_output_bytes    = 0;
	uint64_t callback_calls         = 0;
	uint64_t callback_frames        = 0;
	uint64_t audible_callback_calls = 0;
	uint64_t underrun_frames        = 0;
	uint64_t queue_resets           = 0;
	uint64_t queue_overflow_events  = 0;
	uint64_t dropped_overrun_frames = 0;
	uint64_t submit_errors          = 0;
	uint64_t capacity_wait_events   = 0;
	uint64_t capacity_wait_us       = 0;
	uint64_t capacity_wait_timeouts = 0;
	uint64_t capacity_wait_cancels  = 0;
	float    max_submitted_peak     = 0.0f;
	float    max_callback_peak      = 0.0f;
};

class OutputStream
{
public:
	OutputStream()          = default;
	virtual ~OutputStream() = default;

	OutputStream(const OutputStream&)            = delete;
	OutputStream& operator=(const OutputStream&) = delete;

	[[nodiscard]] virtual bool        Submit(const void* data, const int canonical_volumes[8], std::string* error) = 0;
	[[nodiscard]] virtual StreamStats GetStats() const                                                             = 0;
	virtual void CancelPendingSubmissions() = 0;
};

struct OutputBatchItem
{
	OutputStream* stream            = nullptr;
	const void*   data              = nullptr;
	const int*    canonical_volumes = nullptr;
};

class OutputBackend
{
public:
	OutputBackend();
	~OutputBackend();

	OutputBackend(const OutputBackend&)            = delete;
	OutputBackend& operator=(const OutputBackend&) = delete;

	[[nodiscard]] bool                          Initialize(std::string* error);
	[[nodiscard]] bool                          IsInitialized() const;
	[[nodiscard]] std::string                   DriverName() const;
	[[nodiscard]] std::unique_ptr<OutputStream> Open(const StreamSpec& spec, std::string* error);
	[[nodiscard]] bool SubmitBatch(const OutputBatchItem* items, size_t count, std::string* error);
	[[nodiscard]] bool SubmitBlockingBatch(const OutputBatchItem* items, size_t count, std::string* error);
#if defined(LUMEN_AUDIO_OUT_TEST_HOOK)
	[[nodiscard]] bool TestSetManualCallbacks(bool manual);
	[[nodiscard]] bool TestConsumeFrames(size_t frames, std::vector<float>* output);
	[[nodiscard]] bool TestShutdownMixer();
#endif

private:
	[[nodiscard]] bool SubmitBatchWithPolicy(const OutputBatchItem* items, size_t count, bool wait_for_capacity,
	                                         std::string* error);
	class Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace Lumen::Libs::Audio::Host

#endif // LUMEN_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_AUDIOOUTPUTBACKEND_H_ */
