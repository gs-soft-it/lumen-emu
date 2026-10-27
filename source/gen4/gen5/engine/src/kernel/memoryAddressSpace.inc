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
#if defined(__ANDROID__)
// Match the accepted Gen4 host address-space boundary. Android's 39-bit
// userspace must retain room above guest reservations for the full backing
// alias, translator and native drivers. PS5 logical/physical sizes stay intact.
static constexpr uint64_t HOST_USER_MAX = 0x3fffffffffull;
#else
static constexpr uint64_t HOST_USER_MAX = 0xfbffffffffull;
#endif

// Guest addresses in Darwin's permanent inaccessible reservations cannot be mapped
// identity. Each such interval owns a separate native arena for the lifetime of
// this address space. Translation never consults mutable maps or allocates, so
// the CPU fault path can use it without acquiring an interrupted VM lock.
static constexpr uint64_t TranslatedPageSize = 0x4000;
struct GuestTranslationArena {
	uint64_t guest = 0;
	uint64_t host = 0;
	uint64_t size = 0;
	std::unique_ptr<std::atomic<uint8_t>[]> rights;
};
struct GuestTranslationArenas {
	std::vector<GuestTranslationArena> ranges;
};
static_assert(std::atomic<const GuestTranslationArenas*>::is_always_lock_free);
static_assert(std::atomic<uint8_t>::is_always_lock_free);
static std::atomic<const GuestTranslationArenas*> g_translation_arenas {nullptr};
#if defined(KYTY_BACKING_MUTATION_TESTS)
static void (*g_test_translated_mapping_phase)(uint64_t, uint64_t, bool) = nullptr;
#endif

#if defined(__APPLE__) && defined(LUMEN_PS5_DIRECT_FEX) && defined(__aarch64__)
// Darwin's public region flags do not expose vm_map_entry::vme_permanent.
// NONE/NONE (even with VM_MEMORY_GUARD) also describes user guards that can be
// deallocated and reused. Admit only the OS ABI reservations installed with
// VM_MAP_KERNEL_FLAGS_FIXED_PERMANENT by vm_map_exec/ml_get_vm_reserved_regions:
// https://github.com/apple-oss-distributions/xnu/blob/main/osfmk/vm/vm_map.c
// https://github.com/apple-oss-distributions/xnu/blob/main/osfmk/arm64/machine_routines.c
// The GPU bounds are public SDK constants. The commpage nesting constants are
// kernel-private ABI definitions in osfmk/arm/cpu_capabilities.h.
static constexpr uint64_t DarwinCommpageReservationStart = 0x0000000fc0000000ull;
static constexpr uint64_t DarwinCommpageReservationEnd = 0x0000001000000000ull;
// pmap_create_commpages/pmap_insert_commpage map data and randomized text in the
// last L2 (32 MiB for 16 KiB tasks), underneath the dummy Mach NONE/NONE region.
// Exclude that entire hardware nesting interval, including presently empty PTEs.
// https://github.com/apple-oss-distributions/xnu/blob/main/osfmk/arm64/sptm/pmap/pmap.c
static constexpr uint64_t DarwinCommpageNativeStart = DarwinCommpageReservationEnd - 0x2000000ull;

static bool DarwinPermanentTranslationEnd(uint64_t address, uint64_t size,
                                           const vm_region_basic_info_data_64_t& basic,
                                           uint64_t* translation_end) {
	if (sysconf(_SC_PAGESIZE) != TranslatedPageSize || basic.protection != VM_PROT_NONE ||
	    basic.max_protection != VM_PROT_NONE) return false;
	uint64_t end = 0;
	if (address == DarwinCommpageReservationStart &&
	    size == DarwinCommpageReservationEnd - DarwinCommpageReservationStart) {
		end = DarwinCommpageNativeStart;
	} else if (address == MACH_VM_MIN_GPU_CARVEOUT_ADDRESS &&
	           size == MACH_VM_MAX_GPU_CARVEOUT_ADDRESS - MACH_VM_MIN_GPU_CARVEOUT_ADDRESS) {
		end = MACH_VM_MAX_GPU_CARVEOUT_ADDRESS;
	} else {
		return false;
	}
	// Require the live Mach reservation to still match the known OS layout.
	// A changed layout fails closed instead of aliasing an unrelated mapping.
	mach_vm_address_t region = address;
	mach_vm_size_t bytes = 0;
	vm_region_extended_info_data_t info {};
	mach_msg_type_number_t count = VM_REGION_EXTENDED_INFO_COUNT;
	mach_port_t object = MACH_PORT_NULL;
	const auto status = mach_vm_region(mach_task_self(), &region, &bytes, VM_REGION_EXTENDED_INFO,
	    reinterpret_cast<vm_region_info_t>(&info), &count, &object);
	if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object);
	if (status != KERN_SUCCESS || region != address || bytes != size ||
	    info.user_tag != VM_MEMORY_GUARD || info.share_mode != SM_EMPTY) return false;
	*translation_end = end;
	return true;
}
#endif

static const GuestTranslationArena* FindTranslatedGuestRange(uint64_t guest, uint64_t size) noexcept {
	if (size == 0 || size > UINT64_MAX - guest) return nullptr;
	const auto* arenas = g_translation_arenas.load(std::memory_order_acquire);
	if (arenas == nullptr) return nullptr;
	for (const auto& arena: arenas->ranges) {
		if (guest >= arena.guest && guest - arena.guest < arena.size &&
		    size <= arena.size - (guest - arena.guest)) return &arena;
	}
	return nullptr;
}

bool TranslateGuestMemory(uint64_t guest, uint64_t size, uint64_t* host) noexcept {
	if (host == nullptr || size == 0 || size > UINT64_MAX - guest) return false;
	const auto* arenas = g_translation_arenas.load(std::memory_order_acquire);
	if (arenas != nullptr) {
		for (const auto& arena: arenas->ranges) {
			if (guest < arena.guest + arena.size && arena.guest < guest + size) {
				if (guest < arena.guest || size > arena.size - (guest - arena.guest)) return false;
				*host = arena.host + guest - arena.guest;
				return true;
			}
		}
	}
	*host = guest;
	return true;
}

bool TranslateHostMemoryFault(uint64_t host, uint64_t* guest) noexcept {
	if (guest == nullptr) return false;
	const auto* arenas = g_translation_arenas.load(std::memory_order_acquire);
	if (arenas == nullptr) return false;
	for (const auto& arena: arenas->ranges) {
		if (host >= arena.host && host - arena.host < arena.size) {
			*guest = arena.guest + host - arena.host;
			return true;
		}
	}
	return false;
}

