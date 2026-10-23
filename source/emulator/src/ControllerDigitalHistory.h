#ifndef EMULATOR_SRC_CONTROLLER_DIGITAL_HISTORY_H_
#define EMULATOR_SRC_CONTROLLER_DIGITAL_HISTORY_H_

#include <cstddef>
#include <cstdint>

namespace Lumen::Libs::Controller::Detail {

class DigitalButtonHistory final
{
public:
	static constexpr std::size_t CAPACITY = 64;

	struct Sample
	{
		uint32_t buttons  = 0;
		uint64_t time     = 0;
		uint64_t sequence = 0;
	};

	void Reset(uint32_t buttons = 0)
	{
		m_head            = 0;
		m_count           = 0;
		m_current_buttons = buttons;
		m_sequence        = 0;
		m_overflowed      = false;
	}

	[[nodiscard]] bool Record(uint32_t buttons, uint64_t time)
	{
		if (buttons == m_current_buttons)
		{
			return false;
		}

		m_current_buttons = buttons;
		++m_sequence;

		// Once history is incomplete, keep dropping transitions until the
		// consumer explicitly resynchronizes to the latest physical state.
		if (m_overflowed)
		{
			return true;
		}
		if (m_count == CAPACITY)
		{
			m_head       = 0;
			m_count      = 0;
			m_overflowed = true;
			return true;
		}

		const std::size_t tail = (m_head + m_count) % CAPACITY;
		m_samples[tail]        = Sample {buttons, time, m_sequence};
		++m_count;
		return true;
	}

	[[nodiscard]] bool Pop(Sample* sample)
	{
		if (sample == nullptr || m_count == 0 || m_overflowed)
		{
			return false;
		}

		*sample = m_samples[m_head];
		m_head = (m_head + 1) % CAPACITY;
		--m_count;
		return true;
	}

	void ConsumeThrough(uint64_t sequence)
	{
		if (m_overflowed)
		{
			return;
		}
		while (m_count != 0 && m_samples[m_head].sequence <= sequence)
		{
			m_head = (m_head + 1) % CAPACITY;
			--m_count;
		}
	}

	[[nodiscard]] bool TakeOverflow()
	{
		const bool overflowed = m_overflowed;
		m_overflowed          = false;
		return overflowed;
	}

	[[nodiscard]] uint64_t Sequence() const { return m_sequence; }
	[[nodiscard]] uint32_t CurrentButtons() const { return m_current_buttons; }
	[[nodiscard]] std::size_t Size() const { return m_count; }

private:
	Sample      m_samples[CAPACITY] {};
	std::size_t m_head            = 0;
	std::size_t m_count           = 0;
	uint32_t    m_current_buttons = 0;
	uint64_t    m_sequence        = 0;
	bool        m_overflowed      = false;
};

} // namespace Lumen::Libs::Controller::Detail

#endif // EMULATOR_SRC_CONTROLLER_DIGITAL_HISTORY_H_
