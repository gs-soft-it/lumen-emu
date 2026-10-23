// Lumen modification, 2026-10-02: acquire Android guest placeholders on demand
// without exhausting the native 39-bit host VA or replacing native mappings.
// Internal memory address-space helpers included by memory.cpp.
// Kept in the same translation unit because they depend on memory.cpp-local helpers.

static constexpr uint64_t HOST_SYSTEM_MANAGED_MIN  = 0x0000040000ull;
static constexpr uint64_t HOST_SYSTEM_MANAGED_MAX  = 0x07ffffbfffull;
#if defined(__APPLE__) && (defined(__x86_64__) || defined(_M_X64))
static constexpr uint64_t HOST_SYSTEM_RESERVED_MAX = 0x0fbfffffffull;
static constexpr uint64_t HOST_USER_MIN            = 0x7000000000ull;
#else
static constexpr uint64_t HOST_SYSTEM_RESERVED_MAX = 0x0fffffffffull;
static constexpr uint64_t HOST_USER_MIN            = 0x1000000000ull;
#endif
static constexpr uint64_t HOST_USER_MAX = 0xfbffffffffull;

static bool GuestFreeRangeContains(uint64_t base, uint64_t range_size, uint64_t candidate,
                                   uint64_t size) {
	if (UINT64_MAX - base < range_size || candidate < base) {
		return false;
	}
	const auto range_end = base + range_size;
	return candidate <= range_end && size <= range_end - candidate;
}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
static bool CoalescePlaceholders(uint64_t vaddr, uint64_t size) {
	return size != 0 && VirtualFree(reinterpret_cast<void*>(vaddr), size,
	                                MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS) != 0;
}
#endif

// One atomic state carries a process-lifetime sequence (high 48 bits) and
// active mutators (low 16 bits). Nested/concurrent mutations cannot expose an
// even generation between their begin and end; no VM or renderer lock is added.
static_assert(std::atomic<uint64_t>::is_always_lock_free);
static std::atomic<uint64_t> g_backing_mutation_state {0};
static constexpr uint64_t BackingMutationSequenceStep = uint64_t {1} << 16;
static constexpr uint64_t BackingMutationCountMask = BackingMutationSequenceStep - 1;

static uint64_t ReadBackingMutationGeneration() noexcept {
	if (!Common::UniformMetadata::Enabled()) return 0;
	const auto state = g_backing_mutation_state.load(std::memory_order_seq_cst);
	return (state & ~BackingMutationCountMask) | ((state & BackingMutationCountMask) != 0);
}

class BackingMutationScope final {
public:
	BackingMutationScope(): m_enabled(Common::UniformMetadata::Enabled()) {
		if (m_enabled) Advance(true);
	}
	~BackingMutationScope() {
		if (m_enabled) Advance(false);
	}
	KYTY_CLASS_NO_COPY(BackingMutationScope);
private:
	static void Advance(bool begin) noexcept {
		auto state = g_backing_mutation_state.load(std::memory_order_relaxed);
		for (;;) {
			const auto count = state & BackingMutationCountMask;
			if ((begin && count == BackingMutationCountMask) || (!begin && count == 0) ||
			    (state & ~BackingMutationCountMask) == (UINT64_MAX & ~BackingMutationCountMask)) {
				std::abort(); // Never reuse a proof generation, including on overflow.
			}
			const auto next = state + BackingMutationSequenceStep + (begin ? 1 : -uint64_t {1});
			if (g_backing_mutation_state.compare_exchange_weak(
			        state, next, std::memory_order_seq_cst, std::memory_order_relaxed)) return;
		}
	}
	bool m_enabled;
};

class GuestBackingStore {
public:
	enum class FailureReason {
		None,
		BackingUnavailable,
		PhysicalRangeOutOfBounds,
		FixedMapViewFailed,
		PlaceholderMapFailed,
		ProtectFailed,
		WrongHostAddress,
		AddressRangeUnavailable,
	};

