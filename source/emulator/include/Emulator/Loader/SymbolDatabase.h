#ifndef EMULATOR_INCLUDE_EMULATOR_LOADER_SYMBOLDATABASE_H_
#define EMULATOR_INCLUDE_EMULATOR_LOADER_SYMBOLDATABASE_H_

#include "Lumen/Core/Common.h"
#include "Lumen/Core/Hashmap.h"
#include "Lumen/Core/String.h"
#include "Lumen/Core/Vector.h"

#include "Emulator/Common.h"
#include "Emulator/Loader/HostCallScalar.h"
#include "HostCallEffects.h"
#include "HostCallFastIndex.h"

#include <atomic>
#include <vector>

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Loader {

enum class SymbolType
{
	Unknown,
	Func,
	Object,
	TlsModule,
	NoType,
};

struct SymbolRecord
{
	String   name;
	String   dbg_name;
	uint64_t vaddr;
};

struct SymbolResolve
{
	String     name;
	String     library;
	int        library_version;
	String     module;
	int        module_version_major;
	int        module_version_minor;
	SymbolType type;
};

enum class HostCallRegistryClassification: uint8_t
{
	Unsealed,
	Guest,
	Host,
};

enum class HostCallVariadicKind: uint8_t { None, Printf, Snprintf };

struct HostCallDispatchMetadata
{
	HostCallRegistryClassification classification = HostCallRegistryClassification::Unsealed;
	const HostCallScalarMetadata* scalar = nullptr;
	HostCallGpMetadata gp {};
	HostCallVariadicKind variadic = HostCallVariadicKind::None;
	GuestCpu::HostCallEffect effects = GuestCpu::HostCallEffect::Unknown;
};

// Compute the plain PS4 NID of a symbol name: SHA-1 over the name followed by
// the fixed 16-byte suffix, first eight digest bytes reversed and encoded in
// the PS4 base64 variant ('-' instead of '/', no padding). sceKernelDlsym
// performs this conversion before searching a module's export table.
[[nodiscard]] String ComputeSymbolNid(const char* name, size_t size);

// The promoted macOS baseline uses the conservative native-image classifier.
// Exact one opts back into the authoritative registry for the whole process;
// other platforms retain their previous default-on behavior until separately
// validated.
[[nodiscard]] bool AuthoritativeHostCallRegistryEnabled();

class SymbolDatabase
{
public:
	SymbolDatabase()          = default;
	virtual ~SymbolDatabase() = default;

	void Add(const SymbolResolve& s, uint64_t vaddr);
	void Add(const SymbolResolve& s, uint64_t vaddr, const String& dbg_name);
	// Preserve the complete symbol identity without legacy library/module
	// normalization. Exact entries are not added to the normalized map.
	void AddExact(const SymbolResolve& s, uint64_t vaddr);
	void AddExact(const SymbolResolve& s, uint64_t vaddr, const String& dbg_name);

	// Add symbol with standard name (POSIX-style, for OpenOrbis homebrew)
	void AddByStdName(const String& std_name, uint64_t vaddr, const String& dbg_name = U"");
	[[nodiscard]] bool RegisterHostCallTarget(uint64_t vaddr);
	void               SealHostCallRegistry();
	bool SetHostCallGpMetadata(uint64_t vaddr, HostCallGpMetadata metadata);
	[[nodiscard]] bool SetHostCallScalarMetadata(uint64_t vaddr, const HostCallScalarMetadata& metadata);
	// Borrowed until the next scalar insertion; stable after the registry seal.
	[[nodiscard]] const HostCallScalarMetadata* HostCallScalarMetadataFor(uint64_t vaddr) const;
	[[nodiscard]] bool SetHostCallVariadicKind(uint64_t vaddr, HostCallVariadicKind kind);
	[[nodiscard]] HostCallVariadicKind HostCallVariadicKindFor(uint64_t vaddr) const;
	[[nodiscard]] bool SetHostCallEffects(uint64_t vaddr, GuestCpu::HostCallEffect effects);

	[[nodiscard]] const SymbolRecord* Find(const SymbolResolve& s) const;

	// Find symbol by standard name (POSIX-style, for OpenOrbis homebrew)
	[[nodiscard]] const SymbolRecord* FindByStdName(const String& std_name) const;

	// Find an exported symbol by its plain PS4 NID (the base name before any
	// '#library#module' suffix). First match wins within this database.
	[[nodiscard]] const SymbolRecord* FindByNid(const String& nid) const;
	[[nodiscard]] HostCallRegistryClassification ClassifyHostCallTarget(uint64_t vaddr) const;
	[[nodiscard]] HostCallGpMetadata  HostCallGpMetadataFor(uint64_t vaddr) const;
	// One lookup in the sealed immutable table supplies classification and ABI.
	// Scalar descriptors retain their original database-owned storage/lifetime.
	[[nodiscard]] HostCallDispatchMetadata ResolveHostCall(uint64_t vaddr) const;

	[[nodiscard]] uint32_t StdNameMapSize() const { return m_std_name_map.Size(); }
	[[nodiscard]] uint32_t HostCallTargetCount() const { return m_host_call_targets.Size(); }
	[[nodiscard]] bool     HostCallRegistrySealed() const
	{
		return m_host_call_registry_sealed.load(std::memory_order_acquire);
	}

	void DbgDump(const String& folder, const String& file_name);

	LUMEN_CLASS_NO_COPY(SymbolDatabase);

	static String GenerateName(const SymbolResolve& s);

private:
	struct HostCallDispatchEntry
	{
		uint64_t address = 0;
		uint32_t scalar_index = UINT32_MAX;
		bool registered = false;
		HostCallGpMetadata gp {};
		HostCallVariadicKind variadic = HostCallVariadicKind::None;
		GuestCpu::HostCallEffect effects = GuestCpu::HostCallEffect::Unknown;
	};
	Vector<SymbolRecord>                       m_symbols;
	Core::Hashmap<String, uint32_t>            m_map;
	Core::Hashmap<String, uint32_t>            m_nid_map;  // Maps plain PS4 NIDs like "LwG8g3niqwA"
	Core::Hashmap<String, uint32_t>            m_exact_map;
	Core::Hashmap<String, uint32_t>            m_std_name_map;  // Maps standard names like "pthread_mutex_lock"
	Core::Hashmap<uint64_t, uint8_t>           m_host_call_targets;
	Core::Hashmap<uint64_t, HostCallGpMetadata> m_host_call_gp_metadata;
	Core::Hashmap<uint64_t, uint32_t>          m_host_call_scalar_indices;
	Vector<HostCallScalarMetadata>             m_host_call_scalar_metadata;
	Core::Hashmap<uint64_t, HostCallVariadicKind> m_host_call_variadic_kinds;
	Core::Hashmap<uint64_t, GuestCpu::HostCallEffect> m_host_call_effects;
	std::vector<HostCallDispatchEntry>           m_host_call_dispatch;
	GuestCpu::HostCallFastIndex                 m_host_call_fast_index;
	bool                                      m_host_call_fast_index_enabled = false;
	std::atomic<bool>                           m_host_call_registry_sealed {false};
};

} // namespace Lumen::Loader

#endif // LUMEN_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_LOADER_SYMBOLDATABASE_H_ */
