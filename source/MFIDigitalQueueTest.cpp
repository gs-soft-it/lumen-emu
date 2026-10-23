#include "SDL_events.h"
#include "SDL_joystick.h"
#include "SDL_mfidigitalqueue.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <thread>

namespace {

using EventBuffer = std::array<SDL_MFIDigitalEvent, SDL_MFI_DIGITAL_QUEUE_CAPACITY>;

bool CheckEvent(const SDL_MFIDigitalEvent& event, uint32_t sequence)
{
	return event.type == static_cast<Uint8>(sequence & 1u) && event.index == static_cast<Uint8>(sequence) &&
	       event.value == static_cast<Uint8>(0xffu - sequence);
}

bool TestOrderAndWrap()
{
	SDL_MFIDigitalQueue queue {};
	EventBuffer         events {};
	SDL_MFIDigitalQueueReset(&queue);

	if (SDL_MFIDigitalQueueDrain(&queue, events.data(), static_cast<int>(events.size())) != 0)
	{
		return false;
	}

	for (uint32_t sequence = 0; sequence < SDL_MFI_DIGITAL_QUEUE_CAPACITY; ++sequence)
	{
		if (!SDL_MFIDigitalQueueEnqueue(&queue, static_cast<Uint8>(sequence & 1u), static_cast<Uint8>(sequence),
		                                    static_cast<Uint8>(0xffu - sequence)))
		{
			return false;
		}
	}

	constexpr int FIRST_DRAIN = 17;
	const int     first_count = SDL_MFIDigitalQueueDrain(&queue, events.data(), FIRST_DRAIN);
	if (first_count != FIRST_DRAIN)
	{
		return false;
	}
	for (int index = 0; index < first_count; ++index)
	{
		if (!CheckEvent(events[static_cast<size_t>(index)], static_cast<uint32_t>(index)))
		{
			return false;
		}
	}

	for (uint32_t sequence = SDL_MFI_DIGITAL_QUEUE_CAPACITY;
	     sequence < SDL_MFI_DIGITAL_QUEUE_CAPACITY + FIRST_DRAIN; ++sequence)
	{
		if (!SDL_MFIDigitalQueueEnqueue(&queue, static_cast<Uint8>(sequence & 1u), static_cast<Uint8>(sequence),
		                                    static_cast<Uint8>(0xffu - sequence)))
		{
			return false;
		}
	}

	const int wrapped_count = SDL_MFIDigitalQueueDrain(&queue, events.data(), static_cast<int>(events.size()));
	if (wrapped_count != static_cast<int>(events.size()))
	{
		return false;
	}
	for (int index = 0; index < wrapped_count; ++index)
	{
		const uint32_t sequence = static_cast<uint32_t>(index + FIRST_DRAIN);
		if (!CheckEvent(events[static_cast<size_t>(index)], sequence))
		{
			return false;
		}
	}

	return SDL_MFIDigitalQueueDrain(&queue, events.data(), static_cast<int>(events.size())) == 0 &&
	       !SDL_MFIDigitalQueueTakeOverflow(&queue);
}

bool TestOverflowRecovery()
{
	SDL_MFIDigitalQueue queue {};
	EventBuffer         events {};
	SDL_MFIDigitalQueueReset(&queue);

	for (uint32_t index = 0; index < SDL_MFI_DIGITAL_QUEUE_CAPACITY; ++index)
	{
		if (!SDL_MFIDigitalQueueEnqueue(&queue, SDL_MFI_DIGITAL_EVENT_BUTTON, static_cast<Uint8>(index), SDL_PRESSED))
		{
			return false;
		}
	}

	if (SDL_MFIDigitalQueueEnqueue(&queue, SDL_MFI_DIGITAL_EVENT_BUTTON, 7, SDL_RELEASED) ||
	    SDL_MFIDigitalQueueEnqueue(&queue, SDL_MFI_DIGITAL_EVENT_BUTTON, 8, SDL_PRESSED) ||
	    SDL_MFIDigitalQueueDrain(&queue, events.data(), static_cast<int>(events.size())) != 0 ||
	    !SDL_MFIDigitalQueueTakeOverflow(&queue) || SDL_MFIDigitalQueueTakeOverflow(&queue))
	{
		return false;
	}

	if (!SDL_MFIDigitalQueueEnqueue(&queue, SDL_MFI_DIGITAL_EVENT_HAT, 0, SDL_HAT_RIGHT))
	{
		return false;
	}
	const int recovered = SDL_MFIDigitalQueueDrain(&queue, events.data(), 1);
	return recovered == 1 && events[0].type == SDL_MFI_DIGITAL_EVENT_HAT && events[0].index == 0 &&
	       events[0].value == SDL_HAT_RIGHT;
}

bool TestStopAndReset()
{
	SDL_MFIDigitalQueue queue {};
	EventBuffer         events {};
	SDL_MFIDigitalQueueReset(&queue);
	if (!SDL_MFIDigitalQueueEnqueue(&queue, SDL_MFI_DIGITAL_EVENT_BUTTON, 2, SDL_PRESSED))
	{
		return false;
	}

	SDL_MFIDigitalQueueStop(&queue);
	SDL_MFIDigitalQueueStop(&queue);
	if (SDL_MFIDigitalQueueEnqueue(&queue, SDL_MFI_DIGITAL_EVENT_BUTTON, 2, SDL_RELEASED) ||
	    SDL_MFIDigitalQueueDrain(&queue, events.data(), static_cast<int>(events.size())) != 0 ||
	    SDL_MFIDigitalQueueTakeOverflow(&queue))
	{
		return false;
	}

	SDL_MFIDigitalQueueReset(&queue);
	if (!SDL_MFIDigitalQueueEnqueue(&queue, SDL_MFI_DIGITAL_EVENT_BUTTON, 3, SDL_PRESSED))
	{
		return false;
	}
	return SDL_MFIDigitalQueueDrain(&queue, events.data(), 1) == 1 && events[0].index == 3 &&
	       events[0].value == SDL_PRESSED;
}

bool TestConcurrentDrainAndStop()
{
	SDL_MFIDigitalQueue queue {};
	SDL_MFIDigitalQueueReset(&queue);

	std::atomic<bool>     start {false};
	std::atomic<bool>     producer_done {false};
	std::atomic<bool>     invalid_event {false};
	std::atomic<uint32_t> attempts {0};
	std::atomic<uint32_t> accepted {0};
	std::atomic<uint32_t> drained {0};
	std::atomic<uint32_t> overflows {0};

	std::thread producer([&]
	{
		while (!start.load(std::memory_order_acquire))
		{
			std::this_thread::yield();
		}
		for (uint32_t sequence = 0; sequence < 200000; ++sequence)
		{
			if (SDL_MFIDigitalQueueEnqueue(&queue, SDL_MFI_DIGITAL_EVENT_BUTTON, static_cast<Uint8>(sequence & 3u),
			                                    static_cast<Uint8>(sequence & 1u)))
			{
				accepted.fetch_add(1, std::memory_order_relaxed);
			}
			attempts.store(sequence + 1, std::memory_order_release);
			if ((sequence & 63u) == 0)
			{
				std::this_thread::yield();
			}
		}
		producer_done.store(true, std::memory_order_release);
	});

	std::thread consumer([&]
	{
		std::array<SDL_MFIDigitalEvent, 19> events {};
		while (!start.load(std::memory_order_acquire))
		{
			std::this_thread::yield();
		}
		do
		{
			const int count = SDL_MFIDigitalQueueDrain(&queue, events.data(), static_cast<int>(events.size()));
			for (int index = 0; index < count; ++index)
			{
				const SDL_MFIDigitalEvent& event = events[static_cast<size_t>(index)];
				if (event.type != SDL_MFI_DIGITAL_EVENT_BUTTON || event.index >= 4 || event.value > SDL_PRESSED)
				{
					invalid_event.store(true, std::memory_order_relaxed);
				}
			}
			drained.fetch_add(static_cast<uint32_t>(count), std::memory_order_relaxed);
			if (SDL_MFIDigitalQueueTakeOverflow(&queue))
			{
				overflows.fetch_add(1, std::memory_order_relaxed);
			}
			std::this_thread::yield();
		} while (!producer_done.load(std::memory_order_acquire));
	});

	start.store(true, std::memory_order_release);
	while (attempts.load(std::memory_order_acquire) < 10000 && !producer_done.load(std::memory_order_acquire))
	{
		std::this_thread::yield();
	}
	SDL_MFIDigitalQueueStop(&queue);
	producer.join();
	consumer.join();
	SDL_MFIDigitalQueueStop(&queue);

	EventBuffer events {};
	const bool  stopped = !SDL_MFIDigitalQueueEnqueue(&queue, SDL_MFI_DIGITAL_EVENT_BUTTON, 0, SDL_PRESSED) &&
	                     SDL_MFIDigitalQueueDrain(&queue, events.data(), static_cast<int>(events.size())) == 0 &&
	                     !SDL_MFIDigitalQueueTakeOverflow(&queue);
	std::printf("mfi-digital-queue concurrent accepted=%u drained=%u overflows=%u\n", accepted.load(), drained.load(),
	            overflows.load());
	return stopped && !invalid_event.load(std::memory_order_relaxed) && accepted.load(std::memory_order_relaxed) != 0;
}

} // namespace

int main()
{
	const bool order_ok      = TestOrderAndWrap();
	const bool overflow_ok   = TestOverflowRecovery();
	const bool stop_ok       = TestStopAndReset();
	const bool concurrent_ok = TestConcurrentDrainAndStop();

	std::printf("mfi-digital-queue order=%s overflow=%s stop=%s concurrent=%s\n", order_ok ? "ok" : "fail",
	            overflow_ok ? "ok" : "fail", stop_ok ? "ok" : "fail", concurrent_ok ? "ok" : "fail");
	return order_ok && overflow_ok && stop_ok && concurrent_ok ? 0 : 1;
}
