#ifndef EMULATOR_INCLUDE_EMULATOR_GRAPHICS_LABELEVENTPOOL_H_
#define EMULATOR_INCLUDE_EMULATOR_GRAPHICS_LABELEVENTPOOL_H_

#include <cstddef>
#include <unordered_map>
#include <vector>

namespace Lumen::Libs::Graphics {

// The label manager serializes access and proves native completion before
// recycling. A session reset recycles its completed labels into the same pool.
// Entries remain known while checked out: the native event also remains alive,
// and repeated recycling needs no new hash node after its first admission.
template <typename Device, typename Event>
class LabelEventPool
{
public:
	bool Recycle(Device device, Event event)
	{
		if (device == Device {} || event == Event {}) return false;
		auto& pool = m_devices[device];
		const auto [entry, inserted] = pool.known.try_emplace(event, false);
		if (entry->second) return false; // Duplicate idle ownership changes nothing.
		pool.available.push_back(event);
		entry->second = true;
		++m_available;
		if (inserted) ++m_known;
		return true;
	}

	bool Take(Device device, Event* event)
	{
		if (event == nullptr) return false;
		*event = Event {};
		const auto device_pool = m_devices.find(device);
		if (device_pool == m_devices.end() || device_pool->second.available.empty()) return false;
		auto& pool = device_pool->second;
		const Event selected = pool.available.back();
		const auto entry = pool.known.find(selected);
		if (entry == pool.known.end() || !entry->second) return false;
		entry->second = false;
		pool.available.pop_back();
		--m_available;
		*event = selected;
		return true;
	}

	std::size_t Size() const { return m_available; }
	std::size_t KnownSize() const { return m_known; }

private:
	struct DevicePool
	{
		std::vector<Event> available;
		std::unordered_map<Event, bool> known;
	};
	std::unordered_map<Device, DevicePool> m_devices;
	std::size_t m_available = 0;
	std::size_t m_known = 0;
};

} // namespace Lumen::Libs::Graphics

#endif /* EMULATOR_INCLUDE_EMULATOR_GRAPHICS_LABELEVENTPOOL_H_ */
