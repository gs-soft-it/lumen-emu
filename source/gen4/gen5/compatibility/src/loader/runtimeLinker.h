#ifndef EMULATOR_INCLUDE_EMULATOR_LOADER_RUNTIMELINKER_H_
#define EMULATOR_INCLUDE_EMULATOR_LOADER_RUNTIMELINKER_H_

#include "common/abi.h"
#include "common/common.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "loader/symbolDatabase.h"

#include <atomic>
#include <filesystem>
#include <memory>
#include <unordered_map>
#include <vector>

namespace Loader {

class Elf64;
struct Elf64_Sym;
struct Elf64_Rela;
class RuntimeLinker;

using module_func_t                          = KYTY_SYSV_ABI int (*)(size_t args, const void* argp);
using application_heap_free_func_t           = KYTY_SYSV_ABI void (*)(void*);
using application_heap_malloc_func_t         = KYTY_SYSV_ABI void* (*)(uint64_t);
using application_heap_posix_memalign_func_t = KYTY_SYSV_ABI int (*)(void**, uint64_t, uint64_t);

struct ModuleId {
	bool operator==(const ModuleId& other) const {
		return version_major == other.version_major && version_minor == other.version_minor &&
		       name == other.name;
	}

	std::string id;
	int         version_major;
	int         version_minor;
	std::string name;
};

struct LibraryId {
	bool operator==(const LibraryId& other) const {
		return version == other.version && name == other.name;
	}

	std::string id;
	int         version;
	std::string name;
};

struct ThreadLocalStorage {
	struct Block {
		uint8_t*                     ptr        = nullptr;
		application_heap_free_func_t free_func  = nullptr;
		bool                         vm_alloc   = false;
		uint64_t                     alloc_size = 0;
	};

	~ThreadLocalStorage();

	uint64_t image_vaddr   = 0;
	uint64_t init_size     = 0;
	uint64_t image_size    = 0;
	uint64_t tcb_offset    = 0;
	uint64_t handler_vaddr = 0;

	std::vector<uint8_t>           init_image;
	std::unordered_map<int, Block> tlss;
	Common::Mutex                  mutex;
};

struct DynamicInfo {
	void*    hash_table      = nullptr;
	uint64_t hash_table_size = 0;

	char*    str_table      = nullptr;
	uint64_t str_table_size = 0;

	Elf64_Sym* symbol_table            = nullptr;
	uint64_t   symbol_table_total_size = 0;
	uint64_t   symbol_table_entry_size = 0;

	uint64_t init_vaddr          = 0;
	uint64_t fini_vaddr          = 0;
	uint64_t init_array_vaddr    = 0;
	uint64_t fini_array_vaddr    = 0;
	uint64_t preinit_array_vaddr = 0;
	uint64_t init_array_size     = 0;
	uint64_t fini_array_size     = 0;
	uint64_t preinit_array_size  = 0;

	Elf64_Rela* jmprela_table      = nullptr;
	uint64_t    jmprela_table_size = 0;

	Elf64_Rela* rela_table            = nullptr;
	uint64_t    rela_table_total_size = 0;
	uint64_t    rela_table_entry_size = 0;

	uint64_t relative_count = 0;

	uint64_t debug   = 0;
	uint64_t textrel = 0;
	uint64_t flags   = 0;

	const char* so_name = nullptr;

	std::vector<const char*> needed;
	std::vector<ModuleId>    export_modules;
	std::vector<ModuleId>    import_modules;
	std::vector<LibraryId>   export_libs;
	std::vector<LibraryId>   import_libs;
};

struct Program {
	Program();
	~Program();

	int32_t                      unique_id = -1;
	RuntimeLinker*               rt        = nullptr;
	uint32_t                     load_count = 0; // Successful sceKernelLoadStartModule calls.
	std::atomic<uint32_t>         active_guest_calls {0};
	std::filesystem::path        file_name;
	std::unique_ptr<Elf64>       elf;
	std::unique_ptr<DynamicInfo> dynamic_info;
	uint64_t                     base_vaddr        = 0;
	uint64_t                     base_size         = 0;
	uint64_t                     base_size_aligned = 0;
	uint64_t                     mapped_size       = 0;
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	uint64_t red_zone_trampoline_vaddr = 0;
	uint64_t red_zone_trampoline_size  = 0;
#endif
	std::unique_ptr<SymbolDatabase> export_symbols;
	std::unique_ptr<SymbolDatabase> import_symbols;
	ThreadLocalStorage              tls;
	bool                            dbg_print_reloc             = false;
	std::vector<uint8_t>            rela_bits;
	uint64_t                        proc_param_vaddr            = 0;
};

class RuntimeLinker {
public:
	RuntimeLinker();
	virtual ~RuntimeLinker();
	void Clear();

