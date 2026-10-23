#include "Emulator/Controller.h"

#include "Lumen/Core/Common.h"
#include "Lumen/Core/DbgAssert.h"
#include "Lumen/Core/String.h"
#include "Lumen/Core/Threads.h"
#include "Lumen/Core/Vector.h"

#include "Emulator/Kernel/Pthread.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"

#include "ControllerDigitalHistory.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs::Controller {

LIB_NAME("Pad", "Pad");

bool ControllerOptionsTraceEnabled()
{
	static const bool enabled = [] {
		const char* value = std::getenv("LUMEN_PAD_OPTIONS_TRACE");
		return value != nullptr && value[0] == '1' && value[1] == '\0';
	}();
	return enabled;
}

static void InputTraceV(std::atomic<uint32_t>& records, uint32_t limit, const char* marker, const char* format, va_list args)
{
	uint32_t index = records.load(std::memory_order_relaxed);
	while (index < limit && !records.compare_exchange_weak(index, index + 1, std::memory_order_relaxed)) {}
	if (index >= limit) return;
	char message[768];
	std::vsnprintf(message, sizeof(message), format, args);
	std::fprintf(stderr, "[%s] record=%u %s\n", marker, index + 1, message);
}

static void OptionsTrace(const char* format, ...)
{
	if (!ControllerOptionsTraceEnabled()) return;
	static std::atomic<uint32_t> records {0};
	va_list args;
	va_start(args, format);
	InputTraceV(records, 256, "PAD_OPTIONS_TRACE", format, args);
	va_end(args);
}

bool ControllerTriggerTraceEnabled()
{
	static const bool enabled = [] {
		const char* value = std::getenv("LUMEN_PAD_TRIGGER_TRACE");
		return value != nullptr && value[0] == '1' && value[1] == '\0';
	}();
	return enabled;
}

static void TriggerTrace(const char* format, ...)
{
	if (!ControllerTriggerTraceEnabled()) return;
	static std::atomic<uint32_t> records {0};
	va_list args;
	va_start(args, format);
	InputTraceV(records, 512, "PAD_TRIGGER_TRACE", format, args);
	va_end(args);
}

void ControllerTraceTriggerEvent(int id, int axis, int value, uint32_t event_ticks, uint32_t current_ticks)
{
	TriggerTrace("phase=sdl-axis id=%d axis=%d raw=%d event_ticks=%u dispatch_ticks=%u queue_age_ms=%u",
	             id, axis, value, event_ticks, current_ticks, current_ticks - event_ticks);
}

void ControllerTraceOptionsEvent(int id, bool down, uint32_t event_ticks, uint32_t current_ticks,
                                 const char* name, const char* path, const char* guid)
{
	OptionsTrace("phase=sdl id=%d down=%d event_ticks=%u dispatch_ticks=%u queue_age_ms=%u name=\"%s\" path=\"%s\" guid=%s",
	             id, down, event_ticks, current_ticks, current_ticks - event_ticks,
	             name != nullptr ? name : "unknown", path != nullptr ? path : "unavailable", guid != nullptr ? guid : "unknown");
}

struct PadData
{
	uint32_t buttons;
	uint8_t  left_stick_x;
	uint8_t  left_stick_y;
	uint8_t  right_stick_x;
	uint8_t  right_stick_y;
	uint8_t  analog_buttons_l2;
	uint8_t  analog_buttons_r2;
	uint8_t  padding[2];
	float    orientation_x;
	float    orientation_y;
	float    orientation_z;
	float    orientation_w;
	float    acceleration_x;
	float    acceleration_y;
	float    acceleration_z;
	float    angular_velocity_x;
	float    angular_velocity_y;
	float    angular_velocity_z;
	uint8_t  touch_data_touch_num;
	uint8_t  touch_data_reserve[3];
	uint32_t touch_data_reserve1;
	uint16_t touch_data_touch0_x;
	uint16_t touch_data_touch0_y;
	uint8_t  touch_data_touch0_id;
	uint8_t  touch_data_touch0_reserve[3];
	uint16_t touch_data_touch1_x;
	uint16_t touch_data_touch1_y;
	uint8_t  touch_data_touch1_id;
	uint8_t  touch_data_touch1_reserve[3];
	bool     connected;
	uint64_t timestamp;
	uint32_t extension_unit_data_extension_unit_id;
	uint8_t  extension_unit_data_reserve[1];
	uint8_t  extension_unit_data_data_length;
	uint8_t  extension_unit_data_data[10];
	uint8_t  connected_count;
	uint8_t  reserve[2];
	uint8_t  device_unique_data_len;
	uint8_t  device_unique_data[12];
};

// The console stores exactly this window on both modeled ports for the read
// family (oracle LMNQ92814); the modeled declaration must match it.
static_assert(sizeof(PadData) == PAD_DATA_RECORD_SIZE, "the measured read window must match the modeled record");

struct PadControllerInformation
{
	float    touch_pixel_density;
	uint16_t touch_resolution_x;
	uint16_t touch_resolution_y;
	uint8_t  stick_dead_zone_left;
	uint8_t  stick_dead_zone_right;
	uint8_t  connection_type;
	uint8_t  connected_count;
	bool     connected;
	int      device_class;
};

