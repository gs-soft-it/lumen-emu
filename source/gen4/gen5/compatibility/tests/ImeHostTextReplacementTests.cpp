// Original Lumen tests. SPDX-License-Identifier: GPL-2.0-only
#include "libs/ime.h"
#include "libs/imeDialog.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace Dialog = Libs::Dialog::ImeDialog;
namespace CoreIme = Libs::Ime;

namespace {

uint32_t g_checks = 0;
void Check(bool condition, const char* expression, int line)
{
	++g_checks;
	if (!condition)
	{
		std::fprintf(stderr, "ImeHostTextReplacementTests:%d: %s\n", line, expression);
		std::abort();
	}
}
#define CHECK(condition) Check(bool(condition), #condition, __LINE__)

struct TextBuffer
{
	static constexpr uint32_t Capacity = 8;
	std::array<char16_t, 64> data {};
	char16_t* Text() { return data.data() + 2; }
	void Reset(std::u16string_view text)
	{
		data.fill(0xa5a5);
		std::fill_n(Text(), Capacity + 1, char16_t {0});
		std::memcpy(Text(), text.data(), text.size() * sizeof(char16_t));
	}
	bool Canaries() const
	{
		for (size_t i = 0; i < data.size(); ++i)
			if ((i < 2 || i >= 2 + Capacity + 1) && data[i] != 0xa5a5) return false;
		return true;
	}
};

template <typename Operation> void RunUi(Operation operation)
{
	std::thread ui([&] { operation(); });
	ui.join();
}

void TestDialogReplacement()
{
	TextBuffer buffer;
	buffer.Reset(u"Seed");
	Dialog::Param param {};
	param.user_id = 1000; param.type = Dialog::Type::Default;
	param.option = 2; param.max_text_length = TextBuffer::Capacity;
	param.input_text_buffer = buffer.Text();
	CHECK(Dialog::ImeDialogInit(&param, nullptr) == 0);
	Dialog::HostSnapshot first, snapshot;
	CHECK(Dialog::GetHostSnapshot(&first));
	CHECK(Dialog::ImeDialogAbort() == 0);
	Dialog::Result result {};
	CHECK(Dialog::ImeDialogGetResult(&result) == 0);
	CHECK(Dialog::ImeDialogTerm() == 0);
	CHECK(Dialog::ImeDialogInit(&param, nullptr) == 0);
	CHECK(Dialog::GetHostSnapshot(&snapshot));
	CHECK(snapshot.generation != first.generation);
	const uint64_t generation = snapshot.generation;
	const auto original = buffer.data;
	RunUi([&] { CHECK(Dialog::HostReplaceText(generation, u"A\U0001f600B", 3)); });
	CHECK(buffer.data == original);
	CHECK(Dialog::GetHostSnapshot(&snapshot) && snapshot.text == u"A\U0001f600B" && snapshot.cursor == 3);
	RunUi([&] { CHECK(Dialog::HostReplaceText(generation, u"", 0)); });
	CHECK(Dialog::GetHostSnapshot(&snapshot) && snapshot.text.empty() && snapshot.cursor == 0);
	CHECK(buffer.data == original);
	RunUi([&] { CHECK(Dialog::HostReplaceText(generation, u"Ready", 2)); });
	const auto reject = [&](uint64_t token, std::u16string_view text, uint32_t cursor) {
		RunUi([&] { CHECK(!Dialog::HostReplaceText(token, text, cursor)); });
		CHECK(Dialog::GetHostSnapshot(&snapshot) && snapshot.generation == generation && snapshot.text == u"Ready" && snapshot.cursor == 2);
		CHECK(buffer.data == original);
	};
	const char16_t malformed[] = {0xd800};
	reject(first.generation, u"Late", 0);
	reject(generation, u"123456789", 9);
	reject(generation, {malformed, 1}, 1);
	reject(generation, {u"A\0B", 3}, 3);
	reject(generation, u"A\nB", 3);
	reject(generation, u"A\U0001f600B", 2);
	reject(generation, u"ok", 3);
	RunUi([&] { CHECK(Dialog::HostAccept(generation)); });
	CHECK(buffer.data == original);
	CHECK(Dialog::ImeDialogGetStatus() == int(Dialog::Status::Finished));
	result = {};
	CHECK(Dialog::ImeDialogGetResult(&result) == 0 && result.endstatus == Dialog::EndStatus::Ok);
	CHECK(std::u16string(buffer.Text()) == u"Ready" && buffer.Canaries());
	CHECK(Dialog::ImeDialogTerm() == 0);
	CHECK(!Dialog::HostReplaceText(generation, u"Closed", 6));
	buffer.Reset(u"Seed");
	CHECK(Dialog::ImeDialogInit(&param, nullptr) == 0);
	CHECK(Dialog::GetHostSnapshot(&snapshot));
	const auto clear_original = buffer.data;
	RunUi([&] { CHECK(Dialog::HostReplaceText(snapshot.generation, u"", 0)); });
	CHECK(buffer.data == clear_original);
	CHECK(Dialog::HostAccept(snapshot.generation));
	CHECK(Dialog::ImeDialogGetStatus() == int(Dialog::Status::Finished));
	result = {};
	CHECK(Dialog::ImeDialogGetResult(&result) == 0 && result.endstatus == Dialog::EndStatus::Ok);
	CHECK(buffer.Text()[0] == 0 && buffer.Canaries());
	CHECK(Dialog::ImeDialogTerm() == 0);
}

struct ObservedEvent
{
	uint32_t id = 0, cursor = 0, direction = 0;
	std::u16string text;
};

struct EventProbe
{
	std::thread::id update_thread = std::this_thread::get_id();
	char16_t* work = nullptr;
	std::vector<ObservedEvent> events;
};

void KYTY_SYSV_ABI NativeEvent(void* arg, const CoreIme::Event* event)
{
	auto& probe = *static_cast<EventProbe*>(arg);
	CHECK(std::this_thread::get_id() == probe.update_thread);
	CHECK(event != nullptr);
	ObservedEvent observed;
	observed.id = event->id;
	if (event->id == 1)
	{
		CHECK(event->param.text.str == probe.work && event->param.text.area_num == 1);
		CHECK(event->param.text.text_area[0].mode == CoreIme::TextAreaMode::Edit);
		observed.text = event->param.text.str;
		observed.cursor = event->param.text.caret_index;
	}
	if (event->id == 2) observed.direction = event->param.caret_move;
	probe.events.push_back(std::move(observed));
}

struct CoreFixture
{
	TextBuffer input;
	alignas(16) std::array<uint8_t, CoreIme::WORK_BUFFER_SIZE + 32> work {};
	EventProbe probe;
	CoreIme::Param param {};
	CoreIme::ExtendedParam extended {};
	CoreIme::HostSnapshot snapshot;
	bool open = false;
	CoreFixture(std::u16string_view seed = u"Seed", CoreIme::Type type = CoreIme::Type::Default,
	            CoreIme::TextFilter filter = nullptr, CoreIme::ExtKeyboardFilter key_filter = nullptr)
	{
		input.Reset(seed); work.fill(0xa5);
		param.user_id = 1000; param.type = type; param.option = 2;
		param.filter = filter;
		extended.ext_keyboard_filter = key_filter;
		param.max_text_length = TextBuffer::Capacity; param.input_text_buffer = input.Text();
		param.work = work.data() + 16; param.arg = &probe; param.handler = NativeEvent;
		probe.work = reinterpret_cast<char16_t*>(param.work);
		Open();
	}
	~CoreFixture() { if (open) CHECK(CoreIme::ImeClose() == 0); }
	void Open()
	{
		CHECK(CoreIme::ImeOpen(&param, extended.ext_keyboard_filter != nullptr ? &extended : nullptr) == 0); open = true;
		CHECK(CoreIme::GetHostSnapshot(&snapshot));
		CHECK(CoreIme::ImeUpdate(NativeEvent) == 0);
		CHECK(probe.events.size() == 1 && probe.events[0].id == 0);
		probe.events.clear();
	}
	void Close() { CHECK(CoreIme::ImeClose() == 0); open = false; }
	bool Canaries() const
	{
		for (size_t i = 0; i < 16; ++i)
			if (work[i] != 0xa5 || work[16 + CoreIme::WORK_BUFFER_SIZE + i] != 0xa5) return false;
		return input.Canaries();
	}
	void UpdateText(std::u16string_view expected, uint32_t cursor)
	{
		CHECK(CoreIme::ImeUpdate(NativeEvent) == 0);
		CHECK(CoreIme::GetHostSnapshot(&snapshot) && snapshot.text == expected && snapshot.cursor == cursor);
		CHECK(std::u16string(input.Text()) == expected && std::u16string(probe.work) == expected && Canaries());
	}
};

void TestCoreQueuedReplacement()
{
	CoreFixture fixture;
	const uint64_t generation = fixture.snapshot.generation;
	auto input_before = fixture.input.data;
	auto work_before = fixture.work;
	RunUi([&] { CHECK(CoreIme::HostReplaceText(generation, u"A\U0001f600B", 3)); });
	CHECK(fixture.input.data == input_before && fixture.work == work_before && fixture.probe.events.empty());
	CHECK(CoreIme::GetHostSnapshot(&fixture.snapshot) && fixture.snapshot.text == u"Seed" && fixture.snapshot.cursor == 4);
	fixture.UpdateText(u"A\U0001f600B", 3);
	CHECK(fixture.probe.events.size() == 1 && fixture.probe.events[0].id == 1 &&
	    fixture.probe.events[0].text == u"A\U0001f600B" && fixture.probe.events[0].cursor == 3);
	fixture.probe.events.clear();
	input_before = fixture.input.data; work_before = fixture.work;
	RunUi([&] {
		std::u16string owned = u"Mood";
		CHECK(CoreIme::HostReplaceText(generation, owned, 1));
		owned = u"Last";
		CHECK(CoreIme::HostReplaceText(generation, owned, 4));
		owned = u"BAD";
	});
	CHECK(fixture.input.data == input_before && fixture.work == work_before && fixture.probe.events.empty());
	fixture.UpdateText(u"Last", 4);
	CHECK(fixture.probe.events.size() == 2 && fixture.probe.events[0].id == 1 && fixture.probe.events[1].id == 1);
	CHECK(fixture.probe.events[0].text == u"Mood" && fixture.probe.events[0].cursor == 1);
	CHECK(fixture.probe.events[1].text == u"Last" && fixture.probe.events[1].cursor == 4);
	fixture.probe.events.clear();
	input_before = fixture.input.data; work_before = fixture.work;
	RunUi([&] { CHECK(CoreIme::HostReplaceText(generation, u"Last", 1)); });
	CHECK(fixture.input.data == input_before && fixture.work == work_before);
	fixture.UpdateText(u"Last", 1);
	CHECK(fixture.input.data == input_before && fixture.work == work_before);
	CHECK(fixture.probe.events.size() == 3);
	for (const auto& event: fixture.probe.events) CHECK(event.id == 2 && event.direction == 1);
	fixture.probe.events.clear();
	RunUi([&] { CHECK(CoreIme::HostReplaceText(generation, u"Last", 1)); });
	CHECK(fixture.input.data == input_before && fixture.work == work_before);
	fixture.UpdateText(u"Last", 1);
	CHECK(fixture.probe.events.empty() && fixture.input.data == input_before && fixture.work == work_before);
	RunUi([&] { CHECK(CoreIme::HostReplaceText(generation, u"", 0)); });
	CHECK(fixture.input.data == input_before && fixture.work == work_before);
	fixture.UpdateText(u"", 0);
	CHECK(fixture.probe.events.size() == 1 && fixture.probe.events[0].id == 1 && fixture.probe.events[0].text.empty());
	fixture.probe.events.clear();
	input_before = fixture.input.data; work_before = fixture.work;
	RunUi([&] { CHECK(CoreIme::HostAccept(generation)); });
	CHECK(fixture.probe.events.empty() && fixture.input.data == input_before && fixture.work == work_before);
	fixture.UpdateText(u"", 0);
	CHECK(fixture.probe.events.size() == 1 && fixture.probe.events[0].id == 5);
}

void TestCoreRejectionAndCleanup()
{
	CoreFixture fixture;
	const uint64_t old_generation = fixture.snapshot.generation;
	RunUi([&] { CHECK(CoreIme::HostReplaceText(old_generation, u"Queued", 6)); });
	fixture.Close();
	fixture.input.Reset(u"New");
	fixture.Open();
	CHECK(fixture.snapshot.generation != old_generation && fixture.snapshot.text == u"New");
	const uint64_t generation = fixture.snapshot.generation;
	const auto input_before = fixture.input.data;
	const auto work_before = fixture.work;
	const auto reject = [&](uint64_t token, std::u16string_view text, uint32_t cursor) {
		RunUi([&] { CHECK(!CoreIme::HostReplaceText(token, text, cursor)); });
		CHECK(fixture.probe.events.empty() && fixture.input.data == input_before && fixture.work == work_before);
		fixture.UpdateText(u"New", 3);
		CHECK(fixture.probe.events.empty() && fixture.input.data == input_before && fixture.work == work_before);
	};
	const char16_t malformed[] = {0xd800};
	reject(old_generation, u"Old", 3);
	reject(generation, u"123456789", 9);
	reject(generation, {malformed, 1}, 1);
	reject(generation, {u"A\0B", 3}, 3);
	reject(generation, u"A\nB", 3);
	reject(generation, u"A\U0001f600B", 2);
	reject(generation, u"ok", 3);
	RunUi([&] { CHECK(CoreIme::HostReplaceText(generation, u"Clean", 5)); });
	CHECK(fixture.input.data == input_before && fixture.work == work_before);
	fixture.UpdateText(u"Clean", 5);
	CHECK(fixture.probe.events.size() == 1 && fixture.probe.events[0].id == 1 && fixture.probe.events[0].text == u"Clean");
	fixture.Close();
	CHECK(!CoreIme::HostReplaceText(generation, u"Closed", 6));
	{
		CoreFixture numeric(u"1", CoreIme::Type::Number);
		const auto numeric_input = numeric.input.data;
		const auto numeric_work = numeric.work;
		RunUi([&] { CHECK(!CoreIme::HostReplaceText(numeric.snapshot.generation, u"abc", 3)); });
		numeric.UpdateText(u"1", 1);
		CHECK(numeric.probe.events.empty() && numeric.input.data == numeric_input && numeric.work == numeric_work);
	}
}

enum class FilterMode { Transform, OverCapacity, Malformed, Error };
struct FilterProbe
{
	std::thread::id update_thread = std::this_thread::get_id();
	FilterMode mode = FilterMode::Transform;
	uint32_t calls = 0;
	std::u16string source;
};
FilterProbe* g_filter_probe = nullptr;

int32_t KYTY_SYSV_ABI NativeTextFilter(char16_t* output, uint32_t* length,
                                     const char16_t* source, uint32_t source_length)
{
	CHECK(g_filter_probe != nullptr && std::this_thread::get_id() == g_filter_probe->update_thread);
	CHECK(output != nullptr && length != nullptr && source != nullptr);
	CHECK(*length == TextBuffer::Capacity);
	++g_filter_probe->calls;
	g_filter_probe->source.assign(source, source + source_length);
	if (g_filter_probe->mode == FilterMode::OverCapacity)
	{
		output[0] = u'X';
		*length = TextBuffer::Capacity + 1;
		return 0;
	}
	if (g_filter_probe->mode == FilterMode::Malformed)
	{
		output[0] = 0xdc00;
		*length = 1;
		return 0;
	}
	std::copy_n(u"Map", 3, output);
	*length = 3;
	return g_filter_probe->mode == FilterMode::Error ? 1 : 0;
}

void TestNativeTextFilter()
{
	FilterProbe filter;
	g_filter_probe = &filter;
	{
		CoreFixture fixture(u"Seed", CoreIme::Type::Default, NativeTextFilter);
		const auto input_before = fixture.input.data;
		const auto work_before = fixture.work;
		RunUi([&] { CHECK(CoreIme::HostReplaceText(fixture.snapshot.generation, u"Next", 4)); });
		CHECK(filter.calls == 0 && fixture.probe.events.empty());
		CHECK(fixture.input.data == input_before && fixture.work == work_before);
		fixture.UpdateText(u"Map", 3);
		CHECK(filter.calls == 1 && filter.source == u"Next");
		CHECK(fixture.probe.events.size() == 1 && fixture.probe.events[0].id == 1 &&
		      fixture.probe.events[0].text == u"Map" && fixture.probe.events[0].cursor == 3);
	}
	for (FilterMode mode: {FilterMode::OverCapacity, FilterMode::Malformed, FilterMode::Error})
	{
		filter.mode = mode;
		const uint32_t previous_calls = filter.calls;
		std::u16string output = u"Sentinel";
		CHECK(!Libs::ImeCommon::RunTextFilter(NativeTextFilter, u"Next", TextBuffer::Capacity, &output));
		CHECK(output == u"Sentinel" && filter.calls == previous_calls + 1 && filter.source == u"Next");
	}
	{
		// An unusable filter result preserves the existing valid candidate contract.
		CoreFixture fixture(u"Seed", CoreIme::Type::Default, NativeTextFilter);
		const auto input_before = fixture.input.data;
		const auto work_before = fixture.work;
		const uint32_t previous_calls = filter.calls;
		RunUi([&] { CHECK(CoreIme::HostReplaceText(fixture.snapshot.generation, u"Next", 4)); });
		CHECK(filter.calls == previous_calls && fixture.probe.events.empty());
		CHECK(fixture.input.data == input_before && fixture.work == work_before);
		fixture.UpdateText(u"Next", 4);
		CHECK(filter.calls == previous_calls + 1 && filter.source == u"Next");
		CHECK(fixture.probe.events.size() == 1 && fixture.probe.events[0].id == 1 && fixture.probe.events[0].text == u"Next");
	}
	g_filter_probe = nullptr;
}

struct KeyboardFilterProbe
{
	std::thread::id update_thread = std::this_thread::get_id();
	Libs::ImeCommon::Keycode expected {};
	uint32_t calls = 0;
	int result = 0;
};
KeyboardFilterProbe* g_keyboard_filter_probe = nullptr;

bool SameKeycode(const Libs::ImeCommon::Keycode& first, const Libs::ImeCommon::Keycode& second)
{
	return first.keycode == second.keycode && first.character == second.character && first.status == second.status &&
	       first.type == second.type && first.user_id == second.user_id && first.resource_id == second.resource_id &&
	       first.timestamp == second.timestamp;
}

int KYTY_SYSV_ABI NativeKeyboardFilter(const Libs::ImeCommon::Keycode* source,
                                     uint16_t* keycode, uint32_t* status, void* reserved)
{
	CHECK(g_keyboard_filter_probe != nullptr && std::this_thread::get_id() == g_keyboard_filter_probe->update_thread);
	CHECK(source != nullptr && keycode != nullptr && status != nullptr && reserved == nullptr);
	CHECK(SameKeycode(*source, g_keyboard_filter_probe->expected));
	CHECK(*keycode == source->keycode && *status == source->status);
	++g_keyboard_filter_probe->calls;
	*keycode = 5;
	*status = 0x201;
	return g_keyboard_filter_probe->result;
}

void TestNativeKeyboardFilter()
{
	KeyboardFilterProbe filter;
	filter.expected = {4, u'a', 3, 2, 1000, 7, 44};
	g_keyboard_filter_probe = &filter;
	const auto source = filter.expected;
	uint16_t keycode = source.keycode;
	uint32_t status = source.status;
	CHECK(Libs::ImeCommon::RunKeyboardFilter(NativeKeyboardFilter, source, &keycode, &status) == 0);
	CHECK(keycode == 5 && status == 0x201 && filter.calls == 1);
	CHECK(SameKeycode(source, filter.expected));
	Libs::ImeCommon::ExternalInput input;
	input.key = source; input.action = Libs::ImeCommon::ExternalAction::Text; input.text = u"a";
	CHECK(Libs::ImeCommon::ApplyKeyboardFilterOutput(&input, keycode, status, false));
	CHECK(input.action == Libs::ImeCommon::ExternalAction::Text && input.text == u"B" && input.key.character == u'B');
	filter.result = 7;
	keycode = source.keycode; status = source.status;
	CHECK(Libs::ImeCommon::RunKeyboardFilter(NativeKeyboardFilter, source, &keycode, &status) == 7);
	CHECK(keycode == 5 && status == 0x201 && filter.calls == 2);
	keycode = source.keycode; status = source.status;
	CHECK(Libs::ImeCommon::RunKeyboardFilter(nullptr, source, &keycode, &status) == -1);
	CHECK(Libs::ImeCommon::RunKeyboardFilter(NativeKeyboardFilter, source, nullptr, &status) == -1);
	CHECK(Libs::ImeCommon::RunKeyboardFilter(NativeKeyboardFilter, source, &keycode, nullptr) == -1);
	CHECK(keycode == source.keycode && status == source.status && filter.calls == 2);
	filter.result = 0;
	{
		CoreFixture fixture(u"Seed", CoreIme::Type::Default, nullptr, NativeKeyboardFilter);
		const auto input_before = fixture.input.data;
		const auto work_before = fixture.work;
		input.key = source; input.action = Libs::ImeCommon::ExternalAction::Text; input.text = u"a";
		RunUi([&] { CHECK(CoreIme::HostQueueExternalInput(fixture.snapshot.generation, input)); });
		CHECK(filter.calls == 2 && fixture.probe.events.empty());
		CHECK(fixture.input.data == input_before && fixture.work == work_before);
		fixture.UpdateText(u"SeedB", 5);
		CHECK(filter.calls == 3 && fixture.probe.events.size() == 1 && fixture.probe.events[0].id == 1 &&
		      fixture.probe.events[0].text == u"SeedB");
	}
	g_keyboard_filter_probe = nullptr;
}

} // namespace

int main()
{
	TestDialogReplacement();
	TestCoreQueuedReplacement();
	TestCoreRejectionAndCleanup();
	TestNativeTextFilter();
	TestNativeKeyboardFilter();
	std::printf("[PS5_IME_HOST_TEXT_REPLACEMENT_TEST] checks=%u dialog-full-value/cursor/empty/stale/invalid=pass"
	    " core-owned-queue/order/cursor-only/cleanup=pass UI-thread-guest-write/callback=none"
	    " update-text-event/native-callback=pass native-text-filter/invalid-output-sentinel=pass"
	    " native-keyboard-filter/output-routing=pass"
	    " input/work-canaries=pass controller=untouched FEX-callback=unverified\n", g_checks);
	return 0;
}