	explicit GuestBackingStore(uint64_t size): m_size(size) {
		BackingMutationScope backing_mutation;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		m_handle = CreateFileMappingA(
		    INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE | SEC_COMMIT,
		    static_cast<DWORD>(m_size >> 32u), static_cast<DWORD>(m_size & 0xffffffffu), nullptr);
		if (m_handle == nullptr) {
			LOGF_COLOR(Log::Color::Red,
			           "\t direct-memory backing: CreateFileMapping failed: 0x%08" PRIx32 "\n",
			           static_cast<uint32_t>(GetLastError()));
			return;
		}
		m_backing_base = static_cast<uint8_t*>(
		    MapViewOfFile(m_handle, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, m_size));
		if (m_backing_base == nullptr) {
			LOGF_COLOR(Log::Color::Red,
			           "\t direct-memory backing: writable alias failed: 0x%08" PRIx32 "\n",
			           static_cast<uint32_t>(GetLastError()));
			CloseHandle(m_handle);
			m_handle = nullptr;
		}
#elif KYTY_PLATFORM == KYTY_PLATFORM_LINUX
#if defined(__APPLE__)
		{
			char shm_name[64];
			snprintf(shm_name, sizeof(shm_name), "/kyty-dm-%d-%llx", static_cast<int>(getpid()),
			         static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(this)));
			m_fd = shm_open(shm_name, O_RDWR | O_CREAT | O_EXCL, 0600);
			if (m_fd >= 0) {
				shm_unlink(shm_name);
			}
		}
#else
		m_fd = static_cast<int>(syscall(SYS_memfd_create, "KytyDirectMemory", 0));
#endif
		if (m_fd < 0) {
			const int error = errno;
#if defined(__ANDROID__)
			std::fprintf(stderr, "PS5_ANDROID_VM_BACKING_FAILURE operation=memfd_create bytes=%" PRIu64 " errno=%d\n",
			             m_size, error);
#endif
			LOGF_COLOR(Log::Color::Red, "\t direct-memory backing: shared fd creation failed: %d\n",
			           error);
			return;
		}
		if (ftruncate(m_fd, static_cast<off_t>(m_size)) != 0) {
			const int error = errno;
#if defined(__ANDROID__)
			std::fprintf(stderr, "PS5_ANDROID_VM_BACKING_FAILURE operation=ftruncate bytes=%" PRIu64 " errno=%d\n",
			             m_size, error);
#endif
			LOGF_COLOR(Log::Color::Red, "\t direct-memory backing: ftruncate failed: %d\n", error);
			close(m_fd);
			m_fd = -1;
			return;
		}
		auto* backing = mmap(nullptr, m_size, PROT_READ | PROT_WRITE, MAP_SHARED, m_fd, 0);
		if (backing == MAP_FAILED) {
			const int error = errno;
#if defined(__ANDROID__)
			std::fprintf(stderr, "PS5_ANDROID_VM_BACKING_FAILURE operation=mmap_alias bytes=%" PRIu64 " errno=%d\n",
			             m_size, error);
#endif
			LOGF_COLOR(Log::Color::Red, "\t direct-memory backing: writable alias failed: %d\n",
			           error);
			close(m_fd);
			m_fd = -1;
			return;
		}
		m_backing_base = static_cast<uint8_t*>(backing);
#endif
	}

	~GuestBackingStore() {
		BackingMutationScope backing_mutation;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		for (const auto& [_, entry]: m_maps) {
			UnmapViewPreservePlaceholder(reinterpret_cast<void*>(entry.vaddr), entry.size);
		}
		m_maps.clear();
		if (m_backing_base != nullptr) {
			UnmapViewOfFile(m_backing_base);
		}
		if (m_handle != nullptr) {
			CloseHandle(m_handle);
		}
#elif KYTY_PLATFORM == KYTY_PLATFORM_LINUX
		for (const auto& [_, entry]: m_maps) {
			MEMORY_REQUIRE(
			    !UnmapViewPreservePlaceholder(reinterpret_cast<void*>(entry.vaddr), entry.size));
		}
		m_maps.clear();
		if (m_backing_base != nullptr) {
			munmap(m_backing_base, m_size);
		}
		if (m_fd >= 0) {
			close(m_fd);
		}
#endif
	}

	KYTY_CLASS_NO_COPY(GuestBackingStore);

	static const char* GetFailureReasonName(FailureReason reason) {
		switch (reason) {
			case FailureReason::None: return "n/a";
			case FailureReason::BackingUnavailable: return "backing-unavailable";
			case FailureReason::PhysicalRangeOutOfBounds: return "physical-range-out-of-bounds";
			case FailureReason::FixedMapViewFailed: return "fixed-map-view-failed";
			case FailureReason::PlaceholderMapFailed: return "placeholder-map-failed";
			case FailureReason::ProtectFailed: return "protect-failed";
			case FailureReason::WrongHostAddress: return "wrong-host-address";
			case FailureReason::AddressRangeUnavailable: return "address-range-unavailable";
			default: return "unknown";
		}
	}

	[[nodiscard]] bool IsAvailable() const {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		return m_handle != nullptr && m_backing_base != nullptr;
#elif KYTY_PLATFORM == KYTY_PLATFORM_LINUX
		return m_fd >= 0 && m_backing_base != nullptr;
#else
		return false;
#endif
	}

	[[nodiscard]] uint64_t GetBackingBase() const {
		return reinterpret_cast<uint64_t>(m_backing_base);
	}

	[[nodiscard]] uint64_t GetBackingSize() const { return m_size; }

	bool ZeroRange(uint64_t backing_offset, uint64_t size) {
		BackingMutationScope backing_mutation;
		if (!IsAvailable() || !RangeOk(backing_offset, size)) {
			return false;
		}
		std::lock_guard lock(m_mutex);
		std::memset(m_backing_base + backing_offset, 0, size);
		return true;
	}

	bool TryWriteBacking(uint64_t vaddr, const void* data, uint64_t size) {
		BackingMutationScope backing_mutation;
		if (data == nullptr) {
			return false;
		}
		const auto* source = static_cast<const uint8_t*>(data);
		return TryTransferBacking<false>(vaddr, size,
		                          [source](uint8_t* backing, uint64_t data_offset, size_t bytes) {
			                          std::memcpy(backing, source + data_offset, bytes);
		                          });
	}

	bool TryReadBacking(uint64_t vaddr, void* data, uint64_t size) {
		if (data == nullptr) {
			return false;
		}
		auto* destination = static_cast<uint8_t*>(data);
		return TryTransferBacking<true>(
		    vaddr, size, [destination](uint8_t* backing, uint64_t data_offset, size_t bytes) {
			    std::memcpy(destination + data_offset, backing, bytes);
		    });
	}

	bool TryEqualBacking(uint64_t vaddr, const void* data, uint64_t size) {
		if (data == nullptr) return false;
		const auto* expected = static_cast<const uint8_t*>(data);
		bool equal = true;
		const bool complete = TryTransferBacking<true>(vaddr, size,
		    [&](uint8_t* backing, uint64_t offset, size_t bytes) {
			    if (equal) equal = std::memcmp(backing, expected + offset, bytes) == 0;
		    });
		return complete && equal;
	}

	bool TryHashContiguousBacking(uint64_t vaddr, uint64_t size, uint64_t* hash) {
		if (!IsAvailable() || hash == nullptr || size == 0 || size > SIZE_MAX ||
		    UINT64_MAX - vaddr < size) return false;
		std::shared_lock lock(m_mutex);
		auto mapping = m_maps.upper_bound(vaddr);
		if (mapping == m_maps.begin()) return false;
		--mapping;
		const auto& entry = mapping->second;
		const auto offset = vaddr - entry.vaddr;
		if (offset >= entry.size || size > entry.size - offset ||
		    UINT64_MAX - entry.backing_offset < offset ||
		    !RangeOk(entry.backing_offset + offset, size)) return false;
		*hash = XXH3_64bits(m_backing_base + entry.backing_offset + offset,
		                    static_cast<size_t>(size));
		return true;
	}

	bool TryReadSparseBacking(uint64_t vaddr, void* data, uint64_t size) {
		if (!IsAvailable() || data == nullptr || size == 0 || UINT64_MAX - vaddr < size) {
			return false;
		}

		std::shared_lock lock(m_mutex);
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
		if (g_test_backing_read != nullptr) {
			g_test_backing_read(vaddr, size);
		}
#endif
		const auto end     = vaddr + size;
		auto       mapping = m_maps.lower_bound(vaddr);
		if (mapping != m_maps.begin()) {
			auto previous = std::prev(mapping);
			if (previous->second.vaddr + previous->second.size > vaddr) {
				mapping = previous;
			}
		}

		auto* destination = static_cast<uint8_t*>(data);
		auto  cursor      = vaddr;
		for (; mapping != m_maps.end() && mapping->second.vaddr < end; ++mapping) {
			const auto& entry  = mapping->second;
			const auto  start  = std::max(vaddr, entry.vaddr);
			const auto  bytes  = std::min(end, entry.vaddr + entry.size) - start;
			const auto  offset = entry.backing_offset + start - entry.vaddr;
			if (cursor < start) {
				std::memset(destination + (cursor - vaddr), 0, start - cursor);
			}
			std::memcpy(destination + (start - vaddr), m_backing_base + offset, bytes);
			cursor = start + bytes;
		}
		if (cursor < end) {
			std::memset(destination + (cursor - vaddr), 0, end - cursor);
		}
		return true;
	}

	bool MapExistingPlaceholderFixed(uint64_t vaddr, uint64_t size, uint64_t backing_offset,
	                                 VirtualMemory::Mode mode,
	                                 FailureReason*      failure_reason = nullptr) {
		BackingMutationScope backing_mutation;
		if (!IsAvailable() || !RangeOk(backing_offset, size)) {
			SetFailure(failure_reason, !IsAvailable() ? FailureReason::BackingUnavailable
			                                          : FailureReason::PhysicalRangeOutOfBounds);
			return false;
		}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		void* mapped = ReplacePlaceholderView(vaddr, size, backing_offset, mode, failure_reason);
		if (mapped == nullptr || reinterpret_cast<uint64_t>(mapped) != vaddr) {
			if (mapped != nullptr) {
				UnmapViewPreservePlaceholder(mapped, size);
				SetFailure(failure_reason, FailureReason::WrongHostAddress);
			}
			return false;
		}

		{
			std::lock_guard lock(m_mutex);
			MEMORY_REQUIRE(!m_maps.emplace(vaddr, MapEntry {vaddr, size, backing_offset, mode}).second);
		}
		SetFailure(failure_reason, FailureReason::None);
		return true;
#else
		void* mapped = MapView(vaddr, size, backing_offset, mode, failure_reason);
		if (mapped == nullptr || reinterpret_cast<uint64_t>(mapped) != vaddr) {
			return false;
		}
		{
			std::lock_guard lock(m_mutex);
			MEMORY_REQUIRE(!m_maps.emplace(vaddr, MapEntry {vaddr, size, backing_offset, mode}).second);
		}
		SetFailure(failure_reason, FailureReason::None);
		return true;
#endif
	}

	bool Unmap(uint64_t vaddr, uint64_t size, bool* placeholder_preserved = nullptr) {
		BackingMutationScope backing_mutation;
		if (size == 0 || UINT64_MAX - vaddr < size) {
			return false;
		}

		if (placeholder_preserved != nullptr) {
			*placeholder_preserved = false;
		}

		const auto            end = vaddr + size;
		std::vector<MapEntry> targets;
		{
			std::lock_guard lock(m_mutex);
			auto            current = vaddr;
			while (current < end) {
				MapEntry entry {};
				if (!FindContainingUnlocked(current, 1, &entry)) {
					return false;
				}
				const auto part_size = std::min(end, entry.vaddr + entry.size) - current;
				targets.push_back(
				    {current, part_size, entry.backing_offset + current - entry.vaddr, entry.mode});
				current += part_size;
			}
		}

		std::vector<MapEntry> removed;
		removed.reserve(targets.size());
		for (const auto& entry: targets) {
			bool part_placeholder_preserved = false;
			if (!UnmapOne(entry.vaddr, entry.size, &part_placeholder_preserved)) {
				for (auto it = removed.rbegin(); it != removed.rend(); ++it) {
					MEMORY_REQUIRE(!MapExistingPlaceholderFixed(it->vaddr, it->size, it->backing_offset,
					                                     it->mode));
				}
				if (placeholder_preserved != nullptr) {
					*placeholder_preserved = false;
				}
				return false;
			}
			MEMORY_REQUIRE(!part_placeholder_preserved);
			removed.push_back(entry);
		}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		// Each unmapped file view becomes a separate Windows placeholder. The address-space owner
		// records this contiguous guest span as one free range, so normalize fragmented backing
		// views into one placeholder before publishing it there.
		if (removed.size() > 1 && !CoalescePlaceholders(vaddr, size)) {
			for (auto it = removed.rbegin(); it != removed.rend(); ++it) {
				MEMORY_REQUIRE(!MapExistingPlaceholderFixed(it->vaddr, it->size, it->backing_offset,
				                                     it->mode));
			}
			return false;
		}
#endif

		if (placeholder_preserved != nullptr) {
			*placeholder_preserved = true;
		}
		return true;
	}

	bool UnmapOne(uint64_t vaddr, uint64_t size, bool* placeholder_preserved = nullptr) {
		BackingMutationScope backing_mutation;
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
		if (g_test_backing_store_unmaps_before_failure == 0) {
			g_test_backing_store_unmaps_before_failure = UINT32_MAX;
			return false;
		}
		if (g_test_backing_store_unmaps_before_failure != UINT32_MAX) {
			g_test_backing_store_unmaps_before_failure--;
		}
#endif

		MapEntry old {};
		{
			std::lock_guard lock(m_mutex);
			auto            next = m_maps.upper_bound(vaddr);
			if (next != m_maps.begin()) {
				auto entry = std::prev(next);
				if (vaddr + size <= entry->second.vaddr + entry->second.size) {
					old = entry->second;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
					m_maps.erase(entry);
#endif
				}
			}
		}

		if (old.size == 0) {
			return false;
		}

		const uint64_t left_size  = vaddr - old.vaddr;
		const uint64_t right_addr = vaddr + size;
		const uint64_t old_end    = old.vaddr + old.size;
		const uint64_t right_size = old_end - right_addr;

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		const bool exact_unmap = (vaddr == old.vaddr && size == old.size);
		if (exact_unmap) {
			const bool unmapped =
			    UnmapViewPreservePlaceholder(reinterpret_cast<void*>(old.vaddr), old.size);
			if (!unmapped) {
				std::lock_guard lock(m_mutex);
				m_maps.emplace(old.vaddr, old);
				return false;
			}
			if (placeholder_preserved != nullptr) {
				*placeholder_preserved = true;
			}
			return true;
		}

		if (UnmapViewPreservePlaceholder(reinterpret_cast<void*>(old.vaddr), old.size)) {
			bool ok = true;
			if (left_size != 0 && left_size != old.size) {
				ok = SplitPlaceholder(old.vaddr, left_size) && ok;
			}
			if (right_size != 0) {
				ok = SplitPlaceholder(vaddr, size) && ok;
			}
			if (left_size != 0) {
				ok = MapExistingPlaceholderFixed(old.vaddr, left_size, old.backing_offset,
				                                 old.mode) &&
				     ok;
			}
			if (right_size != 0) {
				ok = MapExistingPlaceholderFixed(right_addr, right_size,
				                                 old.backing_offset + (right_addr - old.vaddr),
				                                 old.mode) &&
				     ok;
			}
			if (ok) {
				if (placeholder_preserved != nullptr) {
					*placeholder_preserved = true;
				}
				return true;
			}

			if (left_size != 0 && Contains(old.vaddr, left_size)) {
				MEMORY_REQUIRE(!UnmapOne(old.vaddr, left_size));
			}
			if (right_size != 0 && Contains(right_addr, right_size)) {
				MEMORY_REQUIRE(!UnmapOne(right_addr, right_size));
			}
			MEMORY_REQUIRE(VirtualFree(reinterpret_cast<void*>(old.vaddr), old.size,
			                    MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS) == 0);
			MEMORY_REQUIRE(
			    !MapExistingPlaceholderFixed(old.vaddr, old.size, old.backing_offset, old.mode));
			return false;
		}
		std::lock_guard lock(m_mutex);
		m_maps.emplace(old.vaddr, old);
		return false;
#else
		// Replace only the requested range so concurrent accesses and GPU watch
		// protections on the surviving mappings remain intact.
		if (!UnmapViewPreservePlaceholder(reinterpret_cast<void*>(vaddr), size)) {
			return false;
		}

		std::lock_guard lock(m_mutex);
		m_maps.erase(old.vaddr);
		if (left_size != 0) {
			m_maps.emplace(old.vaddr,
			               MapEntry {old.vaddr, left_size, old.backing_offset, old.mode});
		}
		if (right_size != 0) {
			m_maps.emplace(right_addr,
			               MapEntry {right_addr, right_size,
			                         old.backing_offset + right_addr - old.vaddr, old.mode});
		}

		if (placeholder_preserved != nullptr) {
			*placeholder_preserved = true;
		}
		return true;
#endif
	}

	// Snapshot only: callers retain the normal guest/GPU synchronization and
	// backing lifetime rules. Do not acquire the outer VM operation mutex here.
	bool QueryUniqueBackingGeneration(uint64_t vaddr, uint64_t size, uint64_t* generation) {
		if (generation == nullptr) return false;
		*generation = 0;
		if (!Common::UniformMetadata::Enabled() || !IsAvailable() || vaddr == 0 || size == 0 ||
		    size > UINT64_MAX - vaddr) return false;
		std::shared_lock lock(m_mutex);
		const auto before = ReadBackingMutationGeneration();
		if (before == 0 || (before & 1) != 0) return false;
		MapEntry selected {};
		if (!FindContainingUnlocked(vaddr, size, &selected)) return false;
		const auto offset = vaddr - selected.vaddr;
		if (offset > UINT64_MAX - selected.backing_offset) return false;
		const auto physical = selected.backing_offset + offset;
		if (!RangeOk(physical, size)) return false;
		for (const auto& [_, alias]: m_maps) {
			if (alias.vaddr == selected.vaddr) continue;
			if (!RangeOk(alias.backing_offset, alias.size) ||
			    (physical < alias.backing_offset + alias.size &&
			     alias.backing_offset < physical + size)) return false;
		}
		if (ReadBackingMutationGeneration() != before) return false;
		*generation = before;
		return true;
	}

	bool Contains(uint64_t vaddr, uint64_t size) {
		if (size == 0 || UINT64_MAX - vaddr < size) {
			return false;
		}

		std::shared_lock lock(m_mutex);
		const auto      end     = vaddr + size;
		auto            current = vaddr;

		while (current < end) {
			MapEntry entry {};
			if (!FindContainingUnlocked(current, 1, &entry)) {
				return false;
			}
			const auto entry_end = entry.vaddr + entry.size;
			if (entry_end <= current) {
				return false;
			}
			current = std::min(end, entry_end);
		}

		return true;
	}

	bool QueryContiguousBackingOffset(uint64_t vaddr, uint64_t size, uint64_t* offset) {
		if (offset == nullptr || !IsAvailable() || vaddr == 0 || size == 0 ||
		    size > UINT64_MAX - vaddr) return false;
		std::shared_lock lock(m_mutex);
		auto current = vaddr;
		uint64_t first = 0;
		while (current < vaddr + size) {
			MapEntry entry {};
			if (!FindContainingUnlocked(current, 1, &entry)) return false;
			const auto bytes = std::min(vaddr + size, entry.vaddr + entry.size) - current;
			if (entry.backing_offset > UINT64_MAX - (current - entry.vaddr)) return false;
			const auto physical = entry.backing_offset + current - entry.vaddr;
			if (!RangeOk(physical, bytes)) return false;
			if (current == vaddr) first = physical;
			else if (current - vaddr > UINT64_MAX - first || physical != first + current - vaddr) return false;
			current += bytes;
		}
		*offset = first;
		return true;
	}

