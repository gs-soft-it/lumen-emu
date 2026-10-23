#ifndef KYTY_COMMON_LOADINGTRACE_H_
#define KYTY_COMMON_LOADINGTRACE_H_

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace Common::LoadingTrace {

inline bool Enabled() noexcept {
	static const bool enabled = [] {
		const auto* value = std::getenv("LUMEN_PS5_LOADING_TRACE");
		return value != nullptr && std::strcmp(value, "1") == 0;
	}();
	return enabled;
}

namespace Detail {
struct State {
	std::once_flag initialize;
	std::atomic<int64_t> origin_ns {0};
	std::atomic<bool> first_guest_present {false};
};

inline State& GetState() noexcept {
	static State state;
	return state;
}

inline int64_t NowNs() noexcept {
	return std::chrono::duration_cast<std::chrono::nanoseconds>(
	           std::chrono::steady_clock::now().time_since_epoch()).count();
}
} // namespace Detail

// One process-wide origin, initialized at Run entry before startup work. The
// disabled path never reads a clock or initializes the diagnostic state.
inline void Initialize() {
	if (!Enabled()) return;
	auto& state = Detail::GetState();
	std::call_once(state.initialize, [&state] {
		const auto origin = Detail::NowNs();
		state.origin_ns.store(origin, std::memory_order_release);
		std::fprintf(stderr,
		             "PS5_LOADING_TRACE_V1 enabled=1 origin_ns=%" PRId64
		             " clock=steady unit=ns menu_ready=unmeasured\n", origin);
	});
}

inline void Milestone(const char* name) noexcept {
	if (!Enabled()) return;
	const auto origin = Detail::GetState().origin_ns.load(std::memory_order_acquire);
	if (origin == 0) return;
	const auto now = Detail::NowNs();
	std::fprintf(stderr,
	             "PS5_LOADING_MILESTONE_V1 milestone=%s timestamp_ns=%" PRId64
	             " elapsed_ms=%.6f\n", name, now,
	             static_cast<double>(now - origin) / 1000000.0);
}

class Phase final {
public:
	explicit Phase(const char* name) noexcept: m_name(name) {
		if (!Enabled()) return;
		m_origin = Detail::GetState().origin_ns.load(std::memory_order_acquire);
		if (m_origin == 0) return;
		m_begin = Detail::NowNs();
		m_active = true;
	}

	~Phase() { Finish(); }
	Phase(const Phase&) = delete;
	Phase& operator=(const Phase&) = delete;

	void Finish() noexcept {
		if (!m_active) return;
		const auto end = Detail::NowNs();
		m_active = false;
		std::fprintf(stderr,
		             "PS5_LOADING_PHASE_V1 phase=%s start_ns=%" PRId64
		             " end_ns=%" PRId64 " elapsed_ms=%.6f duration_ms=%.6f\n",
		             m_name, m_begin, end, static_cast<double>(end - m_origin) / 1000000.0,
		             static_cast<double>(end - m_begin) / 1000000.0);
	}

private:
	const char* m_name;
	int64_t m_origin = 0;
	int64_t m_begin = 0;
	bool m_active = false;
};

inline void FirstGuestPresent() noexcept {
	if (!Enabled()) return;
	auto& state = Detail::GetState();
	if (state.first_guest_present.load(std::memory_order_relaxed)) return;
	const auto origin = state.origin_ns.load(std::memory_order_acquire);
	if (origin == 0 || state.first_guest_present.exchange(true, std::memory_order_relaxed)) return;
	const auto now = Detail::NowNs();
	// An accepted guest submission can still contain black or unchanged pixels.
	std::fprintf(stderr,
	             "PS5_LOADING_MILESTONE_V1 milestone=first_guest_present timestamp_ns=%" PRId64
	             " elapsed_ms=%.6f guest_metric=accepted_submission "
	             "pixel_unique=unmeasured menu_ready=unmeasured\n",
	             now, static_cast<double>(now - origin) / 1000000.0);
}

} // namespace Common::LoadingTrace

#endif // KYTY_COMMON_LOADINGTRACE_H_