bool IsGuestNonIdentity(uint64_t guest, uint64_t size) noexcept {
	return FindTranslatedGuestRange(guest, size) != nullptr;
}

bool QueryTranslatedGuestAccess(uint64_t guest, uint64_t size, bool write) noexcept {
	const auto* arena = FindTranslatedGuestRange(guest, size);
	if (arena == nullptr) return false;
	const uint8_t required = 0x80u | (write ? 2u : 1u);
	const auto last = (guest - arena->guest + size - 1) / TranslatedPageSize;
	for (auto page = (guest - arena->guest) / TranslatedPageSize; page <= last; ++page) {
		if ((arena->rights[page].load(std::memory_order_acquire) & required) != required) return false;
	}
	return true;
}

bool QueryTranslatedHostAccess(uint64_t guest, uint64_t size, bool write) noexcept {
	const auto* arena = FindTranslatedGuestRange(guest, size);
	if (arena == nullptr) return false;
	const uint8_t required = write ? 0x20u : 0x10u;
	const auto last = (guest - arena->guest + size - 1) / TranslatedPageSize;
	for (auto page = (guest - arena->guest) / TranslatedPageSize; page <= last; ++page) {
		if ((arena->rights[page].load(std::memory_order_acquire) & required) != required) return false;
	}
	return true;
}

static void PublishTranslatedRights(uint64_t guest, uint64_t size, uint8_t mask, uint8_t rights) noexcept {
	const auto* arena = FindTranslatedGuestRange(guest, size);
	if (arena == nullptr) return;
	const auto last = (guest - arena->guest + size - 1) / TranslatedPageSize;
	for (auto page = (guest - arena->guest) / TranslatedPageSize; page <= last; ++page) {
		auto previous = arena->rights[page].load(std::memory_order_relaxed);
		while (!arena->rights[page].compare_exchange_weak(previous,
		    (previous & ~mask) | (rights & mask), std::memory_order_release, std::memory_order_relaxed)) {}
	}
}

static void PublishTranslatedGuestRights(uint64_t guest, uint64_t size, uint8_t rights) noexcept {
	PublishTranslatedRights(guest, size, 0x87u, rights);
}

static void PublishTranslatedHostRights(uint64_t guest, uint64_t size, VirtualMemory::Mode mode) noexcept {
	auto rights = static_cast<uint8_t>(mode);
	if ((rights & 6u) != 0) rights |= 1u; // native write and translated execute imply native read
	PublishTranslatedRights(guest, size, 0x30u, (rights & 3u) << 4u);
}

static bool ProtectTranslatedHost(uint64_t guest, uint64_t size, VirtualMemory::Mode mode) {
	uint64_t host = 0;
	if (!TranslateGuestMemory(guest, size, &host) || !VirtualMemory::Protect(host, size, mode)) return false;
	PublishTranslatedHostRights(guest, size, mode);
	return true;
}

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