private:
	template <bool ReadOnly, typename Transfer>
	bool TryTransferBacking(uint64_t vaddr, uint64_t size, Transfer transfer) {
		if (!IsAvailable() || size == 0 || UINT64_MAX - vaddr < size) {
			return false;
		}

		// Backing-map locks permit parallel readers while writes retain atomic
		// preflight. Callers still stabilize guest CPU stores through the existing
		// publication contract; this lock does not replace the FEX execution epoch.
		using Lock = std::conditional_t<ReadOnly, std::shared_lock<std::shared_mutex>,
		                                std::unique_lock<std::shared_mutex>>;
		Lock lock(m_mutex);
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
		if constexpr (ReadOnly) {
			if (g_test_backing_read != nullptr) g_test_backing_read(vaddr, size);
		}
#endif
		const auto end = vaddr + size;
		auto first = m_maps.upper_bound(vaddr);
		if (first == m_maps.begin()) return false;
		--first;
		auto current = vaddr;
		for (auto mapping = first; current < end; ++mapping) {
			if (mapping == m_maps.end() || current < mapping->second.vaddr ||
			    current >= mapping->second.vaddr + mapping->second.size) return false;
			const auto& entry = mapping->second;
			const auto bytes = std::min(end, entry.vaddr + entry.size) - current;
			if (!RangeOk(entry.backing_offset + current - entry.vaddr, bytes)) return false;
			current += bytes;
		}

#if defined(KYTY_BACKING_MUTATION_TESTS)
		if constexpr (!ReadOnly) {
			if (g_test_backing_write_phase != nullptr) g_test_backing_write_phase(false);
		}
#endif
		current = vaddr;
		for (auto mapping = first; current < end; ++mapping) {
			const auto& entry = mapping->second;
			const auto bytes = std::min(end, entry.vaddr + entry.size) - current;
			const auto offset = entry.backing_offset + current - entry.vaddr;
			transfer(m_backing_base + offset, current - vaddr, static_cast<size_t>(bytes));
			current += bytes;
		}
#if defined(KYTY_BACKING_MUTATION_TESTS)
		if constexpr (!ReadOnly) {
			if (g_test_backing_write_phase != nullptr) g_test_backing_write_phase(true);
		}
#endif
		return true;
	}

	struct MapEntry {
		uint64_t            vaddr          = 0;
		uint64_t            size           = 0;
		uint64_t            backing_offset = 0;
		VirtualMemory::Mode mode           = VirtualMemory::Mode::NoAccess;
	};

	bool FindContainingUnlocked(uint64_t vaddr, uint64_t size, MapEntry* out) const {
		MEMORY_REQUIRE(out == nullptr);

		if (size == 0 || UINT64_MAX - vaddr < size) {
			return false;
		}

		auto next = m_maps.upper_bound(vaddr);
		if (next == m_maps.begin()) {
			return false;
		}
		const auto& entry = std::prev(next)->second;
		if (vaddr + size <= entry.vaddr + entry.size) {
			*out = entry;
			return true;
		}

		return false;
	}

	[[nodiscard]] bool RangeOk(uint64_t backing_offset, uint64_t size) const {
		return size != 0 && backing_offset < m_size && size <= m_size - backing_offset;
	}

	static void SetFailure(FailureReason* out, FailureReason reason) {
		if (out != nullptr) {
			*out = reason;
		}
	}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	static DWORD GetProtect(VirtualMemory::Mode mode) {
		switch (mode) {
			case VirtualMemory::Mode::Read: return PAGE_READONLY;
			case VirtualMemory::Mode::Write:
			case VirtualMemory::Mode::ReadWrite: return PAGE_READWRITE;
			case VirtualMemory::Mode::Execute: return PAGE_EXECUTE;
			case VirtualMemory::Mode::ExecuteRead: return PAGE_EXECUTE_READ;
			case VirtualMemory::Mode::ExecuteWrite:
			case VirtualMemory::Mode::ExecuteReadWrite: return PAGE_EXECUTE_READWRITE;
			case VirtualMemory::Mode::NoAccess:
			default: return PAGE_NOACCESS;
		}
	}

	static bool ApplyProtection(void* ptr, uint64_t size, VirtualMemory::Mode mode) {
		DWORD old_protect = 0;
		return VirtualProtect(ptr, size, GetProtect(mode), &old_protect) != 0;
	}

	static bool SplitPlaceholder(uint64_t vaddr, uint64_t size) {
		if (size == 0) {
			return true;
		}
		return VirtualFree(reinterpret_cast<void*>(vaddr), size,
		                   MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) != 0;
	}

	void* ReplacePlaceholderView(uint64_t vaddr, uint64_t size, uint64_t backing_offset,
	                             VirtualMemory::Mode mode, FailureReason* failure_reason) {
		const DWORD map_protect =
		    mode == VirtualMemory::Mode::NoAccess ? PAGE_READWRITE : GetProtect(mode);
		void* ptr =
		    MapViewOfFile3(m_handle, GetCurrentProcess(), reinterpret_cast<void*>(vaddr),
		                   backing_offset, size, MEM_REPLACE_PLACEHOLDER, map_protect, nullptr, 0);
		if (ptr == nullptr || reinterpret_cast<uint64_t>(ptr) != vaddr) {
			if (ptr != nullptr) {
				UnmapViewOfFile2(GetCurrentProcess(), ptr, MEM_PRESERVE_PLACEHOLDER);
			}
			SetFailure(failure_reason, FailureReason::PlaceholderMapFailed);
			return nullptr;
		}

		if (!ApplyProtection(ptr, size, mode)) {
			UnmapViewOfFile2(GetCurrentProcess(), ptr, MEM_PRESERVE_PLACEHOLDER);
			SetFailure(failure_reason, FailureReason::ProtectFailed);
			return nullptr;
		}
		return ptr;
	}

	static bool UnmapViewPreservePlaceholder(void* vaddr, uint64_t /*size*/) {
		return UnmapViewOfFile2(GetCurrentProcess(), vaddr, MEM_PRESERVE_PLACEHOLDER) != 0;
	}

	HANDLE m_handle = nullptr;
