#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADERSOURCERETRY_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_SHADERSOURCERETRY_H_

#include <chrono>
#include <cstdint>

namespace Lumen::Libs::Graphics {

struct ShaderSourcePreflightResult
{
	bool captured = false;
	const char* reason = nullptr;
	uint32_t attempts = 0;
	uint64_t elapsed_ms = 0;
};

inline const char* ShaderSourceRetryContextRejection(bool memory_lease, bool run_owned, bool parser_owned, bool recording_valid)
{
	if (memory_lease) return "source-memory-lease-held";
	if (!run_owned) return "source-run-ownership-missing";
	if (!parser_owned) return "source-parser-ownership-missing";
	if (!recording_valid) return "source-recording-invalid";
	return nullptr;
}

// Only a typed, transient admission refusal may leave this pure preflight.
// The caller releases its processor lock in pause(), before any renderer/GPU
// effects, and revalidates the original recording and native state afterwards.
// Capture must release every guest-data lease before returning false.
template <typename Capture, typename CanPause, typename Pause, typename Revalidate>
ShaderSourcePreflightResult CaptureShaderSourceWithRetry(Capture&& capture, CanPause&& can_pause,
	Pause&& pause, Revalidate&& revalidate, uint32_t max_attempts = 4096,
	std::chrono::milliseconds timeout = std::chrono::seconds(3))
{
	// FEX's mutation deadline is 2 seconds. One extra second allows the owner
	// to finish cancellation/readmission and the worker to be scheduled again.
	using Clock = std::chrono::steady_clock;
	const auto started = Clock::now();
	const auto finish = [&](bool captured, const char* reason, uint32_t attempts) {
		return ShaderSourcePreflightResult {captured, reason, attempts,
		    static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count())};
	};
	for (uint32_t index = 0; index < max_attempts; ++index)
	{
		const uint32_t attempt = index + 1;
		if (const char* reason = can_pause()) return finish(false, reason, index);
		bool retryable = false;
		if (capture(&retryable)) return finish(true, nullptr, attempt);
		if (!retryable) return finish(false, "source-access-refused", attempt);
		if (const char* reason = can_pause()) return finish(false, reason, attempt);
		if (attempt == max_attempts || Clock::now() - started >= timeout)
			return finish(false, "source-admission-timeout", attempt);
		pause();
		if (const char* reason = revalidate()) return finish(false, reason, attempt);
	}
	return finish(false, "source-attempt-budget", 0);
}

} // namespace Lumen::Libs::Graphics
#endif
