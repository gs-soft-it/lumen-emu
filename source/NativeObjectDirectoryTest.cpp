// Lumen, 2026. SPDX-License-Identifier: GPL-2.0-only
#include "Emulator/Kernel/ReadMostlyObjectDirectory.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

namespace {
struct Object { unsigned identity; std::atomic<unsigned>* destroyed; };
using Directory = Lumen::Libs::LibKernel::ReadMostlyObjectDirectory<Object, 16>;

void Check(bool condition, const char* message)
{
	if (!condition) { std::fprintf(stderr, "NativeObjectDirectoryTest: FAIL: %s\n", message); std::abort(); }
}
void Delete(Object* object) { object->destroyed->fetch_add(1); delete object; }
class Gate final
{
public:
	void Open() { { std::lock_guard lock(m_mutex); m_open = true; } m_changed.notify_all(); }
	void Wait()
	{
		std::unique_lock lock(m_mutex);
		Check(m_changed.wait_for(lock, std::chrono::seconds(10), [&] { return m_open; }), "gate watchdog");
	}
private:
	std::mutex m_mutex;
	std::condition_variable m_changed;
	bool m_open = false;
};
struct Pause { Gate entered, release; };
void PauseBeforePin(void* opaque)
{
	auto& pause = *static_cast<Pause*>(opaque);
	pause.entered.Open();
	pause.release.Wait();
}
void Remove(Directory& directory, Directory::Record* record)
{
	Directory::Garbage garbage;
	Check(Directory::TryClose(record), "uncontended close");
	Check(directory.Remove(record, Delete, garbage), "remove current identity");
}
void Collect(Directory& directory) { Directory::Garbage garbage; directory.Collect(garbage); }

void TestPinAndRollback()
{
	std::atomic<unsigned> destroyed {0};
	Directory directory;
	Directory::Garbage garbage;
	auto* record = directory.Insert(1, new Object {1, &destroyed}, garbage);
	Check(record != nullptr, "initial insert");
	auto pin = directory.FindAndPin(1);
	Check(pin.status == Directory::Lookup::Pinned && pin.record == record, "pin identity");
	Check(!Directory::TryClose(record), "live operation must reject destroy");
	Directory::Release(pin.record);
	Check(Directory::TryClose(record), "unowned close");
	Check(directory.FindAndPin(1).status == Directory::Lookup::Closing, "closed record cannot pin");
	Directory::Reopen(record); // Native destroy reported EBUSY: preserve the wrapper.
	pin = directory.FindAndPin(1);
	Check(pin.status == Directory::Lookup::Pinned && pin.record == record, "failed destroy reopens same identity");
	Directory::Release(pin.record);
	Remove(directory, record);
	Check(destroyed == 1 && directory.Inspect().records == 0, "unpinned removal reclaimed exactly once");
}

void TestSuspendedReaderAndReuse()
{
	std::atomic<unsigned> destroyed {0};
	Directory directory;
	Directory::Garbage garbage;
	auto* original = directory.Insert(7, new Object {1, &destroyed}, garbage);
	auto* independent = directory.Insert(9, new Object {2, &destroyed}, garbage);
	Pause pause;
	Directory::Result delayed {Directory::Lookup::Missing, nullptr};
	std::thread reader([&] { delayed = directory.FindAndPin(7, PauseBeforePin, &pause); });
	pause.entered.Wait();
	Remove(directory, original);
	Check(destroyed == 0, "hazard retains closed record before CAS");
	auto* replacement = directory.Insert(7, new Object {3, &destroyed}, garbage);
	Check(replacement != original, "hazard prevents record address reuse");
	// This completes while the first reader is still stopped in the production
	// lookup. No wall-clock comparison or simulated lock fast path is used.
	Gate progressed;
	std::thread other([&] {
		auto result = directory.FindAndPin(9);
		Check(result.status == Directory::Lookup::Pinned && result.record == independent, "independent progress");
		Directory::Release(result.record);
		progressed.Open();
	});
	progressed.Wait();
	other.join();
	auto current = directory.FindAndPin(7);
	Check(current.status == Directory::Lookup::Pinned && current.record->value->identity == 3, "new identity is visible");
	Directory::Release(current.record);
	pause.release.Open();
	reader.join();
	Check(delayed.status == Directory::Lookup::Closing && delayed.record == nullptr, "old snapshot cannot pin replacement");
	Collect(directory);
	Check(destroyed == 1, "last hazard collection reclaims old identity");
	Remove(directory, replacement);
	Remove(directory, independent);
	Check(destroyed == 3 && directory.Inspect().records == 0 && directory.Inspect().retired_buckets == 0,
	      "all retired identities and snapshots reclaimed");
}

void TestPinsAfterRetirementAndNestedRead()
{
	std::atomic<unsigned> destroyed {0};
	Directory directory;
	Directory::Garbage garbage;
	auto* first = directory.Insert(11, new Object {1, &destroyed}, garbage);
	auto* second = directory.Insert(12, new Object {2, &destroyed}, garbage);
	struct Nested { Directory* directory; bool called = false; } nested {&directory};
	auto held = directory.FindAndPin(11, [](void* opaque) {
		auto& state = *static_cast<Nested*>(opaque);
		auto pin = state.directory->FindAndPin(12);
		Check(pin.status == Directory::Lookup::Pinned && pin.record->value->identity == 2, "nested hazard slot");
		Directory::Release(pin.record);
		state.called = true;
	}, &nested);
	Check(nested.called && held.status == Directory::Lookup::Pinned, "outer hazard survives nested read");
	Check(directory.Remove(first, Delete, garbage), "unpublished rollback retirement with pin");
	garbage.Drain();
	Check(destroyed == 0 && held.record->value->identity == 1, "retired native value survives last operation");
	Directory::Release(held.record);
	Collect(directory);
	Check(destroyed == 1, "retired pin reclaimed after release");
	Remove(directory, second);
}

void TestProvisionalPublication()
{
	std::atomic<unsigned> destroyed {0};
	Directory directory;
	Directory::Garbage garbage;
	const auto prepare = [](Directory::Record* record, void*) { record->state.store(0, std::memory_order_relaxed); };
	auto* pending = directory.Insert(21, new Object {1, &destroyed}, garbage, prepare);
	Check(directory.FindAndPin(21).status == Directory::Lookup::Closing, "unpublished identity cannot enroll");
	Pause pause;
	Directory::Result delayed {Directory::Lookup::Missing, nullptr};
	std::thread reader([&] { delayed = directory.FindAndPin(21, PauseBeforePin, &pause); });
	pause.entered.Wait();
	Check(directory.Remove(pending, Delete, garbage), "failed publication removal");
	garbage.Drain();
	Check(destroyed == 0, "provisional record retained for snapshot reader");
	pause.release.Open();
	reader.join();
	Check(delayed.status == Directory::Lookup::Closing, "failed publication never grants pin");
	Collect(directory);
	Check(destroyed == 1, "failed publication safely reclaimed");
	auto* published = directory.Insert(21, new Object {2, &destroyed}, garbage, prepare);
	Directory::Reopen(published); // The output slot write completed successfully.
	auto pin = directory.FindAndPin(21);
	Check(pin.status == Directory::Lookup::Pinned && pin.record == published, "completed publication opens identity");
	Directory::Release(pin.record);
	Remove(directory, published);
	Check(destroyed == 2, "successful publication reclaimed once");
}

void TestBucketPublicationAndBoundedCollection()
{
	std::atomic<unsigned> destroyed {0};
	Directory directory;
	Directory::Garbage garbage;
	std::vector<Directory::Record*> records;
	std::array<unsigned, 16> occupancy {};
	size_t expected_copies = 0;
	for (uintptr_t key = 1; key <= 128; ++key)
	{
		expected_copies += ++occupancy[Directory::Index(key)];
		records.push_back(directory.Insert(key, new Object {static_cast<unsigned>(key), &destroyed}, garbage));
	}
	Check(directory.Inspect().entries_copied == expected_copies, "publication clones only affected bucket");
	Check(directory.Inspect().retired_buckets == 0, "unprotected old buckets do not accumulate");
	for (auto* record: records) Remove(directory, record);
	for (unsigned generation = 0; generation != 256; ++generation)
	{
		auto* record = directory.Insert(42, new Object {generation, &destroyed}, garbage);
		auto pin = directory.FindAndPin(42);
		Check(pin.status == Directory::Lookup::Pinned && pin.record->value->identity == generation, "repeated key identity");
		Directory::Release(pin.record);
		Remove(directory, record);
		Check(directory.Inspect().records == 0 && directory.Inspect().retired_buckets == 0, "cold operations reclaim retirement");
	}
	Check(destroyed == 384, "no retirement leaks");
}
} // namespace

int main()
{
	TestPinAndRollback();
	TestSuspendedReaderAndReuse();
	TestPinsAfterRetirementAndNestedRead();
	TestProvisionalPublication();
	TestBucketPublicationAndBoundedCollection();
	std::puts("NativeObjectDirectoryTest: PASS (identity/pins/rollback/provisional/hazard/reuse/nesting/independent-progress/reclamation)");
}