#elif KYTY_PLATFORM == KYTY_PLATFORM_LINUX
	[[nodiscard]] static uint64_t GetMappableTestSize() { return 0x4000; }

	static int GetProtect(VirtualMemory::Mode mode) {
		switch (mode) {
			case VirtualMemory::Mode::Read: return PROT_READ;
			case VirtualMemory::Mode::Write:
			case VirtualMemory::Mode::ReadWrite: return PROT_READ | PROT_WRITE;
			case VirtualMemory::Mode::Execute: return PROT_EXEC;
			case VirtualMemory::Mode::ExecuteRead: return PROT_EXEC | PROT_READ;
			case VirtualMemory::Mode::ExecuteWrite:
			case VirtualMemory::Mode::ExecuteReadWrite: return PROT_EXEC | PROT_READ | PROT_WRITE;
			case VirtualMemory::Mode::NoAccess:
			default: return PROT_NONE;
		}
	}

	void* MapView(uint64_t vaddr, uint64_t size, uint64_t backing_offset, VirtualMemory::Mode mode,
	              FailureReason* failure_reason) {
		void* ptr = mmap(reinterpret_cast<void*>(vaddr), size, GetProtect(mode),
		                 MAP_SHARED | MAP_FIXED, m_fd, static_cast<off_t>(backing_offset));
		if (ptr == MAP_FAILED) {
			SetFailure(failure_reason, FailureReason::FixedMapViewFailed);
			return nullptr;
		}
		return ptr;
	}

	// Replace the file view atomically instead of exposing an unmapped hole in the
	// process-lifetime guest reservation.
	static bool UnmapViewPreservePlaceholder(void* vaddr, uint64_t size) {
		void* ptr = mmap(vaddr, size, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
		return ptr != MAP_FAILED && ptr == vaddr;
	}

	int m_fd = -1;
#endif

	uint8_t*                     m_backing_base = nullptr;
	uint64_t                     m_size         = 0;
	std::shared_mutex            m_mutex;
	std::map<uint64_t, MapEntry> m_maps;
};