// Local model of the extended information filled by
// scePadDeviceClassGetExtendedInformation, mirroring the 20-byte write window
// measured on the console (oracle LMNQ92802): 32-bit device class, 4 reserved
// bytes and 12 bytes of class data. The console stores its default record
// (class 0, zeroed area) for every call and only selects the class by the
// handle; the local model does the same and the guest window is bounded by
// this declaration.
struct PadDeviceClassExtendedInformation
{
	int     device_class;
	uint8_t reserved[4];
	uint8_t class_data[12];
};
static_assert(sizeof(PadDeviceClassExtendedInformation) == PAD_DEVICE_CLASS_RECORD_SIZE);

struct PadVibrationParam
{
	uint8_t large_motor;
	uint8_t small_motor;
};

struct ControllerState
{
	uint64_t time                                  = 0;
	uint32_t buttons                               = 0;
	int      axes[static_cast<int>(Axis::AxisMax)] = {128, 128, 128, 128, 0, 0};
};

class GameController
{
public:
	GameController()          = default;
	virtual ~GameController() = default;

	LUMEN_CLASS_NO_COPY(GameController);

	void Connect(int id);
	void EnsureVirtualConnected();
	void ResetForNextSession();
	void Disconnect(int id);
	void Button(int id, uint32_t button, bool down);
	void Axis(int id, Axis axis, int value);
	void SetLeftStick(int id, int lx, int ly);
	void GetConnectionInfo(bool* flag, int* count);
	void ReadState(ControllerState* state, bool* flag, int* count);
	int  ReadStates(ControllerState* states, int states_num, bool* flag, int* count);

private:
	static constexpr uint32_t STATES_MAX = 64;

	struct StatePrivate
	{
		bool     obtained        = false;
		uint64_t digital_sequence = 0;
	};

	void                          ConnectLocked(int id);
	void                          CheckActive();
	[[nodiscard]] ControllerState GetLastState() const;
	void                          AddState(const ControllerState& state);
	void                          TraceOptionsRead(const ControllerState* states, int returned, int requested, bool buffered);
	void                          TraceTriggerRead(const ControllerState* states, int returned, int requested, bool buffered);

	Core::Mutex     m_mutex;
	Vector<int>     m_connected_ids;
	int             m_active_id       = -1;
	bool            m_connected       = false;
	int             m_connected_count = 0;
	ControllerState m_states[STATES_MAX];
	StatePrivate    m_private[STATES_MAX];
	ControllerState m_last_state;
	Detail::DigitalButtonHistory m_digital_history;
	uint32_t                     m_states_num  = 0;
	uint32_t                     m_first_state = 0;
	uint8_t                      m_options_trace_reads[2] {};
	bool                         m_options_trace_down[2] {};
	uint8_t                      m_trigger_trace_reads[2] {};
	uint32_t                     m_trigger_trace_mask[2] {};
	uint32_t                     m_trigger_trace_empty_reads[2] {};
	bool                         m_buffered_cursor_valid = false;
	uint64_t                     m_buffered_cursor_sequence = 0;
	uint32_t                     m_buffered_cursor_buttons = 0;
};

static GameController* g_controller = nullptr;

LUMEN_SUBSYSTEM_INIT(Controller)
{
	EXIT_IF(g_controller != nullptr);

	g_controller = new GameController;
}

LUMEN_SUBSYSTEM_UNEXPECTED_SHUTDOWN(Controller) {}

LUMEN_SUBSYSTEM_DESTROY(Controller) {}

void GameController::Connect(int id)
{
	Core::LockGuard lock(m_mutex);

	EXIT_IF(m_connected_ids.Contains(id));
	ConnectLocked(id);
}

void GameController::EnsureVirtualConnected()
{
	Core::LockGuard lock(m_mutex);

	if (m_connected_ids.Contains(VIRTUAL_CONTROLLER_ID))
	{
		return;
	}

	printf("[Controller] Connecting virtual input fallback\n");
	ConnectLocked(VIRTUAL_CONTROLLER_ID);
}

void GameController::ResetForNextSession()
{
	Core::LockGuard lock(m_mutex);

	// Controller instances and SDL handles are process-lifetime on iOS. Keep
	// their ordering/connection counters, but make the next guest session start
	// from one neutral snapshot with no unread digital history from the previous
	// title. In particular, a short touch released during teardown must not be
	// replayed by the next scePadRead call.
	m_states_num  = 0;
	m_first_state = 0;
	m_last_state  = ControllerState();
	for (StatePrivate& state : m_private)
	{
		state = StatePrivate();
	}
	m_digital_history.Reset(m_last_state.buttons);
	for (auto& reads : m_options_trace_reads) reads = 0;
	for (auto& down : m_options_trace_down) down = false;
	for (auto& reads : m_trigger_trace_reads) reads = 0;
	for (auto& mask : m_trigger_trace_mask) mask = 0;
	for (auto& reads : m_trigger_trace_empty_reads) reads = 0;
	m_buffered_cursor_valid = false;
	m_buffered_cursor_sequence = 0;
	m_buffered_cursor_buttons = 0;
}

