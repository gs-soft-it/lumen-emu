#include "Lumen/Core/Threads.h"
#include "Lumen/Core/VirtualMemory.h"

#include "Emulator/Config.h"
#include "Emulator/Libs/ImeDialog.h"
#include "Emulator/Libs/ImeDialogInput.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/HostCallScalar.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "Emulator/Log.h"

#include <SDL.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <type_traits>

namespace {

namespace Ime = Lumen::Libs::Dialog::ImeDialog;
namespace Memory = Lumen::Core::VirtualMemory;
using ScalarType = Lumen::Loader::HostCallScalarType;
using InitFunction = int32_t (LUMEN_SYSV_ABI*)(const Ime::Param*, const Ime::ExtendedParam*);
static_assert(std::is_same_v<decltype(&Ime::ImeDialogInit), InitFunction>);
static_assert(sizeof(Ime::Param) == 0x60 && offsetof(Ime::Param, input_text_buffer) == 0x28);
static_assert(sizeof(Ime::Result) == 0x10 && sizeof(Ime::ExtendedParam) == 0x88);

constexpr uint64_t PAGE_BYTES = 65536;
constexpr uint64_t OFFSET = 128;
constexpr int32_t ERROR_BUSY = static_cast<int32_t>(0x80bc0001);
constexpr int32_t ERROR_INVALID_PARAM = static_cast<int32_t>(0x80bc0030);
constexpr int32_t ERROR_INVALID_EXTENDED = static_cast<int32_t>(0x80bc001c);
constexpr int32_t ERROR_NOT_FINISHED = static_cast<int32_t>(0x80bc0106);
constexpr int32_t ERROR_NOT_IN_USE = static_cast<int32_t>(0x80bc0107);
uint32_t g_checks = 0;

bool Check(bool condition, const char* message)
{
	++g_checks;
	if (!condition) std::fprintf(stderr, "ImeDialogTest: %s\n", message);
	return condition;
}

struct Page
{
	uint64_t address = Memory::Alloc(0, PAGE_BYTES, Memory::Mode::ReadWrite);
	~Page() { if (address != 0) (void)Memory::Free(address); }
	Page() = default;
	Page(const Page&) = delete;
	Page& operator=(const Page&) = delete;
	template <typename T> T* At(uint64_t offset = OFFSET) const { return reinterpret_cast<T*>(address + offset); }
	void Fill(uint8_t value) const { std::memset(reinterpret_cast<void*>(address), value, PAGE_BYTES); }
};

struct Fixture
{
	Page params, extended, text, result, panel;
	bool ready = false;
	const uint32_t allocated_text_capacity;
	std::array<uint8_t, PAGE_BYTES> original_text {};
	Fixture(std::u16string_view seed = u"World", uint32_t capacity = 16): allocated_text_capacity(capacity)
	{
		Ime::ResetSessionState();
		ready = params.address != 0 && extended.address != 0 && text.address != 0 && result.address != 0 && panel.address != 0;
		if (!ready) return;
		params.Fill(0xa5); extended.Fill(0xa5); text.Fill(0xa5); result.Fill(0xa5); panel.Fill(0xa5);
		*Param() = {};
		Param()->user_id = 1;
		Param()->type = Ime::Type::Default;
		Param()->option = 2; // Public no-auto-capitalize option, no device selection.
		Param()->max_text_length = capacity;
		Param()->input_text_buffer = Text();
		std::memset(Text(), 0, (capacity + 1) * sizeof(char16_t));
		std::memcpy(Text(), seed.data(), seed.size() * sizeof(char16_t));
		*Extended() = {};
		*Result() = {};
		Result()->endstatus = static_cast<Ime::EndStatus>(0x12345678);
		std::memcpy(original_text.data(), reinterpret_cast<const void*>(text.address), PAGE_BYTES);
	}
	~Fixture() { Ime::ResetSessionState(); }
	Ime::Param* Param() const { return params.At<Ime::Param>(); }
	Ime::ExtendedParam* Extended() const { return extended.At<Ime::ExtendedParam>(); }
	char16_t* Text() const { return text.At<char16_t>(); }
	Ime::Result* Result() const { return result.At<Ime::Result>(); }
	bool Init() const { return ready && Check(Ime::ImeDialogInit(Param(), nullptr) == 0, "valid managed Init failed"); }
	bool Unchanged() const { return std::memcmp(original_text.data(), reinterpret_cast<const void*>(text.address), PAGE_BYTES) == 0; }
	bool TextEquals(std::u16string_view expected) const
	{
		return std::memcmp(Text(), expected.data(), expected.size() * sizeof(char16_t)) == 0 && Text()[expected.size()] == 0;
	}
	bool TextCanaries() const
	{
		const auto* bytes = reinterpret_cast<const uint8_t*>(text.address);
		const uint64_t end = OFFSET + (allocated_text_capacity + 1) * sizeof(char16_t);
		for (uint64_t i = 0; i < PAGE_BYTES; ++i) if ((i < OFFSET || i >= end) && bytes[i] != 0xa5) return false;
		return true;
	}
};

bool Snapshot(Ime::HostSnapshot* snapshot, std::u16string_view expected)
{
	return Check(Ime::GetHostSnapshot(snapshot), "running host snapshot missing") &&
	    Check(snapshot->text == expected, "host UTF16 text differs");
}

bool Finish(Fixture& fixture, uint64_t generation, Ime::EndStatus expected)
{
	const bool accepted = expected == Ime::EndStatus::Ok ? Ime::HostAccept(generation) :
	    expected == Ime::EndStatus::UserCanceled ? Ime::HostCancel(generation) : Ime::ImeDialogAbort() == 0;
	return Check(accepted, "host/guest finish refused") &&
	    Check(Ime::ImeDialogGetStatus() == 2, "finish did not publish Finished") &&
	    Check(Ime::ImeDialogGetResult(fixture.Result()) == 0, "finished result refused") &&
	    Check(fixture.Result()->endstatus == expected, "finished result has wrong endstatus");
}

bool TestRegistration()
{
	Lumen::Loader::SymbolDatabase symbols;
	if (!Check(Lumen::Libs::Init(U"libSceImeDialog_1", &symbols), "ImeDialog library not selected")) return false;
	struct Binding { const char* nid; uint64_t target; uint8_t arity; };
	const Binding bindings[] = {
	    {"NUeBrN7hzf0", reinterpret_cast<uint64_t>(&Ime::ImeDialogInit), 2},
	    {"IADmD4tScBY", reinterpret_cast<uint64_t>(&Ime::ImeDialogGetStatus), 0},
	    {"gyTyVn+bXMw", reinterpret_cast<uint64_t>(&Ime::ImeDialogTerm), 0},
	    {"x01jxu+vxlc", reinterpret_cast<uint64_t>(&Ime::ImeDialogGetResult), 1},
	    {"oBmw4xrmfKs", reinterpret_cast<uint64_t>(&Ime::ImeDialogAbort), 0},
	    {"wqsJvRXwl58", reinterpret_cast<uint64_t>(&Ime::ImeDialogGetPanelSize), 3},
	    {"CRD+jSErEJQ", reinterpret_cast<uint64_t>(&Ime::ImeDialogGetPanelSizeExtended), 4},
	    {"8jqzzPioYl8", reinterpret_cast<uint64_t>(&Ime::ImeDialogGetPanelPositionAndForm), 1},
	};
	for (const auto& binding: bindings)
	{
		Lumen::Loader::SymbolResolve query {};
		query.name = binding.nid; query.library = "libSceImeDialog"; query.library_version = 1;
		query.module = "libSceImeDialog"; query.module_version_major = query.module_version_minor = 1;
		query.type = Lumen::Loader::SymbolType::Func;
		const auto* record = symbols.Find(query);
		if (!Check(record != nullptr && record->vaddr == binding.target, "exact PS4 NID binding missing")) return false;
		const auto* scalar = symbols.HostCallScalarMetadataFor(record->vaddr);
		if (!Check(scalar != nullptr && scalar->arity == binding.arity && scalar->result == ScalarType::Gp && scalar->invoker != nullptr,
		           "FEX scalar metadata missing or wrong")) return false;
		query.module = "libSceIme";
		if (!Check(symbols.Find(query) == nullptr, "ImeDialog leaked into wrong module")) return false;
	}
	return Check(symbols.HostCallTargetCount() == 8, "unexpected ImeDialog registration count");
}

bool TestLifecycleAndUnicode()
{
	Fixture fixture(u"A", 4);
	if (!Check(fixture.ready, "VM allocation failed") ||
	    !Check(Ime::ImeDialogGetStatus() == 0 && Ime::ImeDialogAbort() == ERROR_NOT_IN_USE &&
	        Ime::ImeDialogTerm() == ERROR_NOT_IN_USE && Ime::ImeDialogGetResult(fixture.Result()) == ERROR_NOT_IN_USE,
	        "no-session lifecycle differs") || !fixture.Init()) return false;
	Ime::HostSnapshot snapshot;
	if (!Check(Ime::ImeDialogGetStatus() == 1, "Init did not publish Running") || !Snapshot(&snapshot, u"A") ||
	    !Check(Ime::ImeDialogInit(fixture.Param(), nullptr) == ERROR_BUSY &&
	        Ime::ImeDialogGetResult(fixture.Result()) == ERROR_NOT_FINISHED && Ime::ImeDialogTerm() == ERROR_NOT_FINISHED,
	        "busy/unfinished lifecycle differs")) return false;
	const uint64_t generation = snapshot.generation;
	const char16_t malformed[] = {0xd800};
	if (!Check(!Ime::HostInsertText(generation, {malformed, 1}), "isolated surrogate admitted") ||
	    !Check(Ime::HostInsertText(generation, u"\U0001f600B"), "valid Unicode insertion failed") || !Snapshot(&snapshot, u"A\U0001f600B") ||
	    !Check(!Ime::HostInsertText(generation, u"C"), "capacity overflow admitted") ||
	    !Check(Ime::HostBackspace(generation), "backspace failed") || !Snapshot(&snapshot, u"A\U0001f600") ||
	    !Check(Ime::HostMoveCursor(generation, -1), "cursor left failed") ||
	    !Check(Ime::GetHostSnapshot(&snapshot) && snapshot.cursor == 1, "cursor split surrogate pair") ||
	    !Check(Ime::HostBackspace(generation), "prefix deletion failed") || !Snapshot(&snapshot, u"\U0001f600") ||
	    !Check(Ime::HostMoveCursor(generation, 1), "cursor right failed") ||
	    !Check(Ime::GetHostSnapshot(&snapshot) && snapshot.cursor == 2, "cursor right split surrogate pair") ||
	    !Check(Ime::HostBackspace(generation), "surrogate backspace failed") || !Snapshot(&snapshot, u"") ||
	    !Check(Ime::HostInsertText(generation, u"abc\U0001f600"), "bounded insertion failed") || !Snapshot(&snapshot, u"abc") ||
	    !Check(Ime::ImeDialogGetStatus() == 1 && fixture.Unchanged(), "Running edits wrote guest text") ||
	    !Finish(fixture, generation, Ime::EndStatus::Ok) ||
	    !Check(fixture.TextEquals(u"abc") && fixture.TextCanaries(), "accepted text or buffer canaries differ") ||
	    !Check(!Ime::HostInsertText(generation, u"late") && !Ime::HostAccept(generation), "Finished accepted stale host input") ||
	    !Check(Ime::ImeDialogTerm() == 0 && Ime::ImeDialogGetStatus() == 0, "Term failed")) return false;
	return true;
}

bool TestCancelAbortAndReset()
{
	for (const auto end: {Ime::EndStatus::UserCanceled, Ime::EndStatus::Aborted})
	{
		Fixture fixture;
		Ime::HostSnapshot snapshot;
		if (!fixture.Init() || !Snapshot(&snapshot, u"World") || !Check(Ime::HostInsertText(snapshot.generation, u" edited"), "edit failed") ||
		    !Finish(fixture, snapshot.generation, end) || !Check(fixture.Unchanged(), "cancel/abort changed original guest text") ||
		    !Check(Ime::ImeDialogTerm() == 0, "cancel/abort Term failed")) return false;
	}
	Fixture fixture;
	Ime::HostSnapshot before, after;
	if (!fixture.Init() || !Snapshot(&before, u"World")) return false;
	Ime::ResetSessionState();
	if (!Check(Ime::ImeDialogGetStatus() == 0 && !Ime::HostInsertText(before.generation, u"stale") &&
	    !Ime::HostAccept(before.generation), "reset retained old host authority") || !fixture.Init() || !Snapshot(&after, u"World") ||
	    !Check(after.generation != before.generation && !Ime::HostCancel(before.generation) &&
	        !Ime::HostMoveCursor(before.generation, -1), "new session accepted old generation") ||
	    !Finish(fixture, after.generation, Ime::EndStatus::UserCanceled)) return false;
	return Check(fixture.Unchanged(), "session reset changed guest text");
}

bool TestFullTextReplacement()
{
	Fixture fixture(u"Seed", 8);
	Ime::HostSnapshot before, snapshot;
	if (!fixture.Init() || !Snapshot(&before, u"Seed")) return false;
	Ime::ResetSessionState();
	if (!fixture.Init() || !Snapshot(&snapshot, u"Seed") ||
	    !Check(snapshot.generation != before.generation, "replacement session did not advance generation")) return false;
	const uint64_t generation = snapshot.generation;
	if (!Check(Ime::HostReplaceText(generation, u"A\U0001f600B", 3), "valid full Unicode replacement refused") ||
	    !Snapshot(&snapshot, u"A\U0001f600B") || !Check(snapshot.cursor == 3 && fixture.Unchanged(), "full replacement cursor or staging differs") ||
	    !Check(Ime::HostReplaceText(generation, u"", 0), "empty full replacement refused") ||
	    !Snapshot(&snapshot, u"") || !Check(snapshot.cursor == 0 && fixture.Unchanged(), "empty replacement cursor or staging differs") ||
	    !Check(Ime::HostReplaceText(generation, u"Ready", 2), "full replacement after clear refused") ||
	    !Snapshot(&snapshot, u"Ready") || !Check(snapshot.cursor == 2, "full replacement did not retain valid cursor")) return false;
	const auto reject = [&](uint64_t token, std::u16string_view text, uint32_t cursor, const char* message) {
		return Check(!Ime::HostReplaceText(token, text, cursor), message) &&
		    Check(Ime::GetHostSnapshot(&snapshot) && snapshot.generation == generation && snapshot.text == u"Ready" && snapshot.cursor == 2,
		        "rejected replacement changed editor state") &&
		    Check(Ime::ImeDialogGetStatus() == 1 && fixture.Unchanged(), "rejected replacement wrote guest text or finished dialog");
	};
	const char16_t invalid_utf16[] = {0xd800};
	if (!reject(before.generation, u"Late", 0, "stale generation full replacement admitted") ||
	    !reject(generation, u"123456789", 9, "over-capacity full replacement admitted") ||
	    !reject(generation, {invalid_utf16, 1}, 1, "malformed UTF16 full replacement admitted") ||
	    !reject(generation, {u"A\0B", 3}, 3, "embedded NUL full replacement admitted") ||
	    !reject(generation, u"A\nB", 3, "single-line full replacement admitted newline") ||
	    !reject(generation, u"A\U0001f600B", 2, "full replacement cursor split surrogate pair") ||
	    !reject(generation, u"ok", 3, "full replacement cursor beyond text admitted") ||
	    !Finish(fixture, generation, Ime::EndStatus::Ok) ||
	    !Check(fixture.TextEquals(u"Ready") && fixture.TextCanaries(), "full replacement final text/canaries differ")) return false;
	{
		Fixture clear_fixture(u"Seed", 8);
		Ime::HostSnapshot clear_snapshot;
		if (!clear_fixture.Init() || !Snapshot(&clear_snapshot, u"Seed") ||
		    !Check(Ime::HostReplaceText(clear_snapshot.generation, u"", 0) && clear_fixture.Unchanged(), "clear edit wrote guest text before accept") ||
		    !Finish(clear_fixture, clear_snapshot.generation, Ime::EndStatus::Ok) ||
		    !Check(clear_fixture.TextEquals(u"") && clear_fixture.TextCanaries(), "accepted empty replacement did not clear text")) return false;
	}
	return true;
}

bool TestInvalidInputs()
{
	enum class Invalid { NullParam, UnalignedParam, InaccessibleParam, NullText, UnalignedText, ReadOnlyText,
	    InaccessibleText, UnterminatedText, MalformedUtf16, ZeroCapacity, OverCapacity, Filter, ExtFilter, Dictionary,
	    DisabledKeyboard, BadReserved };
	for (const auto invalid: {Invalid::NullParam, Invalid::UnalignedParam, Invalid::InaccessibleParam, Invalid::NullText,
	    Invalid::UnalignedText, Invalid::ReadOnlyText, Invalid::InaccessibleText, Invalid::UnterminatedText,
	    Invalid::MalformedUtf16, Invalid::ZeroCapacity, Invalid::OverCapacity, Invalid::Filter, Invalid::ExtFilter,
	    Invalid::Dictionary, Invalid::DisabledKeyboard, Invalid::BadReserved})
	{
		Fixture fixture;
		if (!Check(fixture.ready, "VM allocation failed")) return false;
		const Ime::Param* param = fixture.Param();
		const Ime::ExtendedParam* extended = nullptr;
		std::array<uint8_t, PAGE_BYTES> text_before = fixture.original_text;
		bool parameter_protected = false, text_protected = false;
		switch (invalid)
		{
			case Invalid::NullParam: param = nullptr; break;
			case Invalid::UnalignedParam: param = reinterpret_cast<const Ime::Param*>(fixture.params.address + OFFSET + 1); break;
			case Invalid::InaccessibleParam:
				parameter_protected = Memory::Protect(fixture.params.address, PAGE_BYTES, Memory::Mode::NoAccess); break;
			case Invalid::NullText: fixture.Param()->input_text_buffer = nullptr; break;
			case Invalid::UnalignedText: fixture.Param()->input_text_buffer = reinterpret_cast<char16_t*>(fixture.text.address + OFFSET + 1); break;
			case Invalid::ReadOnlyText: text_protected = Memory::Protect(fixture.text.address, PAGE_BYTES, Memory::Mode::Read); break;
			case Invalid::InaccessibleText: text_protected = Memory::Protect(fixture.text.address, PAGE_BYTES, Memory::Mode::NoAccess); break;
			case Invalid::UnterminatedText:
				for (uint32_t i = 0; i <= fixture.Param()->max_text_length; ++i) fixture.Text()[i] = u'x'; break;
			case Invalid::MalformedUtf16: fixture.Text()[0] = 0xd800; fixture.Text()[1] = 0; break;
			case Invalid::ZeroCapacity: fixture.Param()->max_text_length = 0; break;
			case Invalid::OverCapacity: fixture.Param()->max_text_length = 2049; break;
			case Invalid::Filter: fixture.Param()->filter = reinterpret_cast<decltype(fixture.Param()->filter)>(uintptr_t {0x1234}); break;
			case Invalid::ExtFilter:
				fixture.Extended()->ext_keyboard_filter = reinterpret_cast<decltype(fixture.Extended()->ext_keyboard_filter)>(uintptr_t {0x1234});
				extended = fixture.Extended(); break;
			case Invalid::Dictionary: fixture.Extended()->additional_dictionary_path = reinterpret_cast<const char*>(uintptr_t {0x1234});
				extended = fixture.Extended(); break;
			case Invalid::DisabledKeyboard: fixture.Extended()->disable_device = 2; extended = fixture.Extended(); break;
			case Invalid::BadReserved: fixture.Param()->reserved[0] = 1; break;
		}
		if ((invalid == Invalid::InaccessibleParam && !Check(parameter_protected, "Protect param failed")) ||
		    ((invalid == Invalid::ReadOnlyText || invalid == Invalid::InaccessibleText) && !Check(text_protected, "Protect text failed"))) return false;
		if (!text_protected) std::memcpy(text_before.data(), reinterpret_cast<const void*>(fixture.text.address), PAGE_BYTES);
		const int32_t rc = Ime::ImeDialogInit(param, extended);
		if (parameter_protected && !Check(Memory::Protect(fixture.params.address, PAGE_BYTES, Memory::Mode::ReadWrite), "Restore param failed")) return false;
		if (text_protected && !Check(Memory::Protect(fixture.text.address, PAGE_BYTES, Memory::Mode::ReadWrite), "Restore text failed")) return false;
		const bool no_session = Ime::ImeDialogGetStatus() == 0;
		const bool unchanged = std::memcmp(text_before.data(), reinterpret_cast<const void*>(fixture.text.address), PAGE_BYTES) == 0;
		const bool canaries = fixture.TextCanaries();
		if (rc == 0 || !no_session || !unchanged || !canaries)
		{
			constexpr const char* names[] = {"NullParam", "UnalignedParam", "InaccessibleParam", "NullText", "UnalignedText",
			    "ReadOnlyText", "InaccessibleText", "UnterminatedText", "MalformedUtf16", "ZeroCapacity", "OverCapacity",
			    "Filter", "ExtFilter", "Dictionary", "DisabledKeyboard", "BadReserved"};
			uint64_t first_difference = PAGE_BYTES;
			const auto* actual = reinterpret_cast<const uint8_t*>(fixture.text.address);
			for (uint64_t i = 0; i < PAGE_BYTES; ++i) if (actual[i] != text_before[i]) { first_difference = i; break; }
			std::fprintf(stderr, "[IME_DIALOG_INVALID_INIT_FAILURE] case=%u name=%s rc=%08x no_session=%u"
			    " full_page_unchanged=%u canaries=%u first_difference=%llu original_capacity=%u param_capacity=%u\n",
			    static_cast<unsigned>(invalid), names[static_cast<unsigned>(invalid)], static_cast<uint32_t>(rc), no_session ? 1u : 0u,
			    unchanged ? 1u : 0u, canaries ? 1u : 0u, static_cast<unsigned long long>(first_difference),
			    fixture.allocated_text_capacity, fixture.Param()->max_text_length);
		}
		if (!Check(rc != 0 && no_session, "invalid Init admitted a session") ||
		    !Check(unchanged, "invalid Init modified text allocation") || !Check(canaries, "invalid Init modified buffer canaries") ||
		    (invalid == Invalid::Filter && !Check(rc == ERROR_INVALID_PARAM, "filter refusal differs")) ||
		    ((invalid == Invalid::ExtFilter || invalid == Invalid::Dictionary || invalid == Invalid::DisabledKeyboard) &&
		        !Check(rc == ERROR_INVALID_EXTENDED, "extended callback/dictionary/disabled-keyboard refusal differs"))) return false;
	}
	return true;
}

bool TestContinuity()
{
	enum class Change { ReadOnly, ProtectRestore, Remap, Unrelated };
	for (const auto change: {Change::ReadOnly, Change::ProtectRestore, Change::Remap, Change::Unrelated})
	{
		Fixture fixture(u"Seed");
		Ime::HostSnapshot snapshot;
		if (!fixture.Init() || !Snapshot(&snapshot, u"Seed") || !Check(Ime::HostInsertText(snapshot.generation, u"!"), "continuity edit failed")) return false;
		std::array<uint8_t, PAGE_BYTES> expected = fixture.original_text;
		if (change == Change::ReadOnly || change == Change::ProtectRestore)
		{
			if (!Check(Memory::Protect(fixture.text.address, PAGE_BYTES, Memory::Mode::Read), "text revocation failed")) return false;
			if (change == Change::ProtectRestore && !Check(Memory::Protect(fixture.text.address, PAGE_BYTES, Memory::Mode::ReadWrite), "text restoration failed")) return false;
		}
		if (change == Change::Remap)
		{
			if (!Check(Memory::Free(fixture.text.address) && Memory::AllocFixed(fixture.text.address, PAGE_BYTES, Memory::Mode::ReadWrite), "text remap failed")) return false;
			fixture.text.Fill(0x5a); expected.fill(0x5a);
		}
		if (change == Change::Unrelated)
		{
			Page unrelated;
			if (!Check(unrelated.address != 0, "unrelated allocation failed")) return false;
		}
		(void)Ime::HostAccept(snapshot.generation);
		if (!Check(Ime::ImeDialogGetStatus() == 2, "continuity outcome not Finished") ||
		    !Check(Ime::ImeDialogGetResult(fixture.Result()) == 0, "continuity Result failed") ||
		    !Check(fixture.Result()->endstatus == (change == Change::Unrelated ? Ime::EndStatus::Ok : Ime::EndStatus::Aborted), "continuity outcome differs")) return false;
		if (change == Change::Unrelated)
		{
			if (!Check(fixture.TextEquals(u"Seed!") && fixture.TextCanaries(), "disjoint mutation prevented valid commit")) return false;
		}
		else if (!Check(std::memcmp(expected.data(), reinterpret_cast<const void*>(fixture.text.address), PAGE_BYTES) == 0, "revoked/reused mapping received stale text")) return false;
		if (!Check(Ime::ImeDialogTerm() == 0, "continuity Term failed")) return false;
	}
	return true;
}

bool TestOutputsAndPanels()
{
	Fixture fixture;
	Ime::HostSnapshot snapshot;
	if (!fixture.Init() || !Snapshot(&snapshot, u"World")) return false;
	auto* width = fixture.panel.At<uint32_t>();
	auto* height = fixture.panel.At<uint32_t>(OFFSET + 8);
	if (!Check(Ime::ImeDialogGetPanelSize(fixture.Param(), width, height) == 0 && *width > 0 && *height > 0, "panel size failed") ||
	    !Check(Ime::ImeDialogGetPanelSizeExtended(fixture.Param(), fixture.Extended(), width, height) == 0, "extended panel size failed")) return false;
	auto* form = fixture.panel.At<Ime::PositionAndForm>(OFFSET + 32);
	if (!Check(Ime::ImeDialogGetPanelPositionAndForm(form) == 0 && form->width == *width && form->height == *height, "panel form differs") ||
	    !Finish(fixture, snapshot.generation, Ime::EndStatus::Ok)) return false;
	*fixture.Result() = {};
	fixture.Result()->endstatus = static_cast<Ime::EndStatus>(0x12345678);
	std::array<uint8_t, PAGE_BYTES> result_before;
	std::memcpy(result_before.data(), reinterpret_cast<const void*>(fixture.result.address), PAGE_BYTES);
	if (!Check(Memory::Protect(fixture.result.address, PAGE_BYTES, Memory::Mode::Read), "result Protect failed") ||
	    !Check(Ime::ImeDialogGetResult(fixture.Result()) != 0 &&
	        std::memcmp(result_before.data(), reinterpret_cast<const void*>(fixture.result.address), PAGE_BYTES) == 0, "read-only result was partly written") ||
	    !Check(Memory::Protect(fixture.result.address, PAGE_BYTES, Memory::Mode::ReadWrite), "result Restore failed") ||
	    !Check(Ime::ImeDialogGetResult(nullptr) != 0, "null result admitted") ||
	    !Check(Ime::ImeDialogGetResult(reinterpret_cast<Ime::Result*>(fixture.result.address + OFFSET + 1)) != 0 &&
	        std::memcmp(result_before.data(), reinterpret_cast<const void*>(fixture.result.address), PAGE_BYTES) == 0, "unaligned result admitted or wrote memory")) return false;
	fixture.Result()->reserved[0] = 1;
	if (!Check(Ime::ImeDialogGetResult(fixture.Result()) == static_cast<int32_t>(0x80bc0032), "reserved result bytes admitted")) return false;
	fixture.Result()->reserved[0] = 0;
	if (!Check(Ime::ImeDialogGetResult(fixture.Result()) == 0, "result recovery failed")) return false;
	Page invalid_height;
	if (!Check(invalid_height.address != 0, "panel allocation failed")) return false;
	invalid_height.Fill(0xa5); *width = 0x12345678;
	if (!Check(Memory::Protect(invalid_height.address, PAGE_BYTES, Memory::Mode::Read), "height Protect failed") ||
	    !Check(Ime::ImeDialogGetPanelSize(fixture.Param(), width, invalid_height.At<uint32_t>()) != 0 && *width == 0x12345678,
	        "panel failure published partial width")) return false;
	return Check(Ime::ImeDialogTerm() == 0, "output Term failed");
}

SDL_Event TextEvent(uint32_t type, std::string_view text)
{
	SDL_Event event {};
	event.type = type;
	if (type == SDL_TEXTINPUT)
		std::memcpy(event.text.text, text.data(), text.size());
	else
	{
		std::memcpy(event.edit.text, text.data(), text.size());
		event.edit.start = 0; event.edit.length = static_cast<int32_t>(text.size());
	}
	return event;
}

SDL_Event KeyEvent(uint32_t type, SDL_Keycode key, SDL_Scancode scancode)
{
	SDL_Event event {};
	event.type = type; event.key.state = type == SDL_KEYDOWN ? SDL_PRESSED : SDL_RELEASED;
	event.key.keysym.sym = key; event.key.keysym.scancode = scancode;
	return event;
}

bool InitSdlFixture(Fixture& fixture)
{
	if (!fixture.Init()) return false;
	Ime::SyncSdlInput(nullptr);
	return true;
}

bool TestSdlInputBridge()
{
	// Only the shared event parser is called: no SDL initialization, window,
	// input device, polling or controller path participates in these fixtures.
	{
		Fixture fixture(u"A", 12);
		Ime::HostSnapshot snapshot;
		if (!InitSdlFixture(fixture) || !Snapshot(&snapshot, u"A") ||
		    !Check(!Ime::ProcessSdlEvent(KeyEvent(SDL_KEYUP, SDLK_LEFT, SDL_SCANCODE_LEFT)),
		        "unpaired key release was consumed")) return false;
		if (!Check(Ime::ProcessSdlEvent(TextEvent(SDL_TEXTEDITING, "preview")), "preedit was not consumed") ||
		    !Snapshot(&snapshot, u"A") || !Check(fixture.Unchanged(), "preedit changed guest buffer") ||
		    !Check(Ime::HostCaption().find("preview") != std::string::npos, "preedit is missing from caption")) return false;
		if (!Check(Ime::ProcessSdlEvent(TextEvent(SDL_TEXTINPUT, "\xc3\xa9\xf0\x9f\x98\x80")), "UTF8 input was not consumed") ||
		    !Snapshot(&snapshot, u"A\u00e9\U0001f600") || !Check(fixture.Unchanged(), "SDL staging changed guest buffer") ||
		    !Check(Ime::HostCaption().find("preview") == std::string::npos, "committed input retained preedit caption")) return false;
		for (const std::string_view invalid: {std::string_view {"\xc0\xaf"}, std::string_view {"\xed\xa0\x80"}, std::string_view {"\xf0\x9f\x98"}})
		{
			(void)Ime::ProcessSdlEvent(TextEvent(SDL_TEXTINPUT, invalid));
			if (!Snapshot(&snapshot, u"A\u00e9\U0001f600") || !Check(fixture.Unchanged(), "invalid UTF8 changed guest buffer")) return false;
		}
		if (!Check(Ime::ProcessSdlEvent(KeyEvent(SDL_KEYDOWN, SDLK_RETURN, SDL_SCANCODE_RETURN)), "Return was not consumed") ||
		    !Check(Ime::ProcessSdlEvent(KeyEvent(SDL_KEYUP, SDLK_RETURN, SDL_SCANCODE_RETURN)), "paired Return release escaped after finish") ||
		    !Check(!Ime::ProcessSdlEvent(KeyEvent(SDL_KEYUP, SDLK_RETURN, SDL_SCANCODE_RETURN)), "repeated Return release remained consumed") ||
		    !Check(Ime::ImeDialogGetStatus() == 2 && Ime::ImeDialogGetResult(fixture.Result()) == 0 &&
		        fixture.Result()->endstatus == Ime::EndStatus::Ok, "SDL Return did not accept dialog") ||
		    !Check(fixture.TextEquals(u"A\u00e9\U0001f600") && fixture.TextCanaries(), "SDL accepted text/canaries differ")) return false;
	}
	{
		Fixture fixture(u"Seed");
		Ime::HostSnapshot snapshot;
		if (!InitSdlFixture(fixture) || !Check(Ime::ProcessSdlEvent(TextEvent(SDL_TEXTINPUT, " edited")), "cancel edit was not consumed") ||
		    !Snapshot(&snapshot, u"Seed edited") ||
		    !Check(Ime::ProcessSdlEvent(KeyEvent(SDL_KEYDOWN, SDLK_ESCAPE, SDL_SCANCODE_ESCAPE)), "Escape was not consumed") ||
		    !Check(Ime::ProcessSdlEvent(KeyEvent(SDL_KEYUP, SDLK_ESCAPE, SDL_SCANCODE_ESCAPE)), "paired Escape release escaped") ||
		    !Check(Ime::ImeDialogGetStatus() == 2 && Ime::ImeDialogGetResult(fixture.Result()) == 0 &&
		        fixture.Result()->endstatus == Ime::EndStatus::UserCanceled && fixture.Unchanged(), "Escape did not preserve original text")) return false;
	}
	{
		Fixture fixture(u"Secret");
		fixture.Param()->type = Ime::Type::BasicLatin;
		fixture.Param()->option |= 4; // Public password option.
		Ime::HostSnapshot snapshot;
		if (!InitSdlFixture(fixture) || !Snapshot(&snapshot, u"Secret") ||
		    !Check(Ime::ProcessSdlEvent(TextEvent(SDL_TEXTEDITING, "hidden")), "password preedit was not consumed") ||
		    !Check(Ime::HostCaption().find("Secret") == std::string::npos && Ime::HostCaption().find("hidden") == std::string::npos,
		        "password caption revealed text or preedit") ||
		    !Check(Ime::ProcessSdlEvent(TextEvent(SDL_TEXTINPUT, "X")), "password edit was not consumed") || !Snapshot(&snapshot, u"SecretX") ||
		    !Check(Ime::HostCaption().find("Secret") == std::string::npos && fixture.Unchanged(), "password staging exposed or wrote text") ||
		    !Finish(fixture, snapshot.generation, Ime::EndStatus::UserCanceled)) return false;
	}
	{
		Fixture fixture(u"A", 4);
		Ime::HostSnapshot snapshot;
		if (!InitSdlFixture(fixture) || !Check(Ime::ProcessSdlEvent(TextEvent(SDL_TEXTINPUT, "\xf0\x9f\x98\x80" "B")), "bounded SDL edit not consumed") ||
		    !Snapshot(&snapshot, u"A\U0001f600B")) return false;
		(void)Ime::ProcessSdlEvent(TextEvent(SDL_TEXTINPUT, "overflow"));
		if (!Snapshot(&snapshot, u"A\U0001f600B") || !Check(fixture.Unchanged(), "capacity refusal wrote guest text") ||
		    !Finish(fixture, snapshot.generation, Ime::EndStatus::Ok) ||
		    !Check(fixture.TextEquals(u"A\U0001f600B") && fixture.TextCanaries(), "bounded SDL final text/canaries differ")) return false;
	}
	return true;
}

bool TestSdlOwnershipAndGeneration()
{
	{
		Fixture fixture(u"Owned");
		Ime::SyncSdlInput(nullptr);
		auto down = KeyEvent(SDL_KEYDOWN, SDLK_LEFT, SDL_SCANCODE_LEFT);
		if (!Check(!Ime::ProcessSdlEvent(down), "inactive keydown was consumed")) return false;
		Ime::HostSnapshot snapshot;
		if (!InitSdlFixture(fixture) || !Snapshot(&snapshot, u"Owned")) return false;
		const uint32_t cursor = snapshot.cursor;
		down.key.repeat = 1;
		if (!Check(Ime::ProcessSdlEvent(down), "pre-dialog repeat was not suppressed") ||
		    !Check(Ime::GetHostSnapshot(&snapshot) && snapshot.cursor == cursor && snapshot.text == u"Owned",
		        "pre-dialog repeat transferred key ownership to editor") ||
		    !Check(!Ime::ProcessSdlEvent(KeyEvent(SDL_KEYUP, SDLK_LEFT, SDL_SCANCODE_LEFT)),
		        "release of forwarded key was consumed") ||
		    !Finish(fixture, snapshot.generation, Ime::EndStatus::UserCanceled)) return false;
	}
	{
		Fixture fixture(u"Owned");
		Ime::HostSnapshot snapshot;
		auto down = KeyEvent(SDL_KEYDOWN, SDLK_RIGHT, SDL_SCANCODE_RIGHT);
		if (!InitSdlFixture(fixture) || !Snapshot(&snapshot, u"Owned") ||
		    !Check(Ime::HostMoveCursor(snapshot.generation, -5), "active-repeat setup did not move cursor") ||
		    !Check(Ime::ProcessSdlEvent(down), "dialog keydown was not consumed") ||
		    !Check(Ime::GetHostSnapshot(&snapshot) && snapshot.cursor == 1, "initial Right did not move cursor")) return false;
		const uint64_t owned_generation = snapshot.generation;
		down.key.repeat = 1;
		if (!Check(Ime::ProcessSdlEvent(down) && Ime::GetHostSnapshot(&snapshot) && snapshot.cursor == 2,
		        "active Right repeat was suppressed") ||
		    !Check(Ime::ProcessSdlEvent(down) && Ime::GetHostSnapshot(&snapshot) && snapshot.cursor == 3,
		        "second active Right repeat was suppressed") ||
		    !Finish(fixture, owned_generation, Ime::EndStatus::Ok)) return false;
		if (!Check(Ime::ProcessSdlEvent(down) && Ime::ProcessSdlEvent(down), "owned repeats escaped after finish") ||
		    !Check(Ime::ImeDialogTerm() == 0, "owned dialog Term failed") ||
		    !InitSdlFixture(fixture) || !Snapshot(&snapshot, u"Owned") ||
		    !Check(snapshot.generation != owned_generation && Ime::HostMoveCursor(snapshot.generation, -5),
		        "new dialog ownership/cursor setup failed")) return false;
		const uint64_t new_generation = snapshot.generation;
		if (!Check(Ime::ProcessSdlEvent(down) && Ime::GetHostSnapshot(&snapshot) && snapshot.cursor == 0 && snapshot.text == u"Owned",
		        "old owned repeat edited the new dialog") ||
		    !Check(Ime::ProcessSdlEvent(KeyEvent(SDL_KEYUP, SDLK_RIGHT, SDL_SCANCODE_RIGHT)), "owned release escaped after finish") ||
		    !Check(!Ime::ProcessSdlEvent(KeyEvent(SDL_KEYUP, SDLK_RIGHT, SDL_SCANCODE_RIGHT)), "released key retained ownership")) return false;
		down.key.repeat = 0;
		if (!Check(Ime::ProcessSdlEvent(down) && Ime::GetHostSnapshot(&snapshot) && snapshot.cursor == 1,
		        "fresh Right did not acquire new dialog ownership") ||
		    !Check(Ime::ProcessSdlEvent(KeyEvent(SDL_KEYUP, SDLK_RIGHT, SDL_SCANCODE_RIGHT)), "new owned release escaped") ||
		    !Finish(fixture, new_generation, Ime::EndStatus::UserCanceled) || !Check(fixture.Unchanged(), "owned cursor events changed guest text")) return false;
	}
	{
		Fixture fixture(u"Old");
		Ime::HostSnapshot before, after;
		if (!InitSdlFixture(fixture) || !Snapshot(&before, u"Old") ||
		    !Check(Ime::ProcessSdlEvent(TextEvent(SDL_TEXTEDITING, "prior")), "old preedit was not consumed") ||
		    !Finish(fixture, before.generation, Ime::EndStatus::UserCanceled) ||
		    !Check(Ime::ImeDialogTerm() == 0, "old dialog Term failed") || !fixture.Init() || !Snapshot(&after, u"Old") ||
		    !Check(before.generation != after.generation, "dialog generation did not advance")) return false;
		// These packets were queued before the window adopted the new dialog.
		(void)Ime::ProcessSdlEvent(TextEvent(SDL_TEXTINPUT, " queued"));
		(void)Ime::ProcessSdlEvent(TextEvent(SDL_TEXTEDITING, "obsolete"));
		if (!Snapshot(&after, u"Old") || !Check(fixture.Unchanged(), "previous-generation packet wrote guest text") ||
		    !Check(Ime::HostCaption().find("obsolete") == std::string::npos, "previous-generation preedit reached new caption")) return false;
		Ime::SyncSdlInput(nullptr);
		if (!Check(Ime::HostCaption().find("prior") == std::string::npos && Ime::HostCaption().find("obsolete") == std::string::npos,
		        "new generation retained old preedit") ||
		    !Check(Ime::ProcessSdlEvent(TextEvent(SDL_TEXTINPUT, " new")), "synchronized text input was not consumed") ||
		    !Snapshot(&after, u"Old new") || !Check(fixture.Unchanged(), "synchronized editing wrote guest text") ||
		    !Finish(fixture, after.generation, Ime::EndStatus::Ok) ||
		    !Check(fixture.TextEquals(u"Old new") && fixture.TextCanaries(), "new generation final text/canaries differ")) return false;
	}
	return true;
}

} // namespace