class GuestAddressSpace {
public:
	explicit GuestAddressSpace(uint64_t backing_size) {
		BackingMutationScope backing_mutation;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		SYSTEM_INFO info {};
		GetSystemInfo(&info);
		m_granularity = info.dwAllocationGranularity;
#endif
		ReserveGuestRegions();
		m_backing = new GuestBackingStore(backing_size);
		if (!m_backing->IsAvailable()) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
			EXIT("could not reserve %" PRIu64 " MB for guest direct memory. Windows commits\n"
			     "this up front, so the paging file is usually what needs to be larger.\n",
			     backing_size / (1024U * 1024U));
#else
			EXIT("could not reserve %" PRIu64 " MB for guest direct memory\n",
			     backing_size / (1024U * 1024U));
#endif
		}
	}

	~GuestAddressSpace() {
		BackingMutationScope backing_mutation;
		delete m_backing;
		m_backing = nullptr;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		for (const auto& [start, size]: m_owned) {
			const auto end     = start + size;
			auto       current = start;
			while (current < end) {
				MEMORY_BASIC_INFORMATION info {};
				MEMORY_REQUIRE(VirtualQuery(reinterpret_cast<const void*>(current), &info, sizeof(info)) ==
				        0);
				const auto region_start = reinterpret_cast<uint64_t>(info.BaseAddress);
				const auto region_end   = region_start + static_cast<uint64_t>(info.RegionSize);
				if (info.State == MEM_COMMIT) {
					MEMORY_REQUIRE(VirtualFree(reinterpret_cast<void*>(region_start), info.RegionSize,
					                    MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) == 0);
					continue;
				}
				if (info.State == MEM_RESERVE) {
					MEMORY_REQUIRE(VirtualFree(info.AllocationBase, 0, MEM_RELEASE) == 0);
				}
				current = region_end;
			}
		}
#else
		for (const auto& [start, size]: m_owned) {
			munmap(reinterpret_cast<void*>(start), size);
		}
#endif
	}

	KYTY_CLASS_NO_COPY(GuestAddressSpace);

	[[nodiscard]] uint64_t GetGranularity() const { return m_granularity; }
	[[nodiscard]] uint64_t GetBackingBase() const { return m_backing->GetBackingBase(); }
	[[nodiscard]] uint64_t GetBackingSize() const { return m_backing->GetBackingSize(); }

	bool QueryUniqueBackingGeneration(uint64_t vaddr, uint64_t size, uint64_t* generation) {
		return m_backing->QueryUniqueBackingGeneration(vaddr, size, generation);
	}
	bool QueryContiguousBackingOffset(uint64_t vaddr, uint64_t size, uint64_t* offset) {
		return m_backing->QueryContiguousBackingOffset(vaddr, size, offset);
	}

	bool ZeroBacking(uint64_t backing_offset, uint64_t size) {
		return m_backing->ZeroRange(backing_offset, size);
	}

	[[nodiscard]] bool Owns(uint64_t vaddr, uint64_t size) {
		if (size == 0 || UINT64_MAX - vaddr < size) {
			return false;
		}
		std::shared_lock lock(m_mutex);
		return std::any_of(m_owned.begin(), m_owned.end(), [vaddr, size](const auto& range) {
			return vaddr >= range.first && vaddr + size <= range.first + range.second;
		});
	}

	[[nodiscard]] bool OverlapsOwned(uint64_t vaddr, uint64_t size) {
		if (size == 0 || UINT64_MAX - vaddr < size) {
			return false;
		}
		std::shared_lock lock(m_mutex);
		return std::any_of(m_owned.begin(), m_owned.end(), [vaddr, size](const auto& range) {
			return VirtualRangesOverlap(vaddr, size, range.first, range.second);
		});
	}

	bool TryWriteBacking(uint64_t vaddr, const void* data, uint64_t size) {
		return m_backing->TryWriteBacking(vaddr, data, size);
	}

	bool TryReadBacking(uint64_t vaddr, void* data, uint64_t size) {
		return m_backing->TryReadBacking(vaddr, data, size);
	}

	bool TryEqualBacking(uint64_t vaddr, const void* data, uint64_t size) {
		return m_backing->TryEqualBacking(vaddr, data, size);
	}

	bool TryReadSparseBacking(uint64_t vaddr, void* data, uint64_t size) {
		if (!Owns(vaddr, size)) {
			return false;
		}
		std::shared_lock lock(m_mutex);
		auto            mapping = m_mapped.upper_bound(vaddr);
		if (mapping != m_mapped.begin()) {
			--mapping;
		}
		for (; mapping != m_mapped.end() && mapping->second.vaddr < vaddr + size; ++mapping) {
			const auto& region = mapping->second;
			if (region.type == MappingType::Private && region.vaddr + region.size > vaddr) {
				return false;
			}
		}
		return m_backing->TryReadSparseBacking(vaddr, data, size);
	}

	bool TryHashContiguousBacking(uint64_t vaddr, uint64_t size, uint64_t* hash) {
		if (hash == nullptr || size == 0 || size > SIZE_MAX || UINT64_MAX - vaddr < size ||
		    !Owns(vaddr, size)) return false;
		// Match sparse-read lock ordering. A complete single shared view is a
		// narrower admission than that reader; native mapping splits fall back.
		std::shared_lock lock(m_mutex);
		auto mapping = m_mapped.upper_bound(vaddr);
		if (mapping == m_mapped.begin()) return false;
		--mapping;
		const auto& region = mapping->second;
		const auto offset = vaddr - region.vaddr;
		if (region.type != MappingType::Backing || !region.contiguous_hash_allowed || offset >= region.size ||
		    size > region.size - offset) return false;
		return m_backing->TryHashContiguousBacking(vaddr, size, hash);
	}

	bool BackingContains(uint64_t vaddr, uint64_t size) { return m_backing->Contains(vaddr, size); }
	bool PrivateContains(uint64_t vaddr, uint64_t size) {
		std::shared_lock lock(m_mutex);
		return ContainsMappedUnlocked(vaddr, size, MappingType::Private);
	}

	struct MappingIdentity {
		uint64_t address;
		uint64_t size;
		uint64_t identity;
		uint64_t offset;
		bool private_memory;
	};

	bool QueryMappingIdentities(uint64_t vaddr, uint64_t size,
	                            std::vector<MappingIdentity>* identities) {
		MEMORY_REQUIRE(identities == nullptr);
		identities->clear();
		if (vaddr == 0 || size == 0 || size > UINT64_MAX - vaddr) return false;
		std::lock_guard lock(m_mutex);
		const auto end = vaddr + size;
		auto current = vaddr;
		while (current < end) {
			const auto it = FindMappedContainingUnlocked(current);
			if (it == m_mapped.end() || it->second.identity == 0) {
				identities->clear();
				return false;
			}
			const auto& region = it->second;
			const auto bytes = std::min(end, region.vaddr + region.size) - current;
			identities->push_back({current, bytes, region.identity, current - region.origin,
			                       region.type == MappingType::Private});
			current += bytes;
		}
		return true;
	}

	bool Protect(uint64_t vaddr, uint64_t size, VirtualMemory::Mode mode) {
		BackingMutationScope backing_mutation;
		RecordGuestMemoryMutation(vaddr, size);
		if (size == 0 || UINT64_MAX - vaddr < size) {
			return false;
		}
		std::lock_guard lock(m_mutex);
		return ProtectMappedUnlocked(vaddr, size, mode);
	}

	bool ProtectTransient(uint64_t vaddr, uint64_t size, VirtualMemory::Mode mode) {
		RecordGuestMemoryMutation(vaddr, size);
		if (size == 0 || UINT64_MAX - vaddr < size) {
			return false;
		}

		std::lock_guard lock(m_mutex);
		return ProtectMappedUnlocked(vaddr, size, mode);
	}

	bool MapBacking(uint64_t vaddr, uint64_t size, uint64_t backing_offset,
	                VirtualMemory::Mode               mode,
	                GuestBackingStore::FailureReason* failure_reason = nullptr,
	                bool contiguous_hash_allowed = false) {
		BackingMutationScope backing_mutation;
		RecordGuestMemoryMutation(vaddr, size);
		std::lock_guard lock(m_mutex);
		if (!ConsumeUnlocked(vaddr, size)) {
			if (failure_reason != nullptr) {
				*failure_reason = GuestBackingStore::FailureReason::AddressRangeUnavailable;
			}
			return false;
		}
		const auto host_mode =
#if defined(LUMEN_PS5_FEXCORE)
		    GuestHostProtection(mode);
#else
		    GuestHostProtection(VirtualMemory::IsExecute(mode)
		                           ? VirtualMemory::Mode::ExecuteReadWrite
		                           : VirtualMemory::Mode::ReadWrite);
#endif
		if (m_backing->MapExistingPlaceholderFixed(vaddr, size, backing_offset, host_mode,
		                                           failure_reason)) {
			MEMORY_REQUIRE(!AddMappedUnlocked(vaddr, size, MappingType::Backing, contiguous_hash_allowed));
			return true;
		}
		AddFreeUnlocked(vaddr, size);
		return false;
	}

	bool UnmapBacking(uint64_t vaddr, uint64_t size) {
		BackingMutationScope backing_mutation;
		RecordGuestMemoryMutation(vaddr, size);
		std::lock_guard lock(m_mutex);
		if (!ContainsMappedUnlocked(vaddr, size, MappingType::Backing)) {
			return false;
		}
		bool placeholder_preserved = false;
		if (!m_backing->Unmap(vaddr, size, &placeholder_preserved)) {
			return false;
		}
		if (!placeholder_preserved) {
			return false;
		}
		MEMORY_REQUIRE(!RemoveMappedUnlocked(vaddr, size, MappingType::Backing));
		AddFreeUnlocked(vaddr, size);
		return true;
	}

	uint64_t ReserveAligned(uint64_t search_addr, uint64_t size, uint64_t alignment) {
		if (size == 0) {
			return 0;
		}
		alignment = std::max<uint64_t>(alignment != 0 ? alignment : PageSize(), PageSize());
		std::lock_guard lock(m_mutex);
		if (search_addr == 0) {
			auto addr = FindFreeAlignedUnlocked(HOST_SYSTEM_MANAGED_MIN,
			                                    HOST_SYSTEM_MANAGED_MAX + 1u, size, alignment);
			return addr != 0 ? addr
			                 : FindFreeAlignedUnlocked(HOST_USER_MIN, HOST_USER_MAX + 1u, size,
			                                           alignment);
		}
		return FindFreeAlignedUnlocked(search_addr, HOST_USER_MAX + 1u, size, alignment);
	}

	uint64_t FindFreeAligned(uint64_t search_start, uint64_t search_end, uint64_t size,
	                         uint64_t alignment) {
		if (size == 0 || alignment == 0) {
			return 0;
		}
		alignment = std::max<uint64_t>(alignment, PageSize());
		std::lock_guard lock(m_mutex);
		return FindFreeAlignedUnlocked(search_start, search_end, size, alignment);
	}

	// Pure lookup: never acquire a lazy Android placeholder before admission.
	uint64_t FindOwnedFreeAligned(uint64_t search_start, uint64_t search_end, uint64_t size,
	                         uint64_t alignment) {
		if (size == 0 || alignment == 0) {
			return 0;
		}
		alignment = std::max<uint64_t>(alignment, PageSize());
		std::lock_guard lock(m_mutex);
		return FindFreeAlignedUnlocked(search_start, search_end, size, alignment, false);
	}

	bool ReserveFixed(uint64_t vaddr, uint64_t size) {
		if (vaddr == 0 || size == 0 || !IsPageAligned(vaddr) || !IsPageAligned(size)) {
			return false;
		}
		std::lock_guard lock(m_mutex);
		if (FindContaining(vaddr, size) != m_free.end()) return true;
#if defined(__ANDROID__)
		return AcquirePlaceholderUnlocked(vaddr, size);
#else
		return false;
#endif
	}

#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
	bool TestContainsFree(uint64_t vaddr, uint64_t size) {
		std::lock_guard lock(m_mutex);
		return FindContaining(vaddr, size) != m_free.end();
	}