	KYTY_CLASS_NO_COPY(RuntimeLinker);

	void DbgDump(const std::string& folder);

	Program* LoadProgram(const std::filesystem::path& elf_name);
	void     SaveMainProgram(const std::filesystem::path& elf_name);
	void     SaveProgram(Program* program, const std::filesystem::path& elf_name);
	void     UnloadProgram(Program* program);

	[[nodiscard]] uint64_t GetEntry();
	[[nodiscard]] uint64_t GetProcParam();

	void RelocateAll();
	void RelocateProgram(Program* program);
	void PreparePrograms();
	void SetUpstreamFunctionFallback(bool enabled);
	bool UpstreamFunctionFallbackEnabled() const { return m_upstream_function_fallback; }

	void  Execute(const std::filesystem::path& game_patch = {});
	int   StartModule(Program* program, size_t args, const void* argp, module_func_t func);
	int   StopModule(Program* program, size_t args, const void* argp, module_func_t func);
	void  StartAllModules();
	void  StopAllModules();
	void  DeleteTlss(int thread_id);
	void  SetApplicationHeapApi(void* const api[10]);
	void* ApplicationHeapMalloc(uint64_t size);
	void* ApplicationHeapMemalign(uint64_t alignment, uint64_t size);

	void Resolve(const std::string& name, SymbolType type, Program* program, SymbolRecord* out_info,
	             bool* bind_self);

	SymbolDatabase* Symbols() { return m_symbols.get(); }

	static uint64_t ReadFromElf(Program* program, uint64_t vaddr);
	Program*        FindProgramByAddr(uint64_t vaddr);
	Program*        FindProgramById(int32_t id);
	Program*        FindProgramByFileName(const std::filesystem::path& elf_name);

	static uint8_t* TlsGetAddr(Program* program, bool native_root_entry = false);
	static uint64_t CurrentThreadTlsBase(bool native_root_entry = false);
	// Cached, VM-owned main TLS extent; never creates a block. The caller owns
	// its HLE reader while capturing this native scalar snapshot.
	static bool CurrentThreadTlsExtent(uint64_t* base, uint64_t* size);
	uint8_t* GetThreadTlsAddress(Program* program, uint64_t offset);
	static void     DeleteTls(Program* program, int thread_id);

	void StackTrace(uint64_t frame_ptr, uint64_t stack_ptr);

private:
#if defined(LUMEN_PS5_FEXCORE)
	void PrepareCurrentThreadTls();
	friend bool TestNestedThreadTlsLookup(uint64_t stack_top);
#endif
	static void LoadProgramToMemory(Program* program);
	static void ParseProgramDynamicInfo(Program* program);
	static void CreateSymbolDatabase(Program* program);
	static void Relocate(Program* program);
	static void DeleteProgram(Program* program);
	static void SetupTlsHandler(Program* program);
	void        PreloadAdjacentPrograms();

	static const ModuleId*  FindModule(const Program& program, const std::string& id);
	static const LibraryId* FindLibrary(const Program& program, const std::string& id);

	std::vector<Program*>           m_programs;
	std::unique_ptr<SymbolDatabase> m_symbols;
	Common::Mutex                   m_mutex;
	bool                            m_programs_prepared = false;
	bool                            m_upstream_function_fallback = false;

	application_heap_malloc_func_t         m_application_heap_malloc         = nullptr;
	application_heap_free_func_t           m_application_heap_free           = nullptr;
	application_heap_posix_memalign_func_t m_application_heap_posix_memalign = nullptr;
};

#if defined(LUMEN_PS5_FEXCORE)
bool TestThreadTlsIsolation();
bool TestNestedThreadTlsLookup(uint64_t stack_top);
#endif

#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
bool TestMainEntryUsesGuestStack();
bool TestModuleRelocationUsesWritableHostMapping();
#endif

} // namespace Loader

#endif /* EMULATOR_INCLUDE_EMULATOR_LOADER_RUNTIMELINKER_H_ */