void GameController::ConnectLocked(int id)
{

	// Keep every physical controller before the virtual input fallback.
	// This also preserves the priority after the first physical controller
	// disconnects while another one is still connected.
	const uint32_t virtual_index = m_connected_ids.Find(VIRTUAL_CONTROLLER_ID);
	if (id != VIRTUAL_CONTROLLER_ID && m_connected_ids.IndexValid(virtual_index))
	{
		printf("[Controller] Physical controller %d connected - taking priority over virtual input\n", id);
		m_connected_ids.InsertAt(virtual_index, id);
	}
	else
	{
		printf("[Controller] Controller %d connected%s\n", id, (id == VIRTUAL_CONTROLLER_ID ? " (virtual input)" : ""));
		m_connected_ids.Add(id);
	}

	CheckActive();
}

void GameController::Disconnect(int id)
{
	Core::LockGuard lock(m_mutex);

	if (!m_connected_ids.Contains(id))
	{
		printf("[Controller] WARNING: Disconnect called for unknown controller %d, ignoring\n", id);
		return;
	}

	printf("[Controller] Controller %d disconnected\n", id);
	m_connected_ids.Remove(id);

	CheckActive();
}

void GameController::CheckActive()
{
	bool reset = false;

	if (m_connected)
	{
		if (m_connected_ids.IsEmpty())
		{
			m_active_id = -1;
			m_connected = false;
			reset       = true;
		} else
		{
			if (m_connected_ids.At(0) != m_active_id)
			{
				m_active_id = m_connected_ids.At(0);
				m_connected_count++;
				reset = true;
			}
		}
	} else
	{
		if (!m_connected_ids.IsEmpty())
		{
			m_active_id = m_connected_ids.At(0);
			m_connected = true;
			m_connected_count++;
			reset = true;
		}
	}

	if (reset)
	{
		m_states_num = 0;
		m_last_state = ControllerState();
		m_digital_history.Reset(m_last_state.buttons);
		m_buffered_cursor_valid = false;
		m_buffered_cursor_sequence = 0;
		m_buffered_cursor_buttons = 0;
	}
}

ControllerState GameController::GetLastState() const
{
	if (m_states_num == 0)
	{
		return m_last_state;
	}

	auto last = (m_first_state + m_states_num - 1) % STATES_MAX;

	return m_states[last];
}

void GameController::AddState(const ControllerState& state)
{
	if (m_states_num != 0)
	{
		const auto last = (m_first_state + m_states_num - 1) % STATES_MAX;
		// Keep the freshest analog snapshot until PadRead delivers it. Replaying
		// every intermediate position can leave a slow reader behind the stick,
		// even after it returns to center. Each unread digital transition keeps
		// its own sample and sequence.
		if (!m_private[last].obtained && m_states[last].buttons == state.buttons)
		{
			m_states[last] = state;
			m_last_state   = state;
			return;
		}
		// A pending analog update may outlive a release when PadRead asks for
		// one record. Replace it only if this exact digital generation and mask
		// were already delivered by PadRead; an unread press or repeated tap has
		// another sequence and must keep its own sample. ReadState is independent.
		if (!m_private[last].obtained && m_buffered_cursor_valid &&
		    m_private[last].digital_sequence == m_buffered_cursor_sequence &&
		    m_states[last].buttons == m_buffered_cursor_buttons)
		{
			if (((m_states[last].buttons | state.buttons) & (PAD_BUTTON_L2 | PAD_BUTTON_R2)) != 0)
			{
				TriggerTrace("phase=replace-obsolete-held old_mask=0x%08x new_mask=0x%08x delivered_sequence=%llu",
				             m_states[last].buttons, state.buttons, static_cast<unsigned long long>(m_buffered_cursor_sequence));
			}
			(void)m_digital_history.Record(state.buttons, state.time);
			m_states[last] = state;
			m_last_state = state;
			m_private[last].digital_sequence = m_digital_history.Sequence();
			return;
		}
	}

	if (m_states_num >= STATES_MAX)
	{
		m_states_num  = STATES_MAX - 1;
		m_first_state = (m_first_state + 1) % STATES_MAX;
	}

	auto index = (m_first_state + m_states_num) % STATES_MAX;

	m_states[index] = state;
	m_last_state    = state;

	(void)m_digital_history.Record(state.buttons, state.time);
	m_private[index].obtained         = false;
	m_private[index].digital_sequence = m_digital_history.Sequence();

	m_states_num++;
}

void GameController::Button(int id, uint32_t button, bool down)
{
	Core::LockGuard lock(m_mutex);
	if ((button & PAD_BUTTON_OPTIONS) != 0)
	{
		OptionsTrace("phase=pad-input id=%d active=%d down=%d accepted=%d duplicate=%d sequence=%llu pending=%zu",
		             id, m_active_id, down, m_active_id == id, ((GetLastState().buttons & button) != 0) == down,
		             static_cast<unsigned long long>(m_digital_history.Sequence()), m_digital_history.Size());
	}

	if (m_active_id == id)
	{
		auto state = GetLastState();
		if (((state.buttons & button) != 0) == down)
		{
			return;
		}

		state.time = LibKernel::KernelGetProcessTime();

		if (down)
		{
			state.buttons |= button;
		} else
		{
			state.buttons &= ~button;
		}

		AddState(state);
	}
}

