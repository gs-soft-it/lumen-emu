#ifndef LUMEN_EMULATOR_GRAPHICS_SHADER_SOURCE_SNAPSHOT_CACHE_H
#define LUMEN_EMULATOR_GRAPHICS_SHADER_SOURCE_SNAPSHOT_CACHE_H

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <utility>

namespace Lumen::Libs::Graphics {

// Immutable owned bytes, never a guest mapping or permission proof. Allocation
// of both payload and ownership metadata is nonthrowing, including builds that
// disable C++ exceptions. Retained readers prevent address-reuse/ABA identities.
class ShaderSourceWords
{
public:
	ShaderSourceWords() = default;
	ShaderSourceWords(const ShaderSourceWords& other) noexcept: m_storage(other.m_storage) { Retain(); }
	ShaderSourceWords(ShaderSourceWords&& other) noexcept: m_storage(std::exchange(other.m_storage, nullptr)) {}
	~ShaderSourceWords() { Release(); }
	ShaderSourceWords& operator=(ShaderSourceWords other) noexcept
	{
		std::swap(m_storage, other.m_storage);
		return *this;
	}
	const uint32_t* Data() const { return m_storage == nullptr ? nullptr : m_storage->words.get(); }
	size_t Bytes() const { return m_storage == nullptr ? 0 : m_storage->bytes; }
	explicit operator bool() const { return m_storage != nullptr; }
	bool SameStorage(const ShaderSourceWords& other) const { return m_storage != nullptr && m_storage == other.m_storage; }
	static ShaderSourceWords Copy(const uint32_t* source, size_t bytes)
	{
		ShaderSourceWords result;
		if (source == nullptr || bytes == 0 || bytes % sizeof(uint32_t) != 0) return result;
		auto storage = std::unique_ptr<Storage>(new (std::nothrow) Storage);
		if (!storage) return result;
		storage->words.reset(new (std::nothrow) uint32_t[bytes / sizeof(uint32_t)]);
		if (!storage->words) return result;
		std::memcpy(storage->words.get(), source, bytes);
		storage->bytes = bytes;
		result.m_storage = storage.release();
		return result;
	}

private:
	struct Storage
	{
		std::atomic<size_t> references {1};
		size_t bytes = 0;
		std::unique_ptr<uint32_t[]> words;
	};
	void Retain() noexcept { if (m_storage != nullptr) m_storage->references.fetch_add(1, std::memory_order_relaxed); }
	void Release() noexcept
	{
		if (m_storage != nullptr && m_storage->references.fetch_sub(1, std::memory_order_acq_rel) == 1) delete m_storage;
	}
	Storage* m_storage = nullptr;
};

// The caller must publish all native producers and hold a fresh read lease for
// the exact source extent, or supply its complete admitted private copy.
// Address/length select ONE candidate, never authorize
// reuse: every byte, including padding and binary metadata, is compared again.
class ShaderSourceSnapshotCache
{
public:
	static constexpr size_t CAPACITY = 16;
	static constexpr size_t BYTE_CAP = 2 * 1024 * 1024;

	ShaderSourceWords CaptureWithPrefix(uint64_t address, const uint32_t* current, size_t bytes,
	    const uint32_t (&prefix)[2], uint64_t generation)
	{
		// Validate the full read against the earlier length header before it may
		// change the cache. A failed/partial private copy must never call here.
		return CaptureImpl(address, current, bytes, generation, prefix);
	}

	ShaderSourceWords Capture(uint64_t address, const uint32_t* current, size_t bytes, uint64_t generation)
	{
		return CaptureImpl(address, current, bytes, generation, nullptr);
	}
	size_t RetainedBytes() const { return m_bytes; }
	void Clear()
	{
		for (size_t slot = 0; slot < CAPACITY; ++slot) Remove(slot);
	}

private:
	ShaderSourceWords CaptureImpl(uint64_t address, const uint32_t* current, size_t bytes,
	    uint64_t generation, const uint32_t* prefix)
	{
		if (address == 0 || current == nullptr || bytes == 0 || bytes % sizeof(uint32_t) != 0) return {};
		const auto matches_prefix = [&](const uint32_t* words) {
			return prefix == nullptr || (bytes >= 2 * sizeof(uint32_t) &&
			    words[0] == prefix[0] && words[1] == prefix[1]);
		};
		if (!matches_prefix(current)) return {};
		const size_t slot = Slot(address, bytes);
		auto& entry = m_entries[slot];
		if (m_generation == generation && entry.address == address && entry.words.Bytes() == bytes &&
		    matches_prefix(entry.words.Data()) && std::memcmp(entry.words.Data(), current, bytes) == 0) return entry.words;
		auto captured = ShaderSourceWords::Copy(current, bytes);
		// The fallback source may still be a raw guest borrow. Check the owned
		// result too: a header changing during memcpy cannot enter the cache.
		if (!captured || !matches_prefix(captured.Data())) return {};
		if (m_generation != generation) { Clear(); m_generation = generation; }
		if (bytes > BYTE_CAP) return captured;
		Remove(slot);
		// Eviction is a miss-only operation. Hot draws have one O(1) lookup and
		// one full comparison, never a scan or comparison of every cached shader.
		for (size_t offset = 1; m_bytes > BYTE_CAP - bytes && offset < CAPACITY; ++offset)
			Remove((slot + offset) % CAPACITY);
		entry.address = address;
		entry.words = captured;
		m_bytes += bytes;
		return captured;
	}
	struct Entry { uint64_t address = 0; ShaderSourceWords words; };
	static size_t Slot(uint64_t address, size_t bytes)
	{
		uint64_t key = address ^ (uint64_t(bytes) * UINT64_C(0x9e3779b97f4a7c15));
		key ^= key >> 30;
		key *= UINT64_C(0xbf58476d1ce4e5b9);
		key ^= key >> 27;
		return static_cast<size_t>(key ^ (key >> 31)) & (CAPACITY - 1);
	}
	void Remove(size_t slot)
	{
		m_bytes -= m_entries[slot].words.Bytes();
		m_entries[slot] = {};
	}
	std::array<Entry, CAPACITY> m_entries;
	size_t m_bytes = 0;
	uint64_t m_generation = 0;
};

} // namespace Lumen::Libs::Graphics

#endif
