#include "Lumen/Core/RefCounter.h"
#include "Lumen/Core/RefCounterPolicy.h"
#include "Lumen/Core/Vector.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace Lumen::Core {
namespace {

constexpr const char* SELECTOR = "LUMEN_CORE_ATOMIC_REFS";

bool SetSelector(const char* value)
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS
	const bool okay = _putenv_s(SELECTOR, value != nullptr ? value : "") == 0;
#else
	const bool okay = value != nullptr ? setenv(SELECTOR, value, 1) == 0 : unsetenv(SELECTOR) == 0;
#endif
	ConfigureAtomicRefsFromEnvironment();
	return okay;
}

struct RestoreSelector
{
	RestoreSelector()
	{
		const char* value = std::getenv(SELECTOR);
		present = value != nullptr;
		if (present) original = value;
	}
	~RestoreSelector() { SetSelector(present ? original.c_str() : nullptr); }
	bool present = false;
	std::string original;
};

struct CountingMutex
{
	CountingMutex() { constructed.fetch_add(1); }
	~CountingMutex() { destroyed.fetch_add(1); }
	void Lock() { locked.fetch_add(1); mutex.lock(); }
	void Unlock() { mutex.unlock(); }
	static void Reset() { constructed = 0; destroyed = 0; locked = 0; }

	static inline std::atomic_uint constructed {0};
	static inline std::atomic_uint destroyed {0};
	static inline std::atomic_uint locked {0};
	std::mutex mutex;
};

struct Probe: RefCounter<CountingMutex>
{
	explicit Probe(std::atomic_uint* deaths): deaths(deaths) {}
	Probe(const Probe& source): deaths(source.deaths), value(source.value)
	{
#if defined(__cpp_exceptions)
		if (source.fail_copy) throw 7;
#endif
	}
	~Probe() override { deaths->fetch_add(1); }
	std::atomic_uint* deaths;
	int value = 42;
	bool fail_copy = false;
};

bool OwnershipMechanism(bool atomic)
{
	if (!SetSelector(atomic ? "1" : "0") || AtomicRefsEnabled() != atomic) return false;
	CountingMutex::Reset();
	std::atomic_uint deaths {0};
	auto* first = new Probe(&deaths);
	auto* original = first;
	first->CopyOnWrite(&first);
	bool okay = first == original;
	Probe* second = nullptr;
	first->CopyPtr(&second, first);
	first->CopyOnWrite(&first);
	first->value = 91;
	okay = okay && first != second && second->value == 42 && first->value == 91 && deaths.load() == 0;
	second->Release();
	first->Release();
	okay = okay && deaths.load() == 2;
	if (atomic)
		return okay && CountingMutex::constructed.load() == 0 && CountingMutex::destroyed.load() == 0 && CountingMutex::locked.load() == 0;
	return okay && CountingMutex::constructed.load() == 2 && CountingMutex::destroyed.load() == 2 && CountingMutex::locked.load() > 0;
}

bool ExistingObjectKeepsMode(bool atomic)
{
	if (!SetSelector(atomic ? "1" : "0")) return false;
	CountingMutex::Reset();
	std::atomic_uint deaths {0};
	auto* first = new Probe(&deaths);
	if (!SetSelector(atomic ? "0" : "1")) { first->Release(); return false; }
	Probe* second = nullptr;
	first->CopyPtr(&second, first);
	bool okay = CountingMutex::locked.load() == (atomic ? 0u : 1u);
	first->CopyOnWrite(&first); // The new object captures the new policy.
	first->value = 99;
	okay = okay && second->value == 42 && first->value == 99;
	second->Release();
	first->Release();
	return okay && deaths.load() == 2 && CountingMutex::constructed.load() == 1 && CountingMutex::destroyed.load() == 1;
}

bool ThrowingCloneKeepsOwner(bool atomic)
{
#if defined(__cpp_exceptions)
	if (!SetSelector(atomic ? "1" : "0")) return false;
	std::atomic_uint deaths {0};
	auto* first = new Probe(&deaths);
	Probe* second = nullptr;
	first->CopyPtr(&second, first);
	first->fail_copy = true; // Both owners are local and no payload reader runs.
	bool caught = false;
	try { first->CopyOnWrite(&first); } catch (int) { caught = true; }
	bool okay = caught && first == second && deaths.load() == 0;
	first->fail_copy = false;
	second->Release();
	first->CopyOnWrite(&first);
	okay = okay && first->value == 42 && deaths.load() == 0;
	first->Release();
	return okay && deaths.load() == 1;
#else
	(void)atomic;
	return true;
#endif
}

struct CloneGate
{
	std::atomic_bool entered {false};
	std::atomic_bool resume {false};
	std::atomic_uint source_destroyed {0};
	std::atomic_uint clones {0};
};

struct PausedElement
{
	PausedElement() = default;
	PausedElement(const PausedElement& source): gate(source.gate)
	{
		gate->entered.store(true, std::memory_order_release);
		while (!gate->resume.load(std::memory_order_acquire)) std::this_thread::yield();
		// The test refuses to resume a clone if its source was already freed.
		value = source.value;
		gate->clones.fetch_add(1);
	}
	~PausedElement()
	{
		if (original && gate != nullptr) gate->source_destroyed.fetch_add(1);
	}
	CloneGate* gate = nullptr;
	int value = 0;
	bool original = false;
};