void GameController::Axis(int id, Controller::Axis axis, int value)
{
	Core::LockGuard lock(m_mutex);

	if (m_active_id == id)
	{
		auto state = GetLastState();
		int axis_id = static_cast<int>(axis);

		EXIT_IF(axis_id < 0 || axis_id >= static_cast<int>(Controller::Axis::AxisMax));
		const uint32_t trigger = axis == Controller::Axis::TriggerLeft ? PAD_BUTTON_L2 :
		                         axis == Controller::Axis::TriggerRight ? PAD_BUTTON_R2 : 0;
		const bool unchanged = state.axes[axis_id] == value &&
		                       (trigger == 0 || ((state.buttons & trigger) != 0) == (value > 0));
		if (trigger != 0)
		{
			TriggerTrace("phase=pad-axis id=%d active=%d axis=%d value=%d previous=%d no_op=%d physical_mask=0x%08x sequence=%llu pending=%zu",
			             id, m_active_id, axis_id, value, state.axes[axis_id], unchanged, state.buttons,
			             static_cast<unsigned long long>(m_digital_history.Sequence()), m_digital_history.Size());
		}
		if (unchanged) return;
		state.time = LibKernel::KernelGetProcessTime();

		state.axes[axis_id] = value;

		if (axis == Controller::Axis::TriggerLeft)
		{
			if (value > 0)
			{
				state.buttons |= PAD_BUTTON_L2;
			} else
			{
				state.buttons &= ~PAD_BUTTON_L2;
			}
		}

		if (axis == Controller::Axis::TriggerRight)
		{
			if (value > 0)
			{
				state.buttons |= PAD_BUTTON_R2;
			} else
			{
				state.buttons &= ~PAD_BUTTON_R2;
			}
		}

		AddState(state);
	}
}

void GameController::SetLeftStick(int id, int lx, int ly)
{
	Core::LockGuard lock(m_mutex);

	if (m_active_id == id)
	{
		auto state = GetLastState();

		// Skip if values haven't changed (avoids flooding the ring buffer)
		if (state.axes[static_cast<int>(Controller::Axis::LeftX)] == lx &&
		    state.axes[static_cast<int>(Controller::Axis::LeftY)] == ly)
		{
			return;
		}

		state.time = LibKernel::KernelGetProcessTime();
		state.axes[static_cast<int>(Controller::Axis::LeftX)] = lx;
		state.axes[static_cast<int>(Controller::Axis::LeftY)] = ly;

		AddState(state);
	}
}

void GameController::GetConnectionInfo(bool* flag, int* count)
{
	EXIT_IF(flag == nullptr);
	EXIT_IF(count == nullptr);

	Core::LockGuard lock(m_mutex);

	*flag  = m_connected;
	*count = m_connected_count;
}

void GameController::ReadState(ControllerState* state, bool* flag, int* count)
{
	EXIT_IF(flag == nullptr);
	EXIT_IF(count == nullptr);
	EXIT_IF(state == nullptr);

	Core::LockGuard lock(m_mutex);

	*flag  = m_connected;
	*count = m_connected_count;
	*state = GetLastState();

	if (m_digital_history.TakeOverflow())
	{
		printf("LUMEN_PAD_DIGITAL_HISTORY phase=overflow-resync buttons=0x%08x\n", state->buttons);
		std::fflush(stdout);
	} else
	{
		Detail::DigitalButtonHistory::Sample sample;
		if (m_digital_history.Pop(&sample))
		{
			state->buttons = sample.buttons;
			state->time    = sample.time;
			printf("LUMEN_PAD_DIGITAL_HISTORY phase=latest-read sequence=%llu buttons=0x%08x\n",
			       static_cast<unsigned long long>(sample.sequence), sample.buttons);
			std::fflush(stdout);
		}
	}
	TraceOptionsRead(state, 1, 1, false);
	TraceTriggerRead(state, 1, 1, false);
}

void GameController::TraceTriggerRead(const ControllerState* states, int returned, int requested, bool buffered)
{
	if (!ControllerTriggerTraceEnabled()) return;
	constexpr uint32_t trigger_mask = PAD_BUTTON_L2 | PAD_BUTTON_R2;
	const int channel = buffered ? 1 : 0;
	const char* api = buffered ? "read-buffered" : "read-state";
	if (m_trigger_trace_reads[channel] < 4)
	{
		++m_trigger_trace_reads[channel];
		TriggerTrace("phase=%s requested=%d returned=%d connected=%d", api, requested, returned, m_connected);
	}
	if (returned == 0 && m_trigger_trace_mask[channel] != 0 && m_trigger_trace_empty_reads[channel] != UINT32_MAX)
	{
		++m_trigger_trace_empty_reads[channel];
	}
	for (int index = 0; index < returned; ++index)
	{
		const uint32_t mask = states[index].buttons & trigger_mask;
		if (mask != 0 || m_trigger_trace_mask[channel] != 0)
		{
			const uint64_t now = LibKernel::KernelGetProcessTime();
			TriggerTrace("phase=%s-sample requested=%d returned=%d index=%d mask=0x%08x l2=%d r2=%d sample_us=%llu read_us=%llu age_us=%llu empty_reads=%u physical_mask=0x%08x cursor_sequence=%llu",
			             api, requested, returned, index, mask, states[index].axes[static_cast<int>(Controller::Axis::TriggerLeft)],
			             states[index].axes[static_cast<int>(Controller::Axis::TriggerRight)],
			             static_cast<unsigned long long>(states[index].time), static_cast<unsigned long long>(now),
			             static_cast<unsigned long long>(now >= states[index].time ? now - states[index].time : 0),
			             m_trigger_trace_empty_reads[channel], GetLastState().buttons & trigger_mask,
			             static_cast<unsigned long long>(m_buffered_cursor_sequence));
		}
		m_trigger_trace_mask[channel] = mask;
		m_trigger_trace_empty_reads[channel] = 0;
	}
}