#endif

	bool ReleaseFree(uint64_t vaddr, uint64_t size) {
		std::lock_guard lock(m_mutex);
		return FindContaining(vaddr, size) != m_free.end();
	}

	bool Commit(uint64_t vaddr, uint64_t size, VirtualMemory::Mode mode) {
		BackingMutationScope backing_mutation;
		RecordGuestMemoryMutation(vaddr, size);
		std::lock_guard lock(m_mutex);
		if (!ConsumeUnlocked(vaddr, size)) {
			return false;
		}
		const auto host_mode =
#if defined(LUMEN_PS5_FEXCORE)
		    GuestHostProtection(mode);
#else
		    GuestHostProtection(VirtualMemory::IsExecute(mode)
		                           ? VirtualMemory::Mode::ExecuteReadWrite
		                           : VirtualMemory::Mode::ReadWrite);
#endif

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		auto* ptr = VirtualAlloc2(GetCurrentProcess(), reinterpret_cast<void*>(vaddr), size,
		                          MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER,
		                          GetProtect(host_mode), nullptr, 0);
		if (ptr == nullptr || reinterpret_cast<uint64_t>(ptr) != vaddr) {
			if (ptr != nullptr) {
				VirtualFree(ptr, 0, MEM_RELEASE);
			}
			AddFreeUnlocked(vaddr, size);
			return false;
		}
		MEMORY_REQUIRE(!AddMappedUnlocked(vaddr, size, MappingType::Private));
		return true;
#else
		if (VirtualMemory::Protect(vaddr, size, host_mode)) {
			MEMORY_REQUIRE(!AddMappedUnlocked(vaddr, size, MappingType::Private));
			return true;
		}
		AddFreeUnlocked(vaddr, size);
		return false;
#endif
	}

	bool ReleaseCommitted(uint64_t vaddr, uint64_t size) {
		BackingMutationScope backing_mutation;
		RecordGuestMemoryMutation(vaddr, size);
		if (vaddr == 0 || size == 0 || !IsPageAligned(vaddr) || !IsPageAligned(size)) {
			return false;
		}
		std::lock_guard lock(m_mutex);
		if (!ContainsMappedUnlocked(vaddr, size, MappingType::Private)) {
			return false;
		}
		const auto end     = vaddr + size;
		auto       current = vaddr;
		std::vector<std::pair<uint64_t, uint64_t>> regions;
		while (current < end) {
			const auto it       = FindMappedContainingUnlocked(current);
			const auto part_end = std::min(end, it->second.vaddr + it->second.size);
			regions.emplace_back(current, part_end - current);
			current = part_end;
		}
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		for (const auto& [region_vaddr, region_size]: regions) {
			MEMORY_REQUIRE(VirtualFree(reinterpret_cast<void*>(region_vaddr), region_size,
			                    MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) == 0);
		}
		// ReleaseCommitted also publishes the aggregate span as one logical free range.
		MEMORY_REQUIRE(regions.size() > 1 && !CoalescePlaceholders(vaddr, size));
#else
		for (const auto& [region_vaddr, region_size]: regions) {
			RestorePlaceholder(region_vaddr, region_size);
		}
#endif
		MEMORY_REQUIRE(!RemoveMappedUnlocked(vaddr, size, MappingType::Private));
		AddFreeUnlocked(vaddr, size);
		return true;
	}

	bool SelfTest() {
		const auto addr = ReserveAligned(0, PageSize(), PageSize());
		if (addr == 0) {
			LOGF_COLOR(Log::Color::Red,
			           "\t guest address-space self-test: 16 KiB range unavailable\n");
			return false;
		}
		if (!Commit(addr, PageSize(), VirtualMemory::Mode::ReadWrite)) {
			LOGF_COLOR(Log::Color::Red, "\t guest address-space self-test: commit failed\n");
			return false;
		}

		auto* ptr = reinterpret_cast<uint64_t*>(addr);
		*ptr      = 0x4b59545950484f4cull; // "KYTYPHOL"

		if (!ReleaseCommitted(addr, PageSize())) {
			LOGF_COLOR(Log::Color::Red,
			           "\t guest address-space self-test: release-committed failed\n");
			return false;
		}

#if defined(__ANDROID__)
		// A real native map inside the console VA windows must remain native.
		// Test each acquisition path against it, including direct backing views.
		const auto native_size = PageSize() * 2;
		void* native = mmap(nullptr, native_size, PROT_READ | PROT_WRITE,
		                    MAP_PRIVATE | MAP_ANON, -1, 0);
		if (native == MAP_FAILED) {
			return false;
		}
		const auto collision = AlignUp(reinterpret_cast<uint64_t>(native), PageSize());
		const bool in_guest_window = IsGuestWindow(collision, PageSize());
		constexpr uint64_t native_value = 0x4e41544956455641ull;
		*reinterpret_cast<uint64_t*>(collision) = native_value;
		GuestBackingStore::FailureReason reason {};
		const bool denied = !Owns(collision, PageSize()) &&
		    !ReserveFixed(collision, PageSize()) &&
		    !Commit(collision, PageSize(), VirtualMemory::Mode::ReadWrite) &&
		    !MapBacking(collision, PageSize(), 0, VirtualMemory::Mode::ReadWrite, &reason) &&
		    reason == GuestBackingStore::FailureReason::AddressRangeUnavailable &&
		    *reinterpret_cast<uint64_t*>(collision) == native_value;
		MEMORY_REQUIRE(munmap(native, native_size) != 0);
		if (!denied) {
			return false;
		}
		std::printf("PS5_ANDROID_VM_NATIVE_COLLISION=%s in_guest_window=%u backing_size=%" PRIu64 "\n",
		            in_guest_window ? "PASS" : "OUTSIDE_GUEST_WINDOW", in_guest_window ? 1U : 0U,
		            GetBackingSize());
#endif

		LOGF_COLOR(Log::Color::Green, "\t guest address-space self-test: ok\n");
		return true;
	}

private:
	enum class MappingType { Backing, Private };

	struct MappedRegion {
		uint64_t    vaddr = 0;
		uint64_t    size  = 0;
		MappingType type  = MappingType::Private;
		uint64_t    identity = 0;
		uint64_t    origin = 0;
		// Explicitly enrolled Direct/Flexible views only. Private/Pooled and
		// future mapping kinds keep the byte-taking reader. Splits copy this tag.
		bool        contiguous_hash_allowed = false;
	};

	static constexpr uint64_t PageSize() { return 0x4000; }

	static uint64_t AlignUp(uint64_t value, uint64_t alignment) {
		if (alignment == 0) {
			return 0;
		}
		const auto remainder = value % alignment;
		const auto increment = (remainder != 0 ? alignment - remainder : 0);
		return increment <= UINT64_MAX - value ? value + increment : 0;
	}

	static uint64_t AlignDown(uint64_t value, uint64_t alignment) {
		return value & ~(alignment - 1u);
	}

	[[nodiscard]] static bool IsPageAligned(uint64_t value) {
		return (value & (PageSize() - 1u)) == 0;
	}

	[[nodiscard]] bool IsGranularityAligned(uint64_t value) const {
		return (value & (m_granularity - 1u)) == 0;
	}

	bool ProtectMappedUnlocked(uint64_t vaddr, uint64_t size, VirtualMemory::Mode mode) {
		const auto end = vaddr + size;
		auto       it  = m_mapped.upper_bound(vaddr);
		if (it != m_mapped.begin()) {
			--it;
		}
		for (; it != m_mapped.end() && it->second.vaddr < end; ++it) {
			const auto& region      = it->second;
			const auto  range_start = std::max(vaddr, region.vaddr);
			const auto  range_end   = std::min(end, region.vaddr + region.size);
			if (range_start < range_end &&
			    !VirtualMemory::Protect(range_start, range_end - range_start, GuestHostProtection(mode))) {
				return false;
			}
		}
		return true;
	}

	bool ConsumeUnlocked(uint64_t vaddr, uint64_t size) {
		if (vaddr == 0 || size == 0 || UINT64_MAX - vaddr < size) {
			return false;
		}
		auto it = FindContaining(vaddr, size);
#if defined(__ANDROID__)
		if (it == m_free.end() && AcquirePlaceholderUnlocked(vaddr, size)) {
			it = FindContaining(vaddr, size);
		}
#endif
		if (it == m_free.end() || !SplitUnlocked(it, vaddr, size)) {
			return false;
		}
		it = m_free.find(vaddr);
		if (it == m_free.end() || it->second != size) {
			return false;
		}
		m_free.erase(it);
		return true;
	}

	std::map<uint64_t, MappedRegion>::iterator FindMappedContainingUnlocked(uint64_t vaddr) {
		auto next = m_mapped.upper_bound(vaddr);
		if (next == m_mapped.begin()) {
			return m_mapped.end();
		}
		auto it = std::prev(next);
		return vaddr < it->second.vaddr + it->second.size ? it : m_mapped.end();
	}

	bool ContainsMappedUnlocked(uint64_t vaddr, uint64_t size, MappingType type) {
		if (size == 0 || UINT64_MAX - vaddr < size) {
			return false;
		}
		const auto end     = vaddr + size;
		auto       current = vaddr;
		while (current < end) {
			const auto it = FindMappedContainingUnlocked(current);
			if (it == m_mapped.end() || it->second.type != type) {
				return false;
			}
			current = std::min(end, it->second.vaddr + it->second.size);
		}
		return true;
	}

	bool AddMappedUnlocked(uint64_t vaddr, uint64_t size, MappingType type,
	                       bool contiguous_hash_allowed = false) {
		if (size == 0 || UINT64_MAX - vaddr < size) {
			return false;
		}
		const auto end  = vaddr + size;
		auto       next = m_mapped.lower_bound(vaddr);
		if (next != m_mapped.end() && next->second.vaddr < end) {
			return false;
		}
		if (next != m_mapped.begin()) {
			const auto& previous = std::prev(next)->second;
			if (previous.vaddr + previous.size > vaddr) {
				return false;
			}
		}
		return m_mapped.emplace(vaddr, MappedRegion {
		    vaddr, size, type, NextGuestBackingIdentity(), vaddr, contiguous_hash_allowed}).second;
	}

	bool RemoveMappedUnlocked(uint64_t vaddr, uint64_t size, MappingType type) {
		if (!ContainsMappedUnlocked(vaddr, size, type)) {
			return false;
		}
		const auto end     = vaddr + size;
		auto       current = vaddr;
		while (current < end) {
			auto       it       = FindMappedContainingUnlocked(current);
			const auto original = it->second;
			const auto part_end = std::min(end, original.vaddr + original.size);
			m_mapped.erase(it);
			if (original.vaddr < current) {
				auto left = original;
				left.size = current - original.vaddr;
				MEMORY_REQUIRE(!m_mapped.emplace(left.vaddr, left).second);
			}
			if (part_end < original.vaddr + original.size) {
				auto right = original;
				right.vaddr = part_end;
				right.size = original.vaddr + original.size - part_end;
				MEMORY_REQUIRE(!m_mapped.emplace(right.vaddr, right).second);
			}
			current = part_end;
		}
		return true;
	}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	static DWORD GetProtect(VirtualMemory::Mode mode) {
		switch (mode) {
			case VirtualMemory::Mode::Read: return PAGE_READONLY;
			case VirtualMemory::Mode::Write:
			case VirtualMemory::Mode::ReadWrite: return PAGE_READWRITE;
			case VirtualMemory::Mode::Execute: return PAGE_EXECUTE;
			case VirtualMemory::Mode::ExecuteRead: return PAGE_EXECUTE_READ;
			case VirtualMemory::Mode::ExecuteWrite:
			case VirtualMemory::Mode::ExecuteReadWrite: return PAGE_EXECUTE_READWRITE;
			case VirtualMemory::Mode::NoAccess:
			default: return PAGE_NOACCESS;
		}
	}