template <typename Regions, typename Source, typename Gap>
static void CopySparseRange(const Regions& regions, uint64_t vaddr, void* data, uint64_t size,
                            Source&& source, Gap&& gap) {
	const auto end     = vaddr + size;
	auto       mapping = regions.lower_bound(vaddr);
	if (mapping != regions.begin()) {
		auto previous = std::prev(mapping);
		if (previous->second.vaddr + previous->second.size > vaddr) {
			mapping = previous;
		}
	}

	auto* destination = static_cast<uint8_t*>(data);
	auto  cursor      = vaddr;
	for (; mapping != regions.end() && mapping->second.vaddr < end; ++mapping) {
		const auto& entry = mapping->second;
		const auto  start = std::max(vaddr, entry.vaddr);
		const auto  bytes = std::min(end, entry.vaddr + entry.size) - start;
		if (cursor < start) {
			gap(cursor, destination + (cursor - vaddr), start - cursor);
		}
		std::memcpy(destination + (start - vaddr), source(entry, start), bytes);
		cursor = start + bytes;
	}
	if (cursor < end) {
		gap(cursor, destination + (cursor - vaddr), end - cursor);
	}
}

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
			LOGF_COLOR(Log::Color::Red, "\t direct-memory backing: shared fd creation failed: %d\n",
			           errno);
			return;
		}
		if (ftruncate(m_fd, static_cast<off_t>(m_size)) != 0) {
			LOGF_COLOR(Log::Color::Red, "\t direct-memory backing: ftruncate failed: %d\n", errno);
			close(m_fd);
			m_fd = -1;
			return;
		}
		auto* backing = mmap(nullptr, m_size, PROT_READ | PROT_WRITE, MAP_SHARED, m_fd, 0);
		if (backing == MAP_FAILED) {
			LOGF_COLOR(Log::Color::Red, "\t direct-memory backing: writable alias failed: %d\n",
			           errno);
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
			EXIT_IF(
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
		DirectCodeMutation mutation(0, 0);
		std::lock_guard lock(m_mutex);
		std::memset(m_backing_base + backing_offset, 0, size);
		InvalidatePhysicalAliasesUnlocked(backing_offset, size, mutation);
		return true;
	}

	bool TryWriteBacking(uint64_t vaddr, const void* data, uint64_t size) {
		BackingMutationScope backing_mutation;
		if (!IsAvailable() || data == nullptr || size == 0 || UINT64_MAX - vaddr < size) {
			return false;
		}

		DirectCodeMutation mutation(vaddr, size);
		std::lock_guard lock(m_mutex);
		const auto      end   = vaddr + size;
		auto            first = m_maps.upper_bound(vaddr);
		if (first == m_maps.begin()) {
			return false;
		}
		--first;
		auto current = vaddr;
		for (auto mapping = first; current < end; ++mapping) {
			if (mapping == m_maps.end() || current < mapping->second.vaddr ||
			    current >= mapping->second.vaddr + mapping->second.size) {
				return false;
			}
			const auto& entry = mapping->second;
			const auto  bytes = std::min(end, entry.vaddr + entry.size) - current;
			if (!RangeOk(entry.backing_offset + current - entry.vaddr, bytes)) {
				return false;
			}
			current += bytes;
		}

#if defined(KYTY_BACKING_MUTATION_TESTS)
		if (g_test_backing_write_phase != nullptr) g_test_backing_write_phase(false);
#endif
		const auto* source = static_cast<const uint8_t*>(data);
		current = vaddr;
		for (auto mapping = first; current < end; ++mapping) {
			const auto& entry  = mapping->second;
			const auto  bytes  = std::min(end, entry.vaddr + entry.size) - current;
			const auto  offset = entry.backing_offset + current - entry.vaddr;
			std::memcpy(m_backing_base + offset, source + (current - vaddr), bytes);
			InvalidatePhysicalAliasesUnlocked(offset, bytes, mutation);
			current += bytes;
		}
#if defined(KYTY_BACKING_MUTATION_TESTS)
		if (g_test_backing_write_phase != nullptr) g_test_backing_write_phase(true);
#endif
		return true;
	}

	void InvalidateCodeAliases(uint64_t vaddr, uint64_t size) {
		DirectCodeMutation mutation(vaddr, size);
		std::shared_lock lock(m_mutex);
		const auto end = vaddr + size;
		for (const auto& [_, entry]: m_maps) {
			const auto begin = std::max(vaddr, entry.vaddr);
			const auto limit = std::min(end, entry.vaddr + entry.size);
			if (begin < limit) InvalidatePhysicalAliasesUnlocked(
			    entry.backing_offset + begin - entry.vaddr, limit - begin, mutation);
		}
	}

	bool TryReadBacking(uint64_t vaddr, void* data, uint64_t size) {
		if (!IsAvailable() || data == nullptr || size == 0 || UINT64_MAX - vaddr < size) {
			return false;
		}

		std::shared_lock lock(m_mutex);
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
		if (g_test_backing_read != nullptr) {
			g_test_backing_read(vaddr, size);
		}
#endif
		const auto end   = vaddr + size;
		auto       first = m_maps.upper_bound(vaddr);
		if (first == m_maps.begin()) {
			return false;
		}
		--first;
		auto current = vaddr;
		for (auto mapping = first; current < end; ++mapping) {
			if (mapping == m_maps.end() || current < mapping->second.vaddr ||
			    current >= mapping->second.vaddr + mapping->second.size) {
				return false;
			}
			const auto& entry = mapping->second;
			const auto  bytes = std::min(end, entry.vaddr + entry.size) - current;
			if (!RangeOk(entry.backing_offset + current - entry.vaddr, bytes)) {
				return false;
			}
			current += bytes;
		}

		auto* destination = static_cast<uint8_t*>(data);
		current = vaddr;
		for (auto mapping = first; current < end; ++mapping) {
			const auto& entry  = mapping->second;
			const auto  bytes  = std::min(end, entry.vaddr + entry.size) - current;
			const auto  offset = entry.backing_offset + current - entry.vaddr;
			std::memcpy(destination + (current - vaddr), m_backing_base + offset, bytes);
			current += bytes;
		}
		return true;
	}

	template <typename Gap>
	bool TryReadSparseBacking(uint64_t vaddr, void* data, uint64_t size, Gap&& gap) {
		if (!IsAvailable() || data == nullptr || size == 0 || UINT64_MAX - vaddr < size) {
			return false;
		}

		std::shared_lock lock(m_mutex);
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
		if (g_test_backing_read != nullptr) {
			g_test_backing_read(vaddr, size);
		}
#endif
		CopySparseRange(m_maps, vaddr, data, size,
		                [this](const auto& entry, uint64_t start) {
			                return m_backing_base + (entry.backing_offset + start - entry.vaddr);
		                },
		                gap);
		return true;
	}

	bool MapFixed(uint64_t vaddr, uint64_t size, uint64_t backing_offset,
	              VirtualMemory::Mode mode, FailureReason* failure_reason = nullptr) {
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
			EXIT_IF(!m_maps.emplace(vaddr, MapEntry {vaddr, size, backing_offset, mode}).second);
		}
		SetFailure(failure_reason, FailureReason::None);
		return true;
#else
		std::lock_guard lock(m_mutex);
		void* mapped = MapView(vaddr, size, backing_offset, mode, failure_reason);
		uint64_t host = 0;
		if (mapped == nullptr || !TranslateGuestMemory(vaddr, size, &host) ||
		    reinterpret_cast<uint64_t>(mapped) != host) {
			return false;
		}
		RemoveMappingsUnlocked(vaddr, size);
		EXIT_IF(!m_maps.emplace(vaddr, MapEntry {vaddr, size, backing_offset, mode}).second);
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
					EXIT_IF(!MapFixed(it->vaddr, it->size, it->backing_offset, it->mode));
				}
				if (placeholder_preserved != nullptr) {
					*placeholder_preserved = false;
				}
				return false;
			}
			EXIT_IF(!part_placeholder_preserved);
			removed.push_back(entry);
		}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		// Each unmapped file view becomes a separate Windows placeholder. The address-space owner
		// records this contiguous guest span as one free range, so normalize fragmented backing
		// views into one placeholder before publishing it there.
		if (removed.size() > 1 && !CoalescePlaceholders(vaddr, size)) {
			for (auto it = removed.rbegin(); it != removed.rend(); ++it) {
				EXIT_IF(!MapFixed(it->vaddr, it->size, it->backing_offset, it->mode));
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

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
		const uint64_t left_size  = vaddr - old.vaddr;
		const uint64_t right_addr = vaddr + size;
		const uint64_t right_size = old.vaddr + old.size - right_addr;
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
				ok = MapFixed(old.vaddr, left_size, old.backing_offset, old.mode) && ok;
			}
			if (right_size != 0) {
				ok = MapFixed(right_addr, right_size,
				              old.backing_offset + (right_addr - old.vaddr), old.mode) &&
				     ok;
			}
			if (ok) {
				if (placeholder_preserved != nullptr) {
					*placeholder_preserved = true;
				}
				return true;
			}

			if (left_size != 0 && Contains(old.vaddr, left_size)) {
				EXIT_IF(!UnmapOne(old.vaddr, left_size));
			}
			if (right_size != 0 && Contains(right_addr, right_size)) {
				EXIT_IF(!UnmapOne(right_addr, right_size));
			}
			EXIT_IF(VirtualFree(reinterpret_cast<void*>(old.vaddr), old.size,
			                    MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS) == 0);
			EXIT_IF(!MapFixed(old.vaddr, old.size, old.backing_offset, old.mode));
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
		RemoveMappingsUnlocked(vaddr, size);

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

private:
	void InvalidatePhysicalAliasesUnlocked(uint64_t offset, uint64_t size,
	                                       const DirectCodeMutation& mutation) const {
		for (const auto& [_, alias]: m_maps) {
			const auto begin = std::max(offset, alias.backing_offset);
			const auto end = std::min(offset + size, alias.backing_offset + alias.size);
			if (begin < end) mutation.Publish(alias.vaddr + begin - alias.backing_offset, end - begin);
		}
	}

	struct MapEntry {
		uint64_t            vaddr          = 0;
		uint64_t            size           = 0;
		uint64_t            backing_offset = 0;
		VirtualMemory::Mode mode           = VirtualMemory::Mode::NoAccess;
	};

#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX
	void RemoveMappingsUnlocked(uint64_t vaddr, uint64_t size) {
		const auto end = vaddr + size;
		auto       it = m_maps.upper_bound(vaddr);
		if (it != m_maps.begin()) {
			--it;
		}
		while (it != m_maps.end() && it->first < end) {
			const auto old     = it->second;
			const auto old_end = old.vaddr + old.size;
			if (old_end <= vaddr) {
				++it;
				continue;
			}
			it = m_maps.erase(it);
			if (old.vaddr < vaddr) {
				m_maps.emplace(old.vaddr,
				               MapEntry {old.vaddr, vaddr - old.vaddr, old.backing_offset, old.mode});
			}
			if (end < old_end) {
				m_maps.emplace(end, MapEntry {end, old_end - end,
				                             old.backing_offset + end - old.vaddr, old.mode});
			}
		}
	}
#endif

	bool FindContainingUnlocked(uint64_t vaddr, uint64_t size, MapEntry* out) const {
		EXIT_IF(out == nullptr);

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

		if (mode == VirtualMemory::Mode::NoAccess && !ApplyProtection(ptr, size, mode)) {
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
#if defined(__APPLE__) && defined(LUMEN_PS5_DIRECT_FEX)
		if (VirtualMemory::IsExecute(mode))
			mode = static_cast<VirtualMemory::Mode>((static_cast<uint32_t>(mode) & ~4u) | 1u);
#endif
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
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
		if (const auto callback = g_test_before_backing_map) {
			g_test_before_backing_map = nullptr;
			callback(vaddr, size);
		}
#endif
		uint64_t host = 0;
		if (!TranslateGuestMemory(vaddr, size, &host) ||
		    (host != vaddr && VirtualMemory::IsExecute(mode))) {
			SetFailure(failure_reason, FailureReason::AddressRangeUnavailable);
			return nullptr;
		}
		void* ptr = mmap(reinterpret_cast<void*>(host), size, GetProtect(mode),
		                 MAP_SHARED | MAP_FIXED, m_fd, static_cast<off_t>(backing_offset));
		if (ptr == MAP_FAILED) {
			SetFailure(failure_reason, FailureReason::FixedMapViewFailed);
			return nullptr;
		}
		PublishTranslatedHostRights(vaddr, size, mode);
		return ptr;
	}

	// Replace the file view atomically instead of exposing an unmapped hole in the
	// process-lifetime guest reservation.
	static bool UnmapViewPreservePlaceholder(void* vaddr, uint64_t size) {
		uint64_t host = 0;
		if (!TranslateGuestMemory(reinterpret_cast<uint64_t>(vaddr), size, &host)) return false;
		void* ptr = mmap(reinterpret_cast<void*>(host), size, PROT_NONE,
		                 MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
		if (ptr == MAP_FAILED || reinterpret_cast<uint64_t>(ptr) != host) return false;
		PublishTranslatedHostRights(reinterpret_cast<uint64_t>(vaddr), size, VirtualMemory::Mode::NoAccess);
		return true;
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
		EXIT_IF(g_translation_arenas.exchange(&m_translation_arenas, std::memory_order_acq_rel) != nullptr);
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
				EXIT_IF(VirtualQuery(reinterpret_cast<const void*>(current), &info, sizeof(info)) ==
				        0);
				const auto region_start = reinterpret_cast<uint64_t>(info.BaseAddress);
				const auto region_end   = region_start + static_cast<uint64_t>(info.RegionSize);
				if (info.State == MEM_COMMIT) {
					EXIT_IF(VirtualFree(reinterpret_cast<void*>(region_start), info.RegionSize,
					                    MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) == 0);
					continue;
				}
				if (info.State == MEM_RESERVE) {
					EXIT_IF(VirtualFree(info.AllocationBase, 0, MEM_RELEASE) == 0);
				}
				current = region_end;
			}
		}
#else
		for (const auto& [start, size]: m_owned) {
			uint64_t host = 0;
			EXIT_IF(!TranslateGuestMemory(start, size, &host));
			munmap(reinterpret_cast<void*>(host), size);
		}
#endif
		g_translation_arenas.store(nullptr, std::memory_order_release);
	}

	KYTY_CLASS_NO_COPY(GuestAddressSpace);

	[[nodiscard]] uint64_t GetGranularity() const { return m_granularity; }
	[[nodiscard]] uint64_t GetBackingBase() const { return m_backing->GetBackingBase(); }
	[[nodiscard]] uint64_t GetBackingSize() const { return m_backing->GetBackingSize(); }

	bool QueryUniqueBackingGeneration(uint64_t vaddr, uint64_t size, uint64_t* generation) {
		return m_backing->QueryUniqueBackingGeneration(vaddr, size, generation);
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
		std::lock_guard lock(m_mutex);
		return std::any_of(m_owned.begin(), m_owned.end(), [vaddr, size](const auto& range) {
			return VirtualRangesOverlap(vaddr, size, range.first, range.second);
		});
	}

	void InvalidateCodeAliases(uint64_t vaddr, uint64_t size) {
		m_backing->InvalidateCodeAliases(vaddr, size);
	}

	bool QueryExecutable(uint64_t address, uint64_t* base, uint64_t* size, uint32_t* writable) {
		std::shared_lock lock(m_mutex);
		const auto it = FindMappedContainingUnlocked(address);
		if (it == m_mapped.end() || !VirtualMemory::IsExecute(it->second.mode)) return false;
		*base = it->second.vaddr;
		*size = it->second.size;
		*writable = (static_cast<int>(it->second.mode) & PROT_CPU_WRITE) != 0 ? 1U : 0U;
		return true;
	}

	bool TryWriteBacking(uint64_t vaddr, const void* data, uint64_t size) {
		return m_backing->TryWriteBacking(vaddr, data, size);
	}

	bool TryReadBacking(uint64_t vaddr, void* data, uint64_t size) {
		return m_backing->TryReadBacking(vaddr, data, size);
	}

	// SRT loads are host reads performed for the guest GPU. These are the logical
	// host mapping permissions (CPU/GPU combined), not the CPU ABI access rights.
	// Transient GPU watches are deliberately excluded; the caller resolves exact
	// GPU ownership before copying and must hold no address-space lock while waiting.
	bool CanReadShaderBacking(uint64_t vaddr, uint64_t size) {
		std::shared_lock lock(m_mutex);
		return IsReadableBackingSpanUnlocked(vaddr, size);
	}

	bool TryReadShaderBacking(uint64_t vaddr, void* data, uint64_t size) {
		if (data == nullptr) return false;
		std::shared_lock lock(m_mutex);
		if (!IsReadableBackingSpanUnlocked(vaddr, size)) return false;
		// Recheck after the GPU hand-off and retain mapping/protection lifetime
		// through the complete backing preflight and copy, in the normal lock order.
		return m_backing->TryReadBacking(vaddr, data, size);
	}

	template <bool copy_private>
	bool CopySparseMemory(uint64_t vaddr, void* data, uint64_t size) {
		if (data == nullptr || !Owns(vaddr, size)) {
			return false;
		}
		std::shared_lock lock(m_mutex);
		if constexpr (!copy_private) {
			auto mapping = m_mapped.upper_bound(vaddr);
			if (mapping != m_mapped.begin()) {
				--mapping;
			}
			for (; mapping != m_mapped.end() && mapping->second.vaddr < vaddr + size; ++mapping) {
				const auto& region = mapping->second;
				if (region.type == MappingType::Private && region.vaddr + region.size > vaddr) {
					return false;
				}
			}
		}
		return m_backing->TryReadSparseBacking(
		    vaddr, data, size, [&](uint64_t start, uint8_t* destination, uint64_t bytes) {
			    const auto zero = [](uint64_t, uint8_t* output, uint64_t length) {
				    std::memset(output, 0, length);
			    };
			    if constexpr (copy_private) {
				    // Backing gaps can contain private mappings or reservations only.
				    CopySparseRange(m_mapped, start, destination, bytes,
				                    [](const auto& region, uint64_t address) {
					                    EXIT_IF(region.type != MappingType::Private);
					                    uint64_t host = 0;
					                    EXIT_IF(!TranslateGuestMemory(address, 1, &host));
					                    return reinterpret_cast<const void*>(host);
				                    },
				                    zero);
			    } else {
				    zero(start, destination, bytes);
			    }
		    });
	}

	bool BackingContains(uint64_t vaddr, uint64_t size) { return m_backing->Contains(vaddr, size); }

	bool Protect(uint64_t vaddr, uint64_t size, VirtualMemory::Mode mode) {
		BackingMutationScope backing_mutation;
		if (size == 0 || UINT64_MAX - vaddr < size) {
			return false;
		}
		DirectCodeMutation mutation(vaddr, size);
		std::lock_guard lock(m_mutex);
		return ProtectMappedUnlocked(vaddr, size, mode, true);
	}

	bool ProtectTransient(uint64_t vaddr, uint64_t size, VirtualMemory::Mode mode) {
		if (size == 0 || UINT64_MAX - vaddr < size) {
			return false;
		}

		std::lock_guard lock(m_mutex);
		return ProtectMappedUnlocked(vaddr, size, mode);
	}

	bool MapBacking(uint64_t vaddr, uint64_t size, uint64_t backing_offset,
	                VirtualMemory::Mode               mode,
	                GuestBackingStore::FailureReason* failure_reason = nullptr) {
		if (IsGuestNonIdentity(vaddr, size) && VirtualMemory::IsExecute(mode)) return false;
#if defined(__APPLE__) && defined(LUMEN_PS5_DIRECT_FEX)
		if (size == 0 || !IsPageAligned(vaddr) || !IsPageAligned(size) ||
		    !IsPageAligned(backing_offset)) return false;
#endif
		BackingMutationScope backing_mutation;
		DirectCodeMutation mutation(vaddr, size);
		std::lock_guard lock(m_mutex);
#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX
		const bool replacing = ContainsMappedUnlocked(vaddr, size, MappingType::Backing);
#else
		constexpr bool replacing = false;
#endif
		if (!replacing && !ConsumeUnlocked(vaddr, size)) {
			if (failure_reason != nullptr) {
				*failure_reason = GuestBackingStore::FailureReason::AddressRangeUnavailable;
			}
			return false;
		}
		const auto host_mode = VirtualMemory::IsExecute(mode)
		                           ? static_cast<VirtualMemory::Mode>(static_cast<int>(mode) | PROT_CPU_READ)
		                           : mode;
		if (replacing) {
			IntersectTranslatedRightsUnlocked(vaddr, size, mode);
#if defined(KYTY_BACKING_MUTATION_TESTS)
			if (g_test_translated_mapping_phase) g_test_translated_mapping_phase(vaddr, size, false);
#endif
		}
		if (m_backing->MapFixed(vaddr, size, backing_offset, host_mode, failure_reason)) {
			if (replacing) {
				EXIT_IF(!RemoveMappedUnlocked(vaddr, size, MappingType::Backing, false));
#if defined(KYTY_BACKING_MUTATION_TESTS)
				if (g_test_translated_mapping_phase) g_test_translated_mapping_phase(vaddr, size, true);
#endif
			}
			EXIT_IF(!AddMappedUnlocked(vaddr, size, MappingType::Backing, mode));
			return true;
		}
		if (replacing) {
			RestoreTranslatedRightsUnlocked(vaddr, size);
		} else {
			AddFreeUnlocked(vaddr, size);
		}
		return false;
	}

	bool UnmapBacking(uint64_t vaddr, uint64_t size) {
#if defined(__APPLE__) && defined(LUMEN_PS5_DIRECT_FEX)
		if (size == 0 || !IsPageAligned(vaddr) || !IsPageAligned(size)) return false;
#endif
		BackingMutationScope backing_mutation;
		DirectCodeMutation mutation(vaddr, size);
		std::lock_guard lock(m_mutex);
		if (!ContainsMappedUnlocked(vaddr, size, MappingType::Backing)) {
			return false;
		}
		PublishTranslatedGuestRights(vaddr, size, 0);
		bool placeholder_preserved = false;
		if (!m_backing->Unmap(vaddr, size, &placeholder_preserved)) {
			RestoreTranslatedRightsUnlocked(vaddr, size);
			return false;
		}
		if (!placeholder_preserved) {
			return false;
		}
		EXIT_IF(!RemoveMappedUnlocked(vaddr, size, MappingType::Backing));
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
			                                    HOST_SYSTEM_MANAGED_MAX + 1u, size, alignment, false);
			return addr != 0 ? addr
			                 : FindFreeAlignedUnlocked(HOST_USER_MIN, HOST_USER_MAX + 1u, size,
			                                           alignment, false);
		}
		return FindFreeAlignedUnlocked(search_addr, HOST_USER_MAX + 1u, size, alignment,
		                               IsGuestNonIdentity(search_addr, 1));
	}

	uint64_t FindFreeAligned(uint64_t search_start, uint64_t search_end, uint64_t size,
	                         uint64_t alignment, bool allow_translated = true) {
		if (size == 0 || alignment == 0) {
			return 0;
		}
		alignment = std::max<uint64_t>(alignment, PageSize());
		std::lock_guard lock(m_mutex);
		return FindFreeAlignedUnlocked(search_start, search_end, size, alignment, allow_translated);
	}

	bool ReserveFixed(uint64_t vaddr, uint64_t size) {
		if (vaddr == 0 || size == 0 || !IsPageAligned(vaddr) || !IsPageAligned(size)) {
			return false;
		}
		std::lock_guard lock(m_mutex);
		return FindContaining(vaddr, size) != m_free.end();
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
		if (IsGuestNonIdentity(vaddr, size) && VirtualMemory::IsExecute(mode)) return false;
#if defined(__APPLE__) && defined(LUMEN_PS5_DIRECT_FEX)
		if (size == 0 || !IsPageAligned(vaddr) || !IsPageAligned(size)) return false;
#endif
		BackingMutationScope backing_mutation;
		DirectCodeMutation mutation(vaddr, size);
		std::lock_guard lock(m_mutex);
		if (!ConsumeUnlocked(vaddr, size)) {
			return false;
		}
		const auto host_mode = VirtualMemory::IsExecute(mode)
		                           ? static_cast<VirtualMemory::Mode>(static_cast<int>(mode) | PROT_CPU_READ)
		                           : mode;

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
		EXIT_IF(!AddMappedUnlocked(vaddr, size, MappingType::Private, mode));
		return true;
#else
		if (ProtectTranslatedHost(vaddr, size, host_mode)) {
			EXIT_IF(!AddMappedUnlocked(vaddr, size, MappingType::Private, mode));
			return true;
		}
		AddFreeUnlocked(vaddr, size);
		return false;
#endif
	}

	bool ReleaseCommitted(uint64_t vaddr, uint64_t size) {
		BackingMutationScope backing_mutation;
		if (vaddr == 0 || size == 0 || !IsPageAligned(vaddr) || !IsPageAligned(size)) {
			return false;
		}
		DirectCodeMutation mutation(vaddr, size);
		std::lock_guard lock(m_mutex);
		if (!ContainsMappedUnlocked(vaddr, size, MappingType::Private)) {
			return false;
		}
		PublishTranslatedGuestRights(vaddr, size, 0);
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
			EXIT_IF(VirtualFree(reinterpret_cast<void*>(region_vaddr), region_size,
			                    MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) == 0);
		}
		// ReleaseCommitted also publishes the aggregate span as one logical free range.
		EXIT_IF(regions.size() > 1 && !CoalescePlaceholders(vaddr, size));