void GameController::TraceOptionsRead(const ControllerState* states, int returned, int requested, bool buffered)
{
	if (!ControllerOptionsTraceEnabled()) return;
	const int channel = buffered ? 1 : 0;
	const char* api = buffered ? "read-buffered" : "read-state";
	if (m_options_trace_reads[channel] < 4)
	{
		++m_options_trace_reads[channel];
		OptionsTrace("phase=%s requested=%d returned=%d connected=%d pending=%zu", api, requested, returned,
		             m_connected, m_digital_history.Size());
	}
	for (int index = 0; index < returned; ++index)
	{
		const bool down = (states[index].buttons & PAD_BUTTON_OPTIONS) != 0;
		if (down != m_options_trace_down[channel])
		{
			const uint64_t now = LibKernel::KernelGetProcessTime();
			OptionsTrace("phase=%s-edge requested=%d returned=%d index=%d down=%d buttons=0x%08x sample_us=%llu age_us=%llu pending=%zu",
			             api, requested, returned, index, down, states[index].buttons,
			             static_cast<unsigned long long>(states[index].time),
			             static_cast<unsigned long long>(now >= states[index].time ? now - states[index].time : 0),
			             m_digital_history.Size());
			m_options_trace_down[channel] = down;
		}
	}
}

int GameController::ReadStates(ControllerState* states, int states_num, bool* flag, int* count)
{
	EXIT_IF(flag == nullptr);
	EXIT_IF(count == nullptr);
	EXIT_IF(states == nullptr);
	EXIT_IF(states_num < 1 || states_num > STATES_MAX);

	Core::LockGuard lock(m_mutex);

	*flag  = m_connected;
	*count = m_connected_count;
	if (m_digital_history.TakeOverflow())
	{
		printf("LUMEN_PAD_DIGITAL_HISTORY phase=buffered-overflow-resync buttons=0x%08x\n", GetLastState().buttons);
		std::fflush(stdout);
	}

	int ret_num = 0;

	if (m_connected)
	{
		if (m_states_num == 0)
		{
			ret_num   = 1;
			states[0] = m_last_state;
			m_buffered_cursor_valid = true;
			m_buffered_cursor_sequence = m_digital_history.Sequence();
			m_buffered_cursor_buttons = m_last_state.buttons;
		} else
		{
			for (uint32_t i = 0; i < m_states_num; i++)
			{
				if (ret_num >= states_num)
				{
					break;
				}
				auto index = (m_first_state + i) % STATES_MAX;
				if (!m_private[index].obtained)
				{
					m_private[index].obtained = true;

					states[ret_num++] = m_states[index];
					m_digital_history.ConsumeThrough(m_private[index].digital_sequence);
					m_buffered_cursor_valid = true;
					m_buffered_cursor_sequence = m_private[index].digital_sequence;
					m_buffered_cursor_buttons = m_states[index].buttons;
				}
			}
		}
	}

	TraceOptionsRead(states, ret_num, states_num, true);
	TraceTriggerRead(states, ret_num, states_num, true);
	return ret_num;
}

void ControllerConnect(int id)
{
	EXIT_IF(g_controller == nullptr);

	g_controller->Connect(id);
}

void ControllerEnsureVirtualConnected()
{
	if (g_controller != nullptr)
	{
		g_controller->EnsureVirtualConnected();
	}
}

void ControllerResetForNextSession()
{
	if (g_controller != nullptr)
	{
		g_controller->ResetForNextSession();
	}
}

void ControllerDisconnect(int id)
{
	EXIT_IF(g_controller == nullptr);

	g_controller->Disconnect(id);
}

void ControllerButton(int id, uint32_t button, bool down)
{
	EXIT_IF(g_controller == nullptr);

	g_controller->Button(id, button, down);
}

void ControllerAxis(int id, Axis axis, int value)
{
	EXIT_IF(g_controller == nullptr);

	g_controller->Axis(id, axis, value);
}

void ControllerSetLeftStick(int id, int lx, int ly)
{
	EXIT_IF(g_controller == nullptr);

	g_controller->SetLeftStick(id, lx, ly);
}

int LUMEN_SYSV_ABI PadInit()
{
	PRINT_NAME();

	return OK;
}

// Bounded diagnostic counter: the first accepted special-port opens are
// recorded so the measured profile stays observable in a run; refusals stay
// silent and touch nothing. The bound mirrors the other bounded traces.
static constexpr int kPadOpenTraceLimit = 32;
static std::atomic<int> g_pad_open_trace_count {0};