int main()
{
	Lumen::Core::ThreadsSubsystem::Instance()->Init(nullptr);
	Lumen::Config::ConfigSubsystem::Instance()->Init(nullptr);
	Lumen::Log::LogSubsystem::Instance()->Init(nullptr);
	Lumen::Log::SetDirection(Lumen::Log::Direction::Silent);
	Memory::Init();
	if (!TestRegistration() || !TestLifecycleAndUnicode() || !TestCancelAbortAndReset() || !TestFullTextReplacement() || !TestInvalidInputs() ||
	    !TestContinuity() || !TestOutputsAndPanels() || !TestSdlInputBridge() || !TestSdlOwnershipAndGeneration()) return 1;
	std::printf("[IME_DIALOG_PS4_TEST] checks=%u registry8/scalar-ABI=pass managed-guest-UTF16=pass"
	    " lifecycle/edit/cursor/capacity/cancel/abort/reset=pass full-replacement/empty-clear/rejected-edits=pass callbacks-unsupported=closed"
	    " revoked/restored/remapped=closed disjoint-mutation=accepted output-atomic/canaries=pass"
	    " SDL2-synthetic-UTF8/preedit/password/capacity/key-release=pass SDL-ownership/generation=pass controller=untouched\n", g_checks);
	return 0;
}