#endif

	void ReserveGuestRegions() {
#if defined(__ANDROID__)
		// The console VA windows exceed Android's native default VA on 39-bit
		// devices. Eagerly occupying every gap leaves no space for the backing
		// alias, FEX or host drivers. A requested range becomes guest-owned only
		// after the kernel atomically grants its non-replacing placeholder.
		return;
#else
		const std::array<std::pair<uint64_t, uint64_t>, 3> regions {{
		    {HOST_SYSTEM_MANAGED_MIN, HOST_SYSTEM_RESERVED_MAX + 1u},
		    {HOST_USER_MIN, HOST_USER_MAX + 1u},
		    {kExtendedMemoryBase, kExtendedMemoryBase + kExtendedMemorySize},
		}};

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		constexpr uint64_t MinimumRegionSize = 0x1000000;
		const auto         process           = GetCurrentProcess();
		for (const auto& [guest_start, guest_end]: regions) {
			auto next_addr = guest_start;
			while (next_addr < guest_end) {
				MEMORY_BASIC_INFORMATION info {};
				if (VirtualQuery(reinterpret_cast<const void*>(next_addr), &info, sizeof(info)) == 0) {
					EXIT("failed to inspect guest address space at 0x%016" PRIx64 "\n", next_addr);
				}

				const auto region_base = reinterpret_cast<uint64_t>(info.BaseAddress);
				const auto region_end = std::min<uint64_t>(
				    guest_end, region_base + static_cast<uint64_t>(info.RegionSize));
				const auto reserve_start =
				    AlignUp(std::max(region_base, guest_start), m_granularity);
				const auto reserve_end = AlignDown(region_end, m_granularity);

				if (info.State == MEM_FREE && reserve_end > reserve_start &&
				    reserve_end - reserve_start > MinimumRegionSize) {
					auto* ptr = VirtualAlloc2(
					    process, reinterpret_cast<void*>(reserve_start), reserve_end - reserve_start,
					    MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0);
					if (reinterpret_cast<uint64_t>(ptr) != reserve_start) {
						EXIT("failed to reserve guest address space at 0x%016" PRIx64
						     ", size 0x%016" PRIx64 "\n",
						     reserve_start, reserve_end - reserve_start);
					}
					AddFreeUnlocked(reserve_start, reserve_end - reserve_start);
					m_owned.emplace_back(reserve_start, reserve_end - reserve_start);
				}

				if (region_end <= next_addr) {
					EXIT("guest address-space scan did not advance at 0x%016" PRIx64 "\n", next_addr);
				}
				next_addr = region_end;
			}
		}
#else
#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX && (!defined(__APPLE__) || defined(LUMEN_PS5_FEXCORE))
		std::vector<std::pair<uint64_t, uint64_t>> occupied;
#if defined(__APPLE__)
		// A native ARM64 image and dyld occupy addresses inside the guest's VA
		// windows. Enumerate actual Mach regions and reserve only vacant gaps.
		// mmap remains non-replacing: a race with a new host map only denies a gap.
		mach_vm_address_t cursor = 0;
		for (;;) {
			mach_vm_address_t address = cursor;
			mach_vm_size_t size = 0;
			vm_region_basic_info_data_64_t info {};
			mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
			mach_port_t object = MACH_PORT_NULL;
			const auto status = mach_vm_region(mach_task_self(), &address, &size,
			    VM_REGION_BASIC_INFO_64, reinterpret_cast<vm_region_info_t>(&info), &count,
			    &object);
			if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object);
			if (status == KERN_INVALID_ADDRESS) break;
			if (status != KERN_SUCCESS || size == 0 || size > UINT64_MAX - address ||
			    address + size <= cursor) {
				EXIT("cannot inspect native Mach address space: status=%d\n", status);
			}
			occupied.emplace_back(address, address + size);
			cursor = address + size;
		}
#else
		FILE*                                      maps = fopen("/proc/self/maps", "r");
		MEMORY_REQUIRE(maps == nullptr);

		char line[512];
		while (fgets(line, sizeof(line), maps) != nullptr) {
			unsigned long long mapping_start = 0;
			unsigned long long mapping_end   = 0;
			if (sscanf(line, "%llx-%llx", &mapping_start, &mapping_end) == 2) {
				occupied.emplace_back(static_cast<uint64_t>(mapping_start),
				                      static_cast<uint64_t>(mapping_end));
			}
		}
		fclose(maps);
#endif

		auto reserve_range = [this](uint64_t start, uint64_t end) {
			start = AlignUp(start, PageSize());
			end   = AlignDown(end, PageSize());
			if (start == 0 || end <= start) {
				return;
			}

			const auto size = end - start;
			int        flags = MAP_PRIVATE | MAP_ANON | MAP_NORESERVE;
#if defined(KYTY_LINKED_GUEST_ADDRESS_SPACE)
			flags |= MAP_FIXED;
#elif defined(MAP_FIXED_NOREPLACE)
			flags |= MAP_FIXED_NOREPLACE;
#endif

			void* ptr = mmap(reinterpret_cast<void*>(start), size, PROT_NONE, flags, -1, 0);
			if (ptr == MAP_FAILED || reinterpret_cast<uint64_t>(ptr) != start) {
				if (ptr != MAP_FAILED) {
					munmap(ptr, size);
				}
				return;
			}

			AddFreeUnlocked(start, size);
			m_owned.emplace_back(start, size);
		};

		for (const auto& [region_start, region_end]: regions) {
			auto current = region_start;

			for (const auto& [mapping_start, mapping_end]: occupied) {
				if (mapping_end <= current) {
					continue;
				}
				if (mapping_start >= region_end) {
					break;
				}

				if (mapping_start > current) {
					reserve_range(current, std::min(mapping_start, region_end));
				}

				current = std::max(current, mapping_end);
				if (current >= region_end) {
					break;
				}
			}

			if (current < region_end) {
				reserve_range(current, region_end);
			}
		}
#else
		for (const auto& [start, end]: regions) {
			int flags = MAP_PRIVATE | MAP_ANON | MAP_NORESERVE;
#if defined(KYTY_LINKED_GUEST_ADDRESS_SPACE)
			flags |= MAP_FIXED;
#elif defined(MAP_FIXED_NOREPLACE)
			flags |= MAP_FIXED_NOREPLACE;
#endif
			void* ptr = mmap(reinterpret_cast<void*>(start), end - start, PROT_NONE, flags, -1, 0);
			if (ptr == MAP_FAILED || reinterpret_cast<uint64_t>(ptr) != start) {
				if (ptr != MAP_FAILED) {
					munmap(ptr, end - start);
				}
				EXIT("failed to reserve guest address space at 0x%016" PRIx64
				     ", size 0x%016" PRIx64 "\n",
				     start, end - start);
			}
			AddFreeUnlocked(start, end - start);
			m_owned.emplace_back(start, end - start);
		}
#endif
#endif
#endif
	}

	uint64_t FindFreeAlignedUnlocked(uint64_t search_start, uint64_t search_end, uint64_t size,
	                                 uint64_t alignment, bool acquire_placeholder = true) {
		if (search_start >= search_end || size > search_end - search_start) {
			return 0;
		}
		for (const auto& [base, range_size]: m_free) {
			const auto candidate = AlignUp(std::max(base, search_start), alignment);
			if (candidate < search_end && size <= search_end - candidate &&
			    GuestFreeRangeContains(base, range_size, candidate, size)) {
				return candidate;
			}
		}
		if (!acquire_placeholder) return 0;
#if defined(__ANDROID__)
		return AcquireAlignedPlaceholderUnlocked(search_start, search_end, size, alignment);
#else
		return 0;
#endif
	}