int LUMEN_SYSV_ABI PadOpen(int user_id, int type, int index, const void* param)
{
	PRINT_NAME();

	// Port-type domain measured on the console (oracle LMNQ92801): standard
	// (0) and special (2) are recognized, every other value returns the
	// measured invalid-argument error before the user is validated.
	if (type != PAD_PORT_TYPE_STANDARD && type != PAD_PORT_TYPE_SPECIAL)
	{
		return PAD_ERROR_INVALID_ARGUMENT;
	}

	EXIT_NOT_IMPLEMENTED(user_id != 1);
	EXIT_NOT_IMPLEMENTED(index != 0);
	EXIT_NOT_IMPLEMENTED(param != nullptr);

	if (type == PAD_PORT_TYPE_SPECIAL)
	{
		// The console opens the special port with no device attached: the
		// handle is valid and its controller information reports a
		// device-less port. Every other operation on this handle keeps the
		// fail-closed treatment until its semantics are measured.
		const int count = g_pad_open_trace_count.fetch_add(1, std::memory_order_relaxed);
		if (count < kPadOpenTraceLimit)
		{
			std::fprintf(stderr,
			             "[PAD_OPEN] call=%d user=%d type=%d index=%d result=ok handle=%d model=special-port-no-device console_contract=measured\n",
			             count + 1, user_id, type, index, PAD_SPECIAL_PORT_HANDLE);
			std::fflush(stderr);
		}

		return PAD_SPECIAL_PORT_HANDLE;
	}

	// Always keep virtual keyboard/touch input connected as the final fallback. A
	// physical controller remains active because Connect() orders it first.
	ControllerEnsureVirtualConnected();

	return PAD_STANDARD_PORT_HANDLE;
}

int LUMEN_SYSV_ABI PadSetMotionSensorState(int handle, bool enable)
{
	PRINT_NAME();

	// The device-less special port is valid; this call's semantics on it are
	// not measured yet, so the fail-closed stop is kept.
	EXIT_NOT_IMPLEMENTED(handle == PAD_SPECIAL_PORT_HANDLE);
	if (handle != PAD_STANDARD_PORT_HANDLE) return PAD_ERROR_INVALID_HANDLE;

	printf("\t enable = %s\n", (enable ? "true" : "false"));

	return OK;
}

// Geometry published by the local pad model, shared by the controller
// information of both modeled ports.
static void ApplyModeledPadGeometry(PadControllerInformation* info)
{
	info->touch_pixel_density   = 44.86f;
	info->touch_resolution_x    = 1920;
	info->touch_resolution_y    = 943;
	info->stick_dead_zone_left  = controller_get_axis(-32768, 32767, 8000) - 128;
	info->stick_dead_zone_right = controller_get_axis(-32768, 32767, 8000) - 128;
}

int LUMEN_SYSV_ABI PadGetControllerInformation(int handle, PadControllerInformation* info)
{
	PRINT_NAME();

	if (handle == PAD_SPECIAL_PORT_HANDLE)
	{
		// Device-less special port: the measured console information is
		// connected=false, count=0, device_class=0 and connection_type=0
		// (oracle LMNQ92801); the geometry fields keep the local modeled pad
		// values. The query does not consult live controller state.
		EXIT_NOT_IMPLEMENTED(info == nullptr);

		ApplyModeledPadGeometry(info);
		info->connection_type = 0;
		info->connected_count = 0;
		info->connected       = false;
		info->device_class    = PAD_MODELED_DEVICE_CLASS;

		return OK;
	}

	if (handle != PAD_STANDARD_PORT_HANDLE) return PAD_ERROR_INVALID_HANDLE;

	EXIT_IF(g_controller == nullptr);

	int  connected_count = 0;
	bool connected       = false;

	g_controller->GetConnectionInfo(&connected, &connected_count);

	EXIT_NOT_IMPLEMENTED(info == nullptr);

	ApplyModeledPadGeometry(info);
	info->connection_type = 0;
	info->connected_count = (connected_count > 255 ? 255 : connected_count);
	info->connected       = connected;
	info->device_class    = PAD_MODELED_DEVICE_CLASS;

	return OK;
}

// Bounded diagnostic counter: the first accepted calls are recorded so the
// profile stays observable in a run; rejections stay silent. The bound
// mirrors the other bounded traces of the runtime.
static constexpr int kDeviceClassTraceLimit = 32;
static std::atomic<int> g_device_class_trace_count {0};

// Default record the console stores in the extended-information output as the
// first step of every call (oracle LMNQ92802): device class 0 (Standard) plus
// a zeroed 16-byte area. The write is bounded by the modeled 20-byte record.
static void StoreDeviceClassDefault(PadDeviceClassExtendedInformation* info)
{
	info->device_class = PAD_MODELED_DEVICE_CLASS;
	std::memset(info->reserved, 0, sizeof(info->reserved));
	std::memset(info->class_data, 0, sizeof(info->class_data));
}