bool ProductionCloneRetainsSource(bool atomic)
{
	if (!SetSelector(atomic ? "1" : "0")) return false;
	CloneGate gate;
	Vector<PausedElement> writer(1);
	writer[0].gate = &gate;
	writer[0].value = 42;
	writer[0].original = true;
	auto sibling = std::make_unique<Vector<PausedElement>>(writer);
	std::thread clone([&] { writer[0].value += 1; });
	while (!gate.entered.load(std::memory_order_acquire)) std::this_thread::yield();
	sibling.reset(); // Deliberately release the other owner halfway through copy.
	if (gate.source_destroyed.load() != 0)
	{
		std::fprintf(stderr, "[CORE_REFCOUNTER_TEST] FAIL source destroyed before production Vector clone completed\n");
		std::abort(); // Do not let a regressed implementation perform the freed read.
	}
	gate.resume.store(true, std::memory_order_release);
	clone.join();
	const auto& result = writer;
	return gate.source_destroyed.load() == 1 && gate.clones.load() == 1 && result[0].value == 43;
}

bool ProductionNestedOwners(bool atomic)
{
	if (!SetSelector(atomic ? "1" : "0")) return false;
	Vector<std::string> child;
	child.Add(std::string("source"));
	Vector<Vector<std::string>> first;
	first.Add(child);
	auto second = first;
	second[0][0] = "detached";
	second[0].Add(std::string("tail"));
	const auto& a = first;
	const auto& b = second;
	bool okay = a[0].Size() == 1 && a[0][0] == "source" && b[0].Size() == 2 && b[0][0] == "detached" && b[0][1] == "tail";
	auto moved = std::move(second);
	Vector<Vector<std::string>> assigned;
	assigned = moved;
	assigned[0][1] = "independent";
	const auto& c = moved;
	const auto& d = assigned;
	return okay && c[0][1] == "tail" && d[0][1] == "independent";
}

bool ProductionConcurrentOwners(bool atomic)
{
	if (!SetSelector(atomic ? "1" : "0")) return false;
	Vector<uint32_t> inner;
	inner.Add(0u);
	Vector<Vector<uint32_t>> initial;
	initial.Add(inner);
	const auto& source = initial;
	std::array<std::thread, 8> workers;
	std::atomic_uint arrived {0};
	std::atomic_bool okay {true};
	for (size_t index = 0; index < workers.size(); ++index)
	{
		workers[index] = std::thread([&, index] {
			auto local = source;
			arrived.fetch_add(1, std::memory_order_release);
			while (arrived.load(std::memory_order_acquire) != workers.size()) std::this_thread::yield();
			for (uint32_t round = 0; round < 32; ++round)
			{
				auto sibling = local;
				local[0][0] = static_cast<uint32_t>(index + 1) + round;
				const auto& old = sibling;
				const uint32_t expected = round == 0 ? 0u : static_cast<uint32_t>(index) + round;
				if (old[0][0] != expected || source[0][0] != 0) okay.store(false);
			}
		});
	}
	for (auto& worker: workers) worker.join();
	return okay.load() && source[0][0] == 0;
}

struct DummyProbe: RefCounter<DummyMutex>
{
	DummyProbe() = default;
	DummyProbe(const DummyProbe& source): value(source.value) {}
	int value = 7;
};

bool DummyMutexContract(bool atomic)
{
	if (!SetSelector(atomic ? "1" : "0")) return false;
	auto* first = new DummyProbe;
	DummyProbe* second = nullptr;
	first->CopyPtr(&second, first);
	second->CopyOnWrite(&second);
	second->value = 9;
	const bool okay = first->value == 7 && second->value == 9;
	first->Release();
	second->Release();
	return okay;
}

} // namespace

bool CoreRefCounterContractTest()
{
	RestoreSelector restore;
	bool okay = SetSelector(nullptr) && !AtomicRefsEnabled();
	okay = SetSelector("true") && !AtomicRefsEnabled() && okay;
	for (bool atomic: {false, true})
	{
		const bool mechanism = OwnershipMechanism(atomic);
		const bool captured_mode = ExistingObjectKeepsMode(atomic);
		const bool throwing = ThrowingCloneKeepsOwner(atomic);
		const bool lifetime = ProductionCloneRetainsSource(atomic);
		const bool nested = ProductionNestedOwners(atomic);
		const bool concurrent = ProductionConcurrentOwners(atomic);
		const bool dummy = DummyMutexContract(atomic);
		const bool passed = mechanism && captured_mode && throwing && lifetime && nested && concurrent && dummy;
		std::fprintf(stderr, "[CORE_REFCOUNTER_TEST] mode=%s result=%s mechanism=%u captured_mode=%u throwing_clone=%u "
		                     "production_clone_lifetime=%u nested=%u concurrent_owners=%u dummy=%u\n",
		             atomic ? "atomic" : "legacy", passed ? "PASS" : "FAIL", mechanism, captured_mode, throwing, lifetime, nested, concurrent, dummy);
		okay = passed && okay;
	}
	return okay;
}

} // namespace Lumen::Core