#elif defined(__APPLE__) && defined(LUMEN_PS5_DIRECT_FEX)
		// Metadata can split executable/data permissions below one host page.
		// The admitted release span is host aligned and completely private; retire
		// it in one operation instead of issuing unaligned MAP_FIXED replacements.
		RestorePlaceholder(vaddr, size);
#else
		for (const auto& [region_vaddr, region_size]: regions) {
			RestorePlaceholder(region_vaddr, region_size);
		}
#endif
		EXIT_IF(!RemoveMappedUnlocked(vaddr, size, MappingType::Private));
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

		LOGF_COLOR(Log::Color::Green, "\t guest address-space self-test: ok\n");
		return true;
	}

private:
	enum class MappingType { Backing, Private };

	struct MappedRegion {
		uint64_t    vaddr = 0;
		uint64_t    size  = 0;
		MappingType type  = MappingType::Private;
		VirtualMemory::Mode mode = VirtualMemory::Mode::NoAccess;
	};

	bool IsReadableBackingSpanUnlocked(uint64_t vaddr, uint64_t size) {
		if (vaddr == 0 || size == 0 || size > UINT64_MAX - vaddr) return false;
		const auto end = vaddr + size;
		for (auto cursor = vaddr; cursor < end;) {
			const auto entry = FindMappedContainingUnlocked(cursor);
			if (entry == m_mapped.end() || entry->second.type != MappingType::Backing ||
			    ((static_cast<int>(entry->second.mode) & PROT_CPU_READ) == 0 &&
			     !VirtualMemory::IsExecute(entry->second.mode))) return false;
			cursor = std::min(end, entry->second.vaddr + entry->second.size);
		}
		return true;
	}

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

	bool ProtectMappedUnlocked(uint64_t vaddr, uint64_t size, VirtualMemory::Mode mode,
	                           bool semantic = false) {
		const auto end = vaddr + size;
		uint64_t translated = 0;
		if (!TranslateGuestMemory(vaddr, size, &translated)) return false;
		if (translated != vaddr &&
		    (VirtualMemory::IsExecute(mode) ||
		     (semantic && (!IsPageAligned(vaddr) || !IsPageAligned(size))))) return false;
#if defined(__APPLE__) && defined(LUMEN_PS5_DIRECT_FEX)
		if (semantic) {
			// Verify the complete span before changing either native protection or
			// guest metadata. ELF segments may have finer boundaries than a host page.
			for (auto cursor = vaddr; cursor < end;) {
				const auto entry = FindMappedContainingUnlocked(cursor);
				if (entry == m_mapped.end()) return false;
				cursor = std::min(end, entry->second.vaddr + entry->second.size);
			}
			const auto data_permissions = [](VirtualMemory::Mode permissions) {
				auto bits = static_cast<uint32_t>(permissions);
				if ((bits & 6u) != 0) bits |= 1u; // native write/guest execute require readable data
				return bits & 3u;
			};
			const auto host_size = static_cast<uint64_t>(getpagesize());
			const auto representable = [&](uint64_t page) {
				const auto page_end = page + host_size;
				const auto requested = data_permissions(mode);
				for (auto cursor = page; cursor < page_end;) {
					const auto entry = FindMappedContainingUnlocked(cursor);
					if (entry == m_mapped.end()) return false;
					auto limit = std::min(page_end, entry->second.vaddr + entry->second.size);
					if (cursor < vaddr) limit = std::min(limit, vaddr);
					else if (cursor < end) limit = std::min(limit, end);
					if ((cursor < vaddr || cursor >= end) &&
					    data_permissions(entry->second.mode) != requested) return false;
					cursor = limit;
				}
				return true;
			};
			if ((vaddr % host_size != 0 && !representable(AlignDown(vaddr, host_size))) ||
			    (end % host_size != 0 && !representable(AlignDown(end, host_size)))) return false;
		}
#endif
		if (semantic) {
			// A reader must remain valid throughout RW->R (and RW->RW). Unlike
			// identity memory, every translated access faults, so publishing NONE
			// transiently would turn such valid concurrent accesses into crashes.
			IntersectTranslatedRightsUnlocked(vaddr, size, mode);
#if defined(KYTY_BACKING_MUTATION_TESTS)
			if (g_test_translated_mapping_phase) g_test_translated_mapping_phase(vaddr, size, false);
#endif
		}
		auto       it  = m_mapped.upper_bound(vaddr);
		if (it != m_mapped.begin()) {
			--it;
		}
		for (; it != m_mapped.end() && it->second.vaddr < end; ++it) {
			const auto& region      = it->second;
			const auto  range_start = std::max(vaddr, region.vaddr);
			const auto  range_end   = std::min(end, region.vaddr + region.size);
			auto host_mode = semantic && VirtualMemory::IsExecute(mode)
			                           ? static_cast<VirtualMemory::Mode>(static_cast<int>(mode) | PROT_CPU_READ)
			                           : mode;
#if defined(__APPLE__) && defined(LUMEN_PS5_DIRECT_FEX)
			if (!semantic) {
				// Watch removal only restores rights allowed by the guest mapping.
				auto guest_mode = static_cast<uint32_t>(region.mode);
				if (VirtualMemory::IsExecute(region.mode)) guest_mode |= PROT_CPU_READ;
				host_mode = static_cast<VirtualMemory::Mode>(static_cast<uint32_t>(mode) & guest_mode);
			}
#endif
			if (range_start < range_end &&
			    !ProtectTranslatedHost(range_start, range_end - range_start, host_mode)) {
				if (semantic) RestoreTranslatedRightsUnlocked(vaddr, size);
				return false;
			}
		}
		if (semantic) {
			// Split only changed spans, preserving the old rights on both sides.
			auto current = vaddr;
			while (current < end) {
				const auto entry = FindMappedContainingUnlocked(current);
				if (entry == m_mapped.end()) return false;
				const auto original = entry->second;
				const auto limit = std::min(end, original.vaddr + original.size);
				EXIT_IF(!RemoveMappedUnlocked(current, limit - current, original.type, false));
#if defined(KYTY_BACKING_MUTATION_TESTS)
				if (g_test_translated_mapping_phase) g_test_translated_mapping_phase(current, limit - current, true);
#endif
				EXIT_IF(!AddMappedUnlocked(current, limit - current, original.type, mode));
				current = limit;
			}
		}
		return true;
	}

	void IntersectTranslatedRightsUnlocked(uint64_t vaddr, uint64_t size, VirtualMemory::Mode mode) {
		if (!IsGuestNonIdentity(vaddr, size)) return;
		for (auto cursor = vaddr; cursor < vaddr + size;) {
			const auto it = FindMappedContainingUnlocked(cursor);
			EXIT_IF(it == m_mapped.end());
			const auto end = std::min(vaddr + size, it->second.vaddr + it->second.size);
			PublishTranslatedGuestRights(cursor, end - cursor,
			    0x80u | (static_cast<uint8_t>(it->second.mode) & static_cast<uint8_t>(mode)));
			cursor = end;
		}
	}

	void RestoreTranslatedRightsUnlocked(uint64_t vaddr, uint64_t size) {
		for (auto cursor = vaddr; cursor < vaddr + size;) {
			const auto it = FindMappedContainingUnlocked(cursor);
			if (it == m_mapped.end()) break;
			const auto end = std::min(vaddr + size, it->second.vaddr + it->second.size);
			PublishTranslatedGuestRights(cursor, end - cursor,
			    0x80u | static_cast<uint8_t>(it->second.mode));
			cursor = end;
		}
	}

	bool ConsumeUnlocked(uint64_t vaddr, uint64_t size) {
		auto it = FindContaining(vaddr, size);
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

	bool AddMappedUnlocked(uint64_t vaddr, uint64_t size, MappingType type, VirtualMemory::Mode mode) {
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
		const bool added = m_mapped.emplace(vaddr, MappedRegion {vaddr, size, type, mode}).second;
		if (added) PublishTranslatedGuestRights(vaddr, size, 0x80u | static_cast<uint8_t>(mode));
		return added;
	}

	bool RemoveMappedUnlocked(uint64_t vaddr, uint64_t size, MappingType type, bool revoke = true) {
		if (!ContainsMappedUnlocked(vaddr, size, type)) {
			return false;
		}
		const auto end     = vaddr + size;
		if (revoke) PublishTranslatedGuestRights(vaddr, size, 0);
		auto       current = vaddr;
		while (current < end) {
			auto       it       = FindMappedContainingUnlocked(current);
			const auto original = it->second;
			const auto part_end = std::min(end, original.vaddr + original.size);
			m_mapped.erase(it);
			if (original.vaddr < current) {
				const auto left_size = current - original.vaddr;
				EXIT_IF(!AddMappedUnlocked(original.vaddr, left_size, original.type, original.mode));
			}
			if (part_end < original.vaddr + original.size) {
				const auto right_size = original.vaddr + original.size - part_end;
				EXIT_IF(!AddMappedUnlocked(part_end, right_size, original.type, original.mode));
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
#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX
		std::vector<std::pair<uint64_t, uint64_t>> occupied;
#if defined(__APPLE__)
		std::vector<std::pair<uint64_t, uint64_t>> inaccessible;
		uint64_t pagezero_end = 0;
		// PAGEZERO is absent from mach_vm_region's inventory. Exclude its slid
		// guard explicitly, as well as live host mappings, before reserving gaps.
		const auto* header = _dyld_get_image_header(0);
		EXIT_IF(header == nullptr || header->magic != MH_MAGIC_64);
		auto* commands = reinterpret_cast<const uint8_t*>(header) + sizeof(mach_header_64);
		size_t remaining = header->sizeofcmds;
		for (uint32_t index = 0; index < header->ncmds; ++index) {
			EXIT_IF(remaining < sizeof(load_command));
			const auto* command = reinterpret_cast<const load_command*>(commands);
			EXIT_IF(command->cmdsize < sizeof(load_command) || command->cmdsize > remaining);
			if (command->cmd == LC_SEGMENT_64) {
				EXIT_IF(command->cmdsize < sizeof(segment_command_64));
				const auto* segment = reinterpret_cast<const segment_command_64*>(command);
				if (std::strncmp(segment->segname, SEG_PAGEZERO, sizeof(segment->segname)) == 0) {
					const auto slide = _dyld_get_image_vmaddr_slide(0);
					EXIT_IF(slide < 0 || segment->vmsize > UINT64_MAX - segment->vmaddr ||
					        static_cast<uint64_t>(slide) > UINT64_MAX - segment->vmaddr - segment->vmsize);
					occupied.emplace_back(segment->vmaddr, segment->vmaddr + segment->vmsize + slide);
					pagezero_end = std::max(pagezero_end, segment->vmaddr + segment->vmsize + slide);
				}
			}
			commands += command->cmdsize;
			remaining -= command->cmdsize;
		}
		mach_vm_address_t cursor = 0;
		for (;;) {
			mach_vm_address_t address = cursor;
			mach_vm_size_t size = 0;
			vm_region_basic_info_data_64_t info {};
			mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
			mach_port_t object = MACH_PORT_NULL;
			const auto status = mach_vm_region(mach_task_self(), &address, &size,
			    VM_REGION_BASIC_INFO_64, reinterpret_cast<vm_region_info_t>(&info), &count, &object);
			if (object != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), object);
			if (status == KERN_INVALID_ADDRESS) break;
			EXIT_IF(status != KERN_SUCCESS || size == 0 || size > UINT64_MAX - address ||
			        address + size <= cursor);
			occupied.emplace_back(address, address + size);
#if defined(LUMEN_PS5_DIRECT_FEX) && defined(__aarch64__)
			// Only permanent OS reservations are representable by fault translation.
			// User guards, readable commpage PTEs and PAGEZERO remain host-owned.
			uint64_t translation_end = 0;
			if (DarwinPermanentTranslationEnd(address, size, info, &translation_end)) {
				for (const auto& [guest_begin, guest_end]: regions) {
					const auto begin = AlignUp(std::max({uint64_t(address), guest_begin, pagezero_end}), PageSize());
					const auto end = AlignDown(std::min(translation_end, guest_end), PageSize());
					if (begin < end) inaccessible.emplace_back(begin, end);
				}
			}
#endif
			cursor = address + size;
		}
		std::sort(occupied.begin(), occupied.end());
#else
		FILE*                                      maps = fopen("/proc/self/maps", "r");
		EXIT_IF(maps == nullptr);

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
#if defined(__APPLE__) && defined(LUMEN_PS5_DIRECT_FEX)
			mach_vm_address_t target = start;
			if (mach_vm_allocate(mach_task_self(), &target, size, VM_FLAGS_FIXED) != KERN_SUCCESS)
				return; // a new host mapping won this gap; never replace it
			if (mach_vm_protect(mach_task_self(), target, size, false, VM_PROT_NONE) != KERN_SUCCESS) {
				mach_vm_deallocate(mach_task_self(), target, size);
				return;
			}
#else
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
#endif

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
#if defined(__APPLE__) && defined(LUMEN_PS5_DIRECT_FEX) && defined(__aarch64__)
		for (const auto& [guest_begin, guest_end]: inaccessible) {
			const auto bytes = guest_end - guest_begin;
			mach_vm_address_t host = HOST_USER_MAX + 1;
			if (mach_vm_allocate(mach_task_self(), &host, bytes, VM_FLAGS_ANYWHERE) != KERN_SUCCESS)
				EXIT("guest translation arena allocation failed: guest=0x%016" PRIx64 " size=0x%016" PRIx64 "\n",
				     guest_begin, bytes);
			const bool overlaps_guest = std::any_of(regions.begin(), regions.end(), [&](const auto& range) {
				return host < range.second && range.first < host + bytes;
			});
			if (overlaps_guest || (host & (PageSize() - 1)) != 0 ||
			    mach_vm_protect(mach_task_self(), host, bytes, false, VM_PROT_NONE) != KERN_SUCCESS) {
				mach_vm_deallocate(mach_task_self(), host, bytes);
				EXIT("guest translation arena native reservation is not representable\n");
			}
			GuestTranslationArena arena {guest_begin, host, bytes,
			    std::make_unique<std::atomic<uint8_t>[]>(bytes / TranslatedPageSize)};
			m_translation_arenas.ranges.push_back(std::move(arena));
			// Keep identity/translated boundaries distinct even when guest ranges
			// are adjacent; a single native mapping must never cross an arena.
			EXIT_IF(!m_free.emplace(guest_begin, bytes).second);
			m_owned.emplace_back(guest_begin, bytes);
			LOGF("PS5_GUEST_VA_ARENA guest=0x%016" PRIx64 " host=0x%016" PRIx64
			     " size=0x%016" PRIx64 " source=darwin-permanent-reservation\n", guest_begin, uint64_t(host), bytes);
		}
#endif
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
	}

	uint64_t FindFreeAlignedUnlocked(uint64_t search_start, uint64_t search_end, uint64_t size,
	                                 uint64_t alignment, bool allow_translated = true) {
		if (search_start >= search_end || size > search_end - search_start) {
			return 0;
		}
		for (const auto& [base, range_size]: m_free) {
			const auto candidate = AlignUp(std::max(base, search_start), alignment);
			uint64_t host = 0;
			if (!TranslateGuestMemory(candidate, size, &host) ||
			    (!allow_translated && host != candidate)) continue;
			if (candidate < search_end && size <= search_end - candidate &&
			    GuestFreeRangeContains(base, range_size, candidate, size)) {
				return candidate;
			}
		}
		return 0;
	}

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
			uint64_t host = 0;
			if (prev->first + prev->second != base ||
			    !TranslateGuestMemory(prev->first, end - prev->first, &host)) {
				break;
			}
			base  = prev->first;
			first = prev;
		}

		auto last = std::next(it);
		while (last != m_free.end() && end == last->first) {
			uint64_t host = 0;
			if (!TranslateGuestMemory(base, last->first + last->second - base, &host)) break;
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
		uint64_t host = 0;
		EXIT_IF(!TranslateGuestMemory(vaddr, size, &host));
		void* ptr = mmap(reinterpret_cast<void*>(host), size, PROT_NONE,
		                 MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
		EXIT_IF(ptr == MAP_FAILED || reinterpret_cast<uint64_t>(ptr) != host);
		PublishTranslatedHostRights(vaddr, size, VirtualMemory::Mode::NoAccess);
#else
		(void)vaddr;
		(void)size;
#endif
	}

	std::shared_mutex                          m_mutex;
	std::map<uint64_t, uint64_t>               m_free;
	std::map<uint64_t, MappedRegion>            m_mapped;
	std::vector<std::pair<uint64_t, uint64_t>> m_owned;
	GuestTranslationArenas                    m_translation_arenas;
	uint64_t                                   m_granularity = 0x10000;
	GuestBackingStore*                         m_backing     = nullptr;
};