int LUMEN_SYSV_ABI PadDeviceClassGetExtendedInformation(int handle, PadDeviceClassExtendedInformation* info)
{
	PRINT_NAME();

	// Measured on the console (oracle LMNQ92802): the call stores its default
	// 20-byte record as the first observable effect, and a rejected handle
	// still receives that default record together with the shared
	// invalid-handle error. A null output keeps the family no-write rule
	// because the console behaviour for a null pointer is not measured.
	if (handle != PAD_STANDARD_PORT_HANDLE && handle != PAD_SPECIAL_PORT_HANDLE)
	{
		if (info != nullptr)
		{
			StoreDeviceClassDefault(info);
		}

		return PAD_ERROR_INVALID_HANDLE;
	}

	// The console behaviour for a null output is not measured; keep the
	// fail-closed treatment the family uses for its other output queries
	// (PadGetControllerInformation).
	EXIT_NOT_IMPLEMENTED(info == nullptr);

	// Measured console record: the standard pad reports class 0 (Standard) and
	// the device-less special port reports class -1 (Invalid); both keep the
	// zeroed 16-byte area. The class does not consult live controller state,
	// exactly like the class field of the controller information.
	StoreDeviceClassDefault(info);
	if (handle == PAD_SPECIAL_PORT_HANDLE)
	{
		info->device_class = PAD_DEVICE_CLASS_INVALID;
	}

	const int count = g_device_class_trace_count.fetch_add(1, std::memory_order_relaxed);
	if (count < kDeviceClassTraceLimit)
	{
		std::fprintf(stderr,
		             "[PAD_DEVICE_CLASS] call=%d handle=%d result=ok class=%d bytes=%d model=%s console_contract=measured\n",
		             count + 1, handle, info->device_class, static_cast<int>(sizeof(PadDeviceClassExtendedInformation)),
		             handle == PAD_SPECIAL_PORT_HANDLE ? "special-port-no-device" : "standard-pad");
		std::fflush(stderr);
	}

	return OK;
}

// Default record of the device-less model measured on the console (oracle
// LMNQ92814): 120 bytes with centered sticks, identity orientation (w = 1),
// gravity on the Y axis and a zeroed remainder. The console delivers it
// byte-identically from both reads of the special port and from a refused
// read-state handle.
static void StoreDeviceLessPadData(PadData* data)
{
	std::memset(data, 0, sizeof(PadData));
	data->left_stick_x   = 128;
	data->left_stick_y   = 128;
	data->right_stick_x  = 128;
	data->right_stick_y  = 128;
	data->orientation_w  = 1.0f;
	data->acceleration_y = 1.0f;
}

// Bounded diagnostic counter for the measured device-less reads.
static constexpr int kPadReadTraceLimit = 32;
static std::atomic<int> g_pad_read_trace_count {0};

int LUMEN_SYSV_ABI PadReadState(int handle, PadData* data)
{
	PRINT_NAME();

	if (handle == PAD_SPECIAL_PORT_HANDLE)
	{
		// Measured console profile (oracle LMNQ92814): the device-less
		// special port stores the default record and returns success; a null
		// output is not measured and keeps the family fail-closed treatment.
		EXIT_NOT_IMPLEMENTED(data == nullptr);
		StoreDeviceLessPadData(data);

		const int count = g_pad_read_trace_count.fetch_add(1, std::memory_order_relaxed);
		if (count < kPadReadTraceLimit)
		{
			std::fprintf(stderr,
			             "[PAD_READ_STATE] call=%d handle=%d result=ok model=special-port-no-device console_contract=measured\n",
			             count + 1, handle);
			std::fflush(stderr);
		}

		return OK;
	}

	// Measured console profile (oracle LMNQ92814): a refused handle still
	// receives the default device-less record before the shared invalid-handle
	// error; a null output keeps the family no-write refusal.
	if (handle != PAD_STANDARD_PORT_HANDLE)
	{
		if (data != nullptr)
		{
			StoreDeviceLessPadData(data);
		}

		return PAD_ERROR_INVALID_HANDLE;
	}

	EXIT_IF(g_controller == nullptr);

	int             connected_count = 0;
	bool            connected       = false;
	ControllerState state;

	g_controller->ReadState(&state, &connected, &connected_count);

	EXIT_NOT_IMPLEMENTED(data == nullptr);

	data->buttons                = state.buttons;
	data->left_stick_x           = state.axes[static_cast<int>(Axis::LeftX)];
	data->left_stick_y           = state.axes[static_cast<int>(Axis::LeftY)];
	data->right_stick_x          = state.axes[static_cast<int>(Axis::RightX)];
	data->right_stick_y          = state.axes[static_cast<int>(Axis::RightY)];
	data->analog_buttons_l2      = state.axes[static_cast<int>(Axis::TriggerLeft)];
	data->analog_buttons_r2      = state.axes[static_cast<int>(Axis::TriggerRight)];
	data->orientation_x          = 0.0f;
	data->orientation_y          = 0.0f;
	data->orientation_z          = 0.0f;
	data->orientation_w          = 1.0f;
	data->acceleration_x         = 0.0f;
	data->acceleration_y         = 0.0f;
	data->acceleration_z         = 0.0f;
	data->angular_velocity_x     = 0.0f;
	data->angular_velocity_y     = 0.0f;
	data->angular_velocity_z     = 0.0f;
	data->touch_data_touch_num   = 0;
	data->touch_data_touch0_x    = 0;
	data->touch_data_touch0_y    = 0;
	data->touch_data_touch0_id   = 1;
	data->touch_data_touch1_x    = 0;
	data->touch_data_touch1_y    = 0;
	data->touch_data_touch1_id   = 2;
	data->connected              = connected;
	data->timestamp              = state.time;
	data->connected_count        = connected_count;
	data->device_unique_data_len = 0;

	return OK;
}

