#include "Emulator/Loader/SymbolDatabase.h"

#include "Lumen/Core/File.h"
#include "Lumen/Core/MagicEnum.h"
#include "Lumen/Core/Vector.h"

#if defined(LUMEN_ANDROID_FEXCORE_RUNTIME) && !defined(LUMEN_FEXCORE_RUNTIME)
#define LUMEN_FEXCORE_RUNTIME 1
#endif

#if defined(LUMEN_FEXCORE_RUNTIME)
#include "AndroidFexCoreRuntime.h"
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Loader {

namespace {

// Compact SHA-1 (FIPS 180-1) used only for PS4 NID derivation; keeps the
// loader independent of host crypto libraries on every target.
class SymbolNidSha1
{
public:
	SymbolNidSha1() = default;

	void Update(const uint8_t* data, size_t size)
	{
		m_length_bits += static_cast<uint64_t>(size) * 8u;
		while (size != 0)
		{
			const size_t room = sizeof(m_buffer) - m_buffered;
			const size_t take = size < room ? size : room;
			std::memcpy(m_buffer + m_buffered, data, take);
			m_buffered += take;
			data += take;
			size -= take;
			if (m_buffered == sizeof(m_buffer))
			{
				ProcessBlock(m_buffer);
				m_buffered = 0;
			}
		}
	}

	void Final(uint8_t out[20])
	{
		// The length field holds the original message length, so capture it
		// before appending the padding.
		const uint64_t message_bits = m_length_bits;
		const uint8_t terminator = 0x80;
		UpdateRaw(&terminator, 1);
		const uint8_t zero = 0;
		while (m_buffered != 56) UpdateRaw(&zero, 1);
		uint8_t length_be[8];
		for (int i = 0; i < 8; ++i) length_be[i] = static_cast<uint8_t>((message_bits >> (56 - i * 8)) & 0xffu);
		UpdateRaw(length_be, sizeof(length_be));
		for (int i = 0; i < 5; ++i)
		{
			out[i * 4 + 0] = static_cast<uint8_t>((m_state[i] >> 24) & 0xffu);
			out[i * 4 + 1] = static_cast<uint8_t>((m_state[i] >> 16) & 0xffu);
			out[i * 4 + 2] = static_cast<uint8_t>((m_state[i] >> 8) & 0xffu);
			out[i * 4 + 3] = static_cast<uint8_t>(m_state[i] & 0xffu);
		}
	}

private:
	void UpdateRaw(const uint8_t* data, size_t size)
	{
		while (size != 0)
		{
			const size_t room = sizeof(m_buffer) - m_buffered;
			const size_t take = size < room ? size : room;
			std::memcpy(m_buffer + m_buffered, data, take);
			m_buffered += take;
			data += take;
			size -= take;
			if (m_buffered == sizeof(m_buffer))
			{
				ProcessBlock(m_buffer);
				m_buffered = 0;
			}
		}
	}

	void ProcessBlock(const uint8_t* block)
	{
		uint32_t w[80];
		for (int i = 0; i < 16; ++i)
		{
			w[i] = (static_cast<uint32_t>(block[i * 4 + 0]) << 24) | (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
			       (static_cast<uint32_t>(block[i * 4 + 2]) << 8) | static_cast<uint32_t>(block[i * 4 + 3]);
		}
		for (int i = 16; i < 80; ++i)
		{
			const uint32_t v = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
			w[i] = (v << 1) | (v >> 31);
		}
		uint32_t a = m_state[0], b = m_state[1], c = m_state[2], d = m_state[3], e = m_state[4];
		for (int i = 0; i < 80; ++i)
		{
			uint32_t f, k;
			if (i < 20)
			{
				f = (b & c) | (~b & d);
				k = 0x5a827999u;
			} else if (i < 40)
			{
				f = b ^ c ^ d;
				k = 0x6ed9eba1u;
			} else if (i < 60)
			{
				f = (b & c) | (b & d) | (c & d);
				k = 0x8f1bbcdcu;
			} else
			{
				f = b ^ c ^ d;
				k = 0xca62c1d6u;
			}
			const uint32_t temp = ((a << 5) | (a >> 27)) + f + e + k + w[i];
			e = d;
			d = c;
			c = (b << 30) | (b >> 2);
			b = a;
			a = temp;
		}
		m_state[0] += a;
		m_state[1] += b;
		m_state[2] += c;
		m_state[3] += d;
		m_state[4] += e;
	}

	uint32_t m_state[5] {0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u, 0xc3d2e1f0u};
	uint64_t m_length_bits = 0;
	uint8_t  m_buffer[64] {};
	size_t   m_buffered = 0;
};

} // anonymous namespace

String ComputeSymbolNid(const char* name, size_t size)
{
	// Fixed suffix appended to the symbol name before hashing (PS4 NID).
	static constexpr uint8_t suffix[16] = {0x51, 0x8d, 0x64, 0xa6, 0x35, 0xde, 0xd8, 0xc1,
	                                       0xe6, 0xb0, 0x39, 0xb1, 0xc3, 0xe5, 0x52, 0x30};
	if (name == nullptr) return String();

	SymbolNidSha1 sha;
	sha.Update(reinterpret_cast<const uint8_t*>(name), size);
	sha.Update(suffix, sizeof(suffix));
	uint8_t digest[20];
	sha.Final(digest);

	// The first eight digest bytes are reversed, then encoded with the PS4
	// base64 alphabet ('-' instead of '/') without padding.
	uint8_t reversed[8];
	for (int i = 0; i < 8; ++i) reversed[i] = digest[7 - i];

	static constexpr char codes[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";
	char nid[12];
	int  out = 0;
	for (int i = 0; i + 3 <= 8; i += 3)
	{
		const uint32_t group = (static_cast<uint32_t>(reversed[i]) << 16) |
		                       (static_cast<uint32_t>(reversed[i + 1]) << 8) | static_cast<uint32_t>(reversed[i + 2]);
		nid[out++] = codes[(group >> 18) & 0x3f];
		nid[out++] = codes[(group >> 12) & 0x3f];
		nid[out++] = codes[(group >> 6) & 0x3f];
		nid[out++] = codes[group & 0x3f];
	}
	const uint32_t tail = (static_cast<uint32_t>(reversed[6]) << 16) | (static_cast<uint32_t>(reversed[7]) << 8);
	nid[out++] = codes[(tail >> 18) & 0x3f];
	nid[out++] = codes[(tail >> 12) & 0x3f];
	nid[out++] = codes[(tail >> 6) & 0x3f];
	return String::FromUtf8(nid, 11);
}

bool AuthoritativeHostCallRegistryEnabled()
{
	static const bool enabled = []
	{
		const char* value =
		    std::getenv("LUMEN_X64_AUTHORITATIVE_HOST_CALL_REGISTRY");
#if defined(LUMEN_FEXCORE_RUNTIME)
		// FEX cannot use native-image classification as a fallback: every native
		// AArch64 target must be published explicitly before guest execution.
		// Keep this fail-closed even if the process environment requests OFF.
		constexpr bool result = true;
#elif LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
		constexpr bool product_default = false;
		const bool result =
		    value == nullptr ? product_default : std::strcmp(value, "1") == 0;
#else
		constexpr bool product_default = true;
		const bool result =
		    value == nullptr ? product_default : std::strcmp(value, "1") == 0;
#endif
		std::fprintf(stderr,
		             "[AUTHORITATIVE_HLE_HOST_CALL_REGISTRY] requested=%s effective=%d mode=%s\n",
		             value == nullptr ? "absent" : value, result ? 1 : 0,
		             result ? "authoritative" : "native-image-fallback");
		std::fflush(stderr);
		return result;
	}();
	return enabled;
}

constexpr char32_t LIB_PREFIX[] = {0x0000006c, 0x00000069, 0x00000062, 0x00000053, 0x00000063, 0x00000065, 0};
constexpr char32_t LIB_OLD_4[]  = {0x00000047, 0x0000006e, 0x0000006d, 0};
constexpr char32_t LIB_NEW_4[]  = {0x00000047, 0x00000072, 0x00000061, 0x00000070, 0x00000068, 0x00000069, 0x00000063, 0x00000073, 0};

static String update_name(const String& str)
{
	auto ret = (str.StartsWith(LIB_PREFIX) ? str.RemoveFirst(6) : str);
	return ret.ReplaceStr(LIB_OLD_4, LIB_NEW_4);
}

String SymbolDatabase::GenerateName(const SymbolResolve& s)
{
	auto library = update_name(s.library);
	auto module  = update_name(s.module);
	return String::FromPrintf("%s[%s_v%d][%s_v%d.%d][%s]", s.name.C_Str(), library.C_Str(), s.library_version, module.C_Str(),
	                          s.module_version_major, s.module_version_minor, Core::EnumName(s.type).C_Str());
}

static String generate_exact_name(const SymbolResolve& s)
{
	return String::FromPrintf("%s[%s_v%d][%s_v%d.%d][%s]", s.name.C_Str(), s.library.C_Str(), s.library_version, s.module.C_Str(),
	                          s.module_version_major, s.module_version_minor, Core::EnumName(s.type).C_Str());
}

void SymbolDatabase::Add(const SymbolResolve& s, uint64_t vaddr)
{
	SymbolRecord r {};
	r.name  = GenerateName(s);
	r.vaddr = vaddr;
	m_map.Put(r.name, m_symbols.Size());
	m_nid_map.Put(s.name, m_symbols.Size());
	m_symbols.Add(r);
}

void SymbolDatabase::Add(const SymbolResolve& s, uint64_t vaddr, const String& dbg_name)
{
	SymbolRecord r {};
	r.name     = GenerateName(s);
	r.vaddr    = vaddr;
	r.dbg_name = dbg_name;
	m_map.Put(r.name, m_symbols.Size());
	m_nid_map.Put(s.name, m_symbols.Size());
	m_symbols.Add(r);
}

void SymbolDatabase::AddExact(const SymbolResolve& s, uint64_t vaddr)
{
	SymbolRecord r {};
	r.name  = generate_exact_name(s);
	r.vaddr = vaddr;
	m_exact_map.Put(r.name, m_symbols.Size());
	m_nid_map.Put(s.name, m_symbols.Size());
	m_symbols.Add(r);
}

void SymbolDatabase::AddExact(const SymbolResolve& s, uint64_t vaddr, const String& dbg_name)
{
	SymbolRecord r {};
	r.name     = generate_exact_name(s);
	r.vaddr    = vaddr;
	r.dbg_name = dbg_name;
	m_exact_map.Put(r.name, m_symbols.Size());
	m_nid_map.Put(s.name, m_symbols.Size());
	m_symbols.Add(r);
}

void SymbolDatabase::AddByStdName(const String& std_name, uint64_t vaddr, const String& dbg_name)
{
	SymbolRecord r {};
	r.name     = std_name;
	r.vaddr    = vaddr;
	r.dbg_name = dbg_name;
	m_std_name_map.Put(std_name, m_symbols.Size());
	m_symbols.Add(r);
}

bool SymbolDatabase::RegisterHostCallTarget(uint64_t vaddr)
{
	if (vaddr == 0) return false;
	const bool authoritative = AuthoritativeHostCallRegistryEnabled();
	if (authoritative &&
	    m_host_call_registry_sealed.load(std::memory_order_acquire))
	{
		return false;
	}
#if defined(LUMEN_FEXCORE_RUNTIME)
	// Publish the native target to FEX before the Lumen registry can make it
	// visible to guest execution. Failure is fatal to loader construction: FEX
	// must never try to decode an AArch64 function body as guest x86-64.
	if (LumenAndroidFexCoreRegisterHostCallTargetV1(vaddr) == 0) return false;
#endif
	// OFF intentionally accepts valid registrations as no-ops. This keeps
	// loader factories and LIB_ADD call sites unchanged while preserving an
	// empty, unsealed database for the native-image fallback path.
	if (!authoritative) return true;
	// Registration is single-threaded loader work. Once sealed, guest threads
	// may read the exact set concurrently and it must remain immutable.
	m_host_call_targets.Put(vaddr, 1);
	return true;
}

void SymbolDatabase::SealHostCallRegistry()
{
	if (!AuthoritativeHostCallRegistryEnabled()) return;
	// Loader construction is single-threaded. Never rebuild storage after the
	// release publication below: concurrent readers may borrow scalar metadata.
	if (m_host_call_registry_sealed.load(std::memory_order_acquire)) return;
	std::vector<HostCallDispatchEntry> pending;
	pending.reserve(static_cast<size_t>(m_host_call_targets.Size()) + m_host_call_gp_metadata.Size() +
	                m_host_call_scalar_indices.Size() + m_host_call_variadic_kinds.Size() + m_host_call_effects.Size());
	m_host_call_targets.ForEach([](const uint64_t* address, const uint8_t*, void* output) {
		auto& entries = *static_cast<std::vector<HostCallDispatchEntry>*>(output);
		HostCallDispatchEntry entry; entry.address = *address; entry.registered = true;
		entries.push_back(entry); return true;
	}, &pending);
	m_host_call_gp_metadata.ForEach([](const uint64_t* address, const HostCallGpMetadata* metadata, void* output) {
		auto& entries = *static_cast<std::vector<HostCallDispatchEntry>*>(output);
		HostCallDispatchEntry entry; entry.address = *address; entry.gp = *metadata;
		entries.push_back(entry); return true;
	}, &pending);
	m_host_call_scalar_indices.ForEach([](const uint64_t* address, const uint32_t* index, void* output) {
		auto& entries = *static_cast<std::vector<HostCallDispatchEntry>*>(output);
		HostCallDispatchEntry entry; entry.address = *address; entry.scalar_index = *index;
		entries.push_back(entry); return true;
	}, &pending);
	m_host_call_variadic_kinds.ForEach([](const uint64_t* address, const HostCallVariadicKind* kind, void* output) {
		auto& entries = *static_cast<std::vector<HostCallDispatchEntry>*>(output);
		HostCallDispatchEntry entry; entry.address = *address; entry.variadic = *kind;
		entries.push_back(entry); return true;
	}, &pending);
	m_host_call_effects.ForEach([](const uint64_t* address, const GuestCpu::HostCallEffect* effects, void* output) {
		auto& entries = *static_cast<std::vector<HostCallDispatchEntry>*>(output);
		HostCallDispatchEntry entry; entry.address = *address; entry.effects = *effects;
		entries.push_back(entry); return true;
	}, &pending);
	std::sort(pending.begin(), pending.end(), [](const auto& left, const auto& right) { return left.address < right.address; });
	m_host_call_dispatch.reserve(pending.size());
	for (const auto& entry: pending)
	{
		if (m_host_call_dispatch.empty() || m_host_call_dispatch.back().address != entry.address)
		{
			m_host_call_dispatch.push_back(entry);
			continue;
		}
		auto& merged = m_host_call_dispatch.back();
		merged.registered |= entry.registered;
		if (entry.scalar_index != UINT32_MAX) merged.scalar_index = entry.scalar_index;
		if (entry.gp.invoker != nullptr) merged.gp = entry.gp;
		if (entry.variadic != HostCallVariadicKind::None) merged.variadic = entry.variadic;
		if (entry.effects != GuestCpu::HostCallEffect::Unknown) merged.effects = entry.effects;
	}
	m_host_call_fast_index_enabled = GuestCpu::HostCallFastIndexEnabled();
	if (m_host_call_fast_index_enabled)
	{
		std::vector<uint64_t> addresses;
		addresses.reserve(m_host_call_dispatch.size());
		for (const auto& entry: m_host_call_dispatch) addresses.push_back(entry.address);
		m_host_call_fast_index = GuestCpu::HostCallFastIndex(addresses);
		m_host_call_fast_index_enabled = m_host_call_fast_index.IsUsable();
	}
	// Publish every completed LIB_FUNC/LIB_FUNC_STD insertion before guest Run
	// is allowed to treat a miss as authoritative guest code.
	m_host_call_registry_sealed.store(true, std::memory_order_release);
}

HostCallDispatchMetadata SymbolDatabase::ResolveHostCall(uint64_t vaddr) const
{
	HostCallDispatchMetadata result;
	if (!m_host_call_registry_sealed.load(std::memory_order_acquire))
	{
		// Preserve the mutable loader and registry-OFF paths. No snapshot or
		// borrowed cache survives an insertion or another database's lifetime.
		result.scalar = HostCallScalarMetadataFor(vaddr);
		result.gp = HostCallGpMetadataFor(vaddr);
		result.variadic = HostCallVariadicKindFor(vaddr);
		result.effects = m_host_call_effects.Get(vaddr, GuestCpu::HostCallEffect::Unknown);
		return result;
	}
	result.classification = HostCallRegistryClassification::Guest;
	const HostCallDispatchEntry* entry = nullptr;
	if (m_host_call_fast_index_enabled)
	{
		const size_t index = m_host_call_fast_index.Find(vaddr);
		if (index == GuestCpu::HostCallFastIndex::NotFound) return result;
		entry = &m_host_call_dispatch[index];
	}
	else
	{
		const auto found = std::lower_bound(m_host_call_dispatch.begin(), m_host_call_dispatch.end(), vaddr,
		    [](const auto& candidate, uint64_t address) { return candidate.address < address; });
		if (found == m_host_call_dispatch.end() || found->address != vaddr) return result;
		entry = &*found;
	}
	// Metadata alone never publishes a host target.
	if (entry->registered) result.classification = HostCallRegistryClassification::Host;
	if (entry->scalar_index != UINT32_MAX) result.scalar = &m_host_call_scalar_metadata[entry->scalar_index];
	result.gp = entry->gp;
	result.variadic = entry->variadic;
	result.effects = entry->effects;
	return result;
}

bool SymbolDatabase::SetHostCallEffects(uint64_t vaddr, GuestCpu::HostCallEffect effects)
{
	if (vaddr == 0 || HostCallRegistrySealed() || !GuestCpu::ValidHostCallEffects(effects)) return false;
	if (effects == GuestCpu::HostCallEffect::None)
	{
		// The light bridge must never touch unused/overflow guest arguments.
		const auto* scalar = HostCallScalarMetadataFor(vaddr);
		if (scalar == nullptr || scalar->arity > 6 ||
		    (scalar->result != HostCallScalarType::Gp && scalar->result != HostCallScalarType::Void)) return false;
		for (uint32_t index = 0; index < scalar->arity; ++index)
			if (scalar->arguments[index] != HostCallScalarType::Gp) return false;
	}
	const auto previous = m_host_call_effects.Get(vaddr, GuestCpu::HostCallEffect::Unknown);
	if (previous != GuestCpu::HostCallEffect::Unknown && previous != effects) return false;
	m_host_call_effects.Put(vaddr, effects);
	return true;
}

HostCallRegistryClassification SymbolDatabase::ClassifyHostCallTarget(uint64_t vaddr) const
{
	if (!m_host_call_registry_sealed.load(std::memory_order_acquire))
	{
		return HostCallRegistryClassification::Unsealed;
	}
	return vaddr != 0 && m_host_call_targets.Contains(vaddr)
	           ? HostCallRegistryClassification::Host
	           : HostCallRegistryClassification::Guest;
}

bool SymbolDatabase::SetHostCallGpMetadata(uint64_t vaddr, HostCallGpMetadata metadata)
{
	if (vaddr == 0 || HostCallRegistrySealed() || metadata.arity < 9 || metadata.arity > 12 ||
	    metadata.invoker == nullptr || m_host_call_scalar_indices.Contains(vaddr) ||
	    m_host_call_variadic_kinds.Contains(vaddr)) return false;
	if (const auto* previous = m_host_call_gp_metadata.Find(vaddr); previous != nullptr)
	{
		return previous->arity == metadata.arity && previous->invoker == metadata.invoker;
	}
	m_host_call_gp_metadata.Put(vaddr, metadata);
	return true;
}

HostCallGpMetadata SymbolDatabase::HostCallGpMetadataFor(uint64_t vaddr) const
{
	return m_host_call_gp_metadata.Get(vaddr, {});
}

bool SymbolDatabase::SetHostCallScalarMetadata(uint64_t vaddr, const HostCallScalarMetadata& metadata)
{
	if (vaddr == 0 || HostCallRegistrySealed() || metadata.arity > HOST_CALL_SCALAR_MAX_ARITY ||
	    metadata.invoker == nullptr || metadata.result == HostCallScalarType::Unsupported ||
	    static_cast<unsigned>(metadata.result) > static_cast<unsigned>(HostCallScalarType::Unsupported) ||
	    m_host_call_gp_metadata.Contains(vaddr) || m_host_call_variadic_kinds.Contains(vaddr)) return false;
	for (uint32_t index = 0; index < metadata.arity; ++index)
	{
		if (metadata.arguments[index] != HostCallScalarType::Gp &&
		    metadata.arguments[index] != HostCallScalarType::Float32 &&
		    metadata.arguments[index] != HostCallScalarType::Float64) return false;
	}
	if (const auto* previous = HostCallScalarMetadataFor(vaddr); previous != nullptr)
	{
		return previous->arity == metadata.arity && previous->arguments == metadata.arguments &&
		       previous->result == metadata.result && previous->invoker == metadata.invoker;
	}
	// A caller may register another alias using a descriptor borrowed from
	// this vector. Preserve it before Add can reallocate that storage.
	const auto owned_metadata = metadata;
	m_host_call_scalar_indices.Put(vaddr, m_host_call_scalar_metadata.Size());
	m_host_call_scalar_metadata.Add(owned_metadata);
	return true;
}

const HostCallScalarMetadata* SymbolDatabase::HostCallScalarMetadataFor(uint64_t vaddr) const
{
	if (m_host_call_scalar_metadata.IsEmpty()) return nullptr;
	const auto* index = m_host_call_scalar_indices.Find(vaddr);
	return index != nullptr ? &m_host_call_scalar_metadata[*index] : nullptr;
}

bool SymbolDatabase::SetHostCallVariadicKind(uint64_t vaddr, HostCallVariadicKind kind)
{
	if (vaddr == 0 || HostCallRegistrySealed() ||
	    (kind != HostCallVariadicKind::Printf && kind != HostCallVariadicKind::Snprintf) ||
	    m_host_call_gp_metadata.Contains(vaddr) || m_host_call_scalar_indices.Contains(vaddr)) return false;
	if (const auto* previous = m_host_call_variadic_kinds.Find(vaddr); previous != nullptr)
		return *previous == kind;
	m_host_call_variadic_kinds.Put(vaddr, kind);
	return true;
}

HostCallVariadicKind SymbolDatabase::HostCallVariadicKindFor(uint64_t vaddr) const
{
	return m_host_call_variadic_kinds.Get(vaddr, HostCallVariadicKind::None);
}

void SymbolDatabase::DbgDump(const String& folder, const String& file_name)
{
	auto folder_str = folder.FixDirectorySlash();

	Core::File::CreateDirectories(folder_str);

	Core::File f;
	f.Create(folder_str + file_name);

	for (const auto& sym: m_symbols)
	{
		f.Printf("%" PRIx64 " %s\n", sym.vaddr, sym.name.C_Str());
	}

	f.Close();
}

const SymbolRecord* SymbolDatabase::Find(const SymbolResolve& s) const
{
	auto index = m_exact_map.Get(generate_exact_name(s), decltype(m_symbols)::INVALID_INDEX);
	if (m_symbols.IndexValid(index))
	{
		return &m_symbols.At(index);
	}
	index = m_map.Get(GenerateName(s), decltype(m_symbols)::INVALID_INDEX);
	if (!m_symbols.IndexValid(index))
	{
		return nullptr;
	}
	return &m_symbols.At(index);
}

const SymbolRecord* SymbolDatabase::FindByStdName(const String& std_name) const
{
	auto index = m_std_name_map.Get(std_name, decltype(m_symbols)::INVALID_INDEX);
	if (!m_symbols.IndexValid(index))
	{
		return nullptr;
	}
	return &m_symbols.At(index);
}

const SymbolRecord* SymbolDatabase::FindByNid(const String& nid) const
{
	auto index = m_nid_map.Get(nid, decltype(m_symbols)::INVALID_INDEX);
	if (!m_symbols.IndexValid(index))
	{
		return nullptr;
	}
	return &m_symbols.At(index);
}

} // namespace Lumen::Loader

#endif // LUMEN_EMU_ENABLED