#if defined(__ANDROID__)
	static bool IsGuestWindow(uint64_t vaddr, uint64_t size) {
		return GuestFreeRangeContains(HOST_SYSTEM_MANAGED_MIN,
		           HOST_SYSTEM_RESERVED_MAX + 1u - HOST_SYSTEM_MANAGED_MIN, vaddr, size) ||
		       GuestFreeRangeContains(HOST_USER_MIN, HOST_USER_MAX + 1u - HOST_USER_MIN,
		                              vaddr, size) ||
		       GuestFreeRangeContains(kExtendedMemoryBase, kExtendedMemorySize, vaddr, size);
	}

	// Caller holds m_mutex. Ownership and free-space metadata are published only
	// after acquiring the actual placeholder; MAP_FIXED never acquires new VA.
	bool AcquirePlaceholderUnlocked(uint64_t vaddr, uint64_t size) {
		if (vaddr == 0 || size == 0 || !IsPageAligned(vaddr) || !IsPageAligned(size) ||
		    !IsGuestWindow(vaddr, size)) {
			return false;
		}
		const auto end = vaddr + size;
		auto mapped = m_mapped.lower_bound(vaddr);
		if (mapped != m_mapped.begin()) {
			--mapped;
		}
		for (; mapped != m_mapped.end() && mapped->second.vaddr < end; ++mapped) {
			if (mapped->second.vaddr + mapped->second.size > vaddr) {
				return false;
			}
		}
		// A fixed aggregate can contain earlier free placeholders and new gaps.
		// Acquire all gaps before publishing any of them, and roll back only the
		// new kernel mappings if another native allocation wins one of the gaps.
		std::vector<std::pair<uint64_t, uint64_t>> acquired;
		int flags = MAP_PRIVATE | MAP_ANON | MAP_NORESERVE;
#if defined(MAP_FIXED_NOREPLACE)
		flags |= MAP_FIXED_NOREPLACE;
#endif
		auto cursor = vaddr;
		while (cursor < end) {
			if (const auto free = FindContaining(cursor, 1); free != m_free.end()) {
				cursor = std::min(end, free->first + free->second);
				continue;
			}
			const auto next = m_free.lower_bound(cursor);
			const auto gap_end = next == m_free.end() ? end : std::min(end, next->first);
			const auto gap_size = gap_end - cursor;
			void* ptr = mmap(reinterpret_cast<void*>(cursor), gap_size, PROT_NONE, flags, -1, 0);
			if (ptr == MAP_FAILED || reinterpret_cast<uint64_t>(ptr) != cursor) {
				if (ptr != MAP_FAILED) {
					munmap(ptr, gap_size);
				}
				for (const auto& [start, length]: acquired) {
					MEMORY_REQUIRE(munmap(reinterpret_cast<void*>(start), length) != 0);
				}
				return false;
			}
			acquired.emplace_back(cursor, gap_size);
			cursor = gap_end;
		}
		for (const auto& [start, length]: acquired) {
			AddFreeUnlocked(start, length);
			m_owned.emplace_back(start, length);
		}
		std::sort(m_owned.begin(), m_owned.end());
		// Adjacent owned placeholders form one logical span, as in the eager
		// reservation. This preserves aggregate protect/read/unmap checks.
		size_t output = 0;
		for (const auto& range: m_owned) {
			if (output != 0 && m_owned[output - 1].first + m_owned[output - 1].second == range.first) {
				m_owned[output - 1].second += range.second;
			} else {
				m_owned[output++] = range;
			}
		}
		m_owned.resize(output);
		return true;
	}

	uint64_t AcquireAlignedPlaceholderUnlocked(uint64_t search_start, uint64_t search_end,
	                                          uint64_t size, uint64_t alignment) {
		FILE* maps = fopen("/proc/self/maps", "r");
		if (maps == nullptr) return 0;
		std::vector<std::pair<uint64_t, uint64_t>> occupied;
		char line[512];
		while (fgets(line, sizeof(line), maps) != nullptr) {
			unsigned long long start = 0, end = 0;
			if (sscanf(line, "%llx-%llx", &start, &end) == 2 && end > start)
				occupied.emplace_back(start, end);
		}
		const bool valid = ferror(maps) == 0;
		fclose(maps);
		if (!valid) return 0;
		std::sort(occupied.begin(), occupied.end());
		auto try_gap = [&](uint64_t start, uint64_t end) {
			const auto candidate = AlignUp(std::max(start, search_start), alignment);
			return candidate >= start && candidate < end && size <= end - candidate &&
			       AcquirePlaceholderUnlocked(candidate, size) ? candidate : uint64_t {0};
		};
		uint64_t cursor = search_start;
		for (const auto& [start, end]: occupied) {
			if (end <= cursor) continue;
			if (start >= search_end) break;
			if (start > cursor) {
				if (const auto candidate = try_gap(cursor, std::min(start, search_end)); candidate != 0)
					return candidate;
			}
			cursor = std::max(cursor, end);
			if (cursor >= search_end) return 0;
		}
		return cursor < search_end ? try_gap(cursor, search_end) : 0;
	}
#endif

	std::map<uint64_t, uint64_t>::iterator FindContaining(uint64_t vaddr, uint64_t size) {
		auto next = m_free.upper_bound(vaddr);
		if (next == m_free.begin()) {
			return m_free.end();
		}
		auto it  = std::prev(next);
		auto end = it->first + it->second;
		return (vaddr >= it->first && vaddr + size <= end ? it : m_free.end());
	}

	bool SplitUnlocked(std::map<uint64_t, uint64_t>::iterator it, uint64_t vaddr, uint64_t size) {
		const auto base = it->first;
		const auto end  = it->first + it->second;
		if (base == vaddr && end == vaddr + size) {
			return true;
		}

		if (base != vaddr) {
			const auto left_size = vaddr - base;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
			if (VirtualFree(reinterpret_cast<void*>(base), left_size,
			                MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) == 0) {
				LOGF_COLOR(Log::Color::Red,
				           "\t guest placeholder split failed: base=0x%016" PRIx64
				           " size=0x%016" PRIx64 " requested=0x%016" PRIx64
				           " request_size=0x%016" PRIx64 " error=0x%08" PRIx32 "\n",
				           base, end - base, vaddr, size, static_cast<uint32_t>(GetLastError()));
				return false;
			}
#endif
			it->second = left_size;
			it         = m_free.emplace(vaddr, end - vaddr).first;
		}

		if (it->second != size) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
			if (VirtualFree(reinterpret_cast<void*>(it->first), size,
			                MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) == 0) {
				LOGF_COLOR(Log::Color::Red,
				           "\t guest placeholder split failed: base=0x%016" PRIx64
				           " size=0x%016" PRIx64 " requested=0x%016" PRIx64
				           " request_size=0x%016" PRIx64 " error=0x%08" PRIx32 "\n",
				           it->first, it->second, vaddr, size,
				           static_cast<uint32_t>(GetLastError()));
				return false;
			}
#endif
			const auto right_start = it->first + size;
			const auto right_size  = it->second - size;
			it->second             = size;
			m_free.emplace(right_start, right_size);
		}

		return true;
	}

	void AddFreeUnlocked(uint64_t vaddr, uint64_t size) {
		if (vaddr == 0 || size == 0) {
			return;
		}
		auto [it, inserted] = m_free.emplace(vaddr, size);
		if (!inserted) {
			it->second = size;
		}

		auto base = it->first;
		auto end  = it->first + it->second;

		auto first = it;
		while (first != m_free.begin()) {
			auto prev = std::prev(first);
			if (prev->first + prev->second != base) {
				break;
			}
			base  = prev->first;
			first = prev;
		}

		auto last = std::next(it);
		while (last != m_free.end() && end == last->first) {
			end  = last->first + last->second;
			last = std::next(last);
		}

		if (base != it->first || end != it->first + it->second) {
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
			if (CoalescePlaceholders(base, end - base)) {
				m_free.erase(first, last);
				m_free.emplace(base, end - base);
			}
#else
			m_free.erase(first, last);
			m_free.emplace(base, end - base);
#endif
		}
	}

	void RestorePlaceholder(uint64_t vaddr, uint64_t size) {
#if KYTY_PLATFORM != KYTY_PLATFORM_WINDOWS
		// The address remains owned for the process lifetime; MAP_FIXED only replaces one of our
		// mappings and never acquires a new guest address.
		void* ptr = mmap(reinterpret_cast<void*>(vaddr), size, PROT_NONE,
		                 MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
		MEMORY_REQUIRE(ptr == MAP_FAILED || reinterpret_cast<uint64_t>(ptr) != vaddr);
#else
		(void)vaddr;
		(void)size;
#endif
	}

	std::shared_mutex                          m_mutex;
	std::map<uint64_t, uint64_t>               m_free;
	std::map<uint64_t, MappedRegion>            m_mapped;
	std::vector<std::pair<uint64_t, uint64_t>> m_owned;
	uint64_t                                   m_granularity = 0x10000;
	GuestBackingStore*                         m_backing     = nullptr;
};