int LUMEN_SYSV_ABI PadRead(int handle, PadData* data, int num)
{
	PRINT_NAME();

	if (handle == PAD_SPECIAL_PORT_HANDLE)
	{
		// Measured console profile (oracle LMNQ92814): the device-less
		// special port delivers exactly one default record for every count in
		// 1..64 (the requested number never changes the result) and refuses
		// any other count with the shared invalid-argument error before
		// touching the output. A null output is not measured and keeps the
		// family fail-closed treatment.
		if (num < 1 || num > 64) return PAD_ERROR_INVALID_ARGUMENT;

		EXIT_NOT_IMPLEMENTED(data == nullptr);
		StoreDeviceLessPadData(data);

		const int count = g_pad_read_trace_count.fetch_add(1, std::memory_order_relaxed);
		if (count < kPadReadTraceLimit)
		{
			std::fprintf(stderr,
			             "[PAD_READ] call=%d handle=%d num=%d result=one-record rc=1 model=special-port-no-device console_contract=measured\n",
			             count + 1, handle, num);
			std::fflush(stderr);
		}

		return 1;
	}

	if (handle != PAD_STANDARD_PORT_HANDLE) return PAD_ERROR_INVALID_HANDLE;

	EXIT_NOT_IMPLEMENTED(num < 1 || num > 64);
	EXIT_NOT_IMPLEMENTED(data == nullptr);

	EXIT_IF(g_controller == nullptr);

	int             connected_count = 0;
	bool            connected       = false;
	ControllerState states[64];

	int ret_num = g_controller->ReadStates(states, num, &connected, &connected_count);

	if (!connected)
	{
		ret_num = 1;
	}

	for (int i = 0; i < ret_num; i++)
	{
		data[i].buttons                = states[i].buttons;
		data[i].left_stick_x           = states[i].axes[static_cast<int>(Axis::LeftX)];
		data[i].left_stick_y           = states[i].axes[static_cast<int>(Axis::LeftY)];
		data[i].right_stick_x          = states[i].axes[static_cast<int>(Axis::RightX)];
		data[i].right_stick_y          = states[i].axes[static_cast<int>(Axis::RightY)];
		data[i].analog_buttons_l2      = states[i].axes[static_cast<int>(Axis::TriggerLeft)];
		data[i].analog_buttons_r2      = states[i].axes[static_cast<int>(Axis::TriggerRight)];
		data[i].orientation_x          = 0.0f;
		data[i].orientation_y          = 0.0f;
		data[i].orientation_z          = 0.0f;
		data[i].orientation_w          = 1.0f;
		data[i].acceleration_x         = 0.0f;
		data[i].acceleration_y         = 0.0f;
		data[i].acceleration_z         = 0.0f;
		data[i].angular_velocity_x     = 0.0f;
		data[i].angular_velocity_y     = 0.0f;
		data[i].angular_velocity_z     = 0.0f;
		data[i].touch_data_touch_num   = 0;
		data[i].touch_data_touch0_x    = 0;
		data[i].touch_data_touch0_y    = 0;
		data[i].touch_data_touch0_id   = 1;
		data[i].touch_data_touch1_x    = 0;
		data[i].touch_data_touch1_y    = 0;
		data[i].touch_data_touch1_id   = 2;
		data[i].connected              = connected;
		data[i].timestamp              = states[i].time;
		data[i].connected_count        = connected_count;
		data[i].device_unique_data_len = 0;
	}

	return ret_num;
}

int LUMEN_SYSV_ABI PadSetVibration(int handle, const PadVibrationParam* param)
{
	PRINT_NAME();

	// The device-less special port is valid; this call's semantics on it are
	// not measured yet, so the fail-closed stop is kept.
	EXIT_NOT_IMPLEMENTED(handle == PAD_SPECIAL_PORT_HANDLE);
	if (handle != PAD_STANDARD_PORT_HANDLE) return PAD_ERROR_INVALID_HANDLE;

	printf("\t large_motor = %d\n", static_cast<int>(param->large_motor));
	printf("\t small_motor = %d\n", static_cast<int>(param->small_motor));

	return OK;
}

int LUMEN_SYSV_ABI PadResetLightBar(int handle)
{
	PRINT_NAME();

	// The device-less special port is valid; this call's semantics on it are
	// not measured yet, so the fail-closed stop is kept.
	EXIT_NOT_IMPLEMENTED(handle == PAD_SPECIAL_PORT_HANDLE);
	if (handle != PAD_STANDARD_PORT_HANDLE) return PAD_ERROR_INVALID_HANDLE;

	return OK;
}

int LUMEN_SYSV_ABI PadSetLightBar(int handle, const PadLightBarParam* param)
{
	PRINT_NAME();

	// The device-less special port is valid; this call's semantics on it are
	// not measured yet, so the fail-closed stop is kept.
	EXIT_NOT_IMPLEMENTED(handle == PAD_SPECIAL_PORT_HANDLE);
	if (handle != PAD_STANDARD_PORT_HANDLE) return PAD_ERROR_INVALID_HANDLE;
	EXIT_NOT_IMPLEMENTED(param == nullptr);

	return OK;
}

} // namespace Lumen::Libs::Controller

#endif // LUMEN_EMU_ENABLED
