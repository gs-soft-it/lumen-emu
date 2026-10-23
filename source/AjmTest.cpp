#include "Libs/AjmCore.h"

extern "C" {
#include <bit_reader.h>
}

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <type_traits>
#include <vector>

namespace {

namespace Ajm = Lumen::Libs::Ajm;

bool g_ok = true;

#define CHECK(expression)                                                                                                                   \
	do                                                                                                                                       \
	{                                                                                                                                        \
		if (!(expression))                                                                                                                      \
		{                                                                                                                                    \
			std::fprintf(stderr, "ajm-test failed at line %d: %s\n", __LINE__, #expression);                                                   \
			g_ok = false;                                                                                                                       \
		}                                                                                                                                    \
	} while (false)

uint32_t ReadU32(const uint8_t* data)
{
	uint32_t value = 0;
	std::memcpy(&value, data, sizeof(value));
	return value;
}

uint64_t ReadU64(const uint8_t* data)
{
	uint64_t value = 0;
	std::memcpy(&value, data, sizeof(value));
	return value;
}

void WriteU32(uint8_t* data, uint32_t value)
{
	std::memcpy(data, &value, sizeof(value));
}

void WriteU64(uint8_t* data, uint64_t value)
{
	std::memcpy(data, &value, sizeof(value));
}

uint64_t ReadFlags(const uint8_t* data)
{
	const uint32_t header = ReadU32(data);
	return (static_cast<uint64_t>((header >> 6u) & 0x000fffffu) << 32u) | ReadU32(data + 4);
}

struct InitializeData
{
	std::array<uint8_t, 4> config {};
	uint32_t               reserved = 0;
};

static_assert(sizeof(void*) == 8);
static_assert(sizeof(InitializeData) == 8);
static_assert(offsetof(Ajm::BatchError, error_code) == 0);
static_assert(offsetof(Ajm::BatchError, job_addr) == 8);
static_assert(offsetof(Ajm::BatchError, cmd_offset) == 16);
static_assert(offsetof(Ajm::BatchError, job_ra) == 24);

using ControlBuilder = void* (*)(void*, uint32_t, uint64_t, const void*, size_t, void*, size_t, const void*);
using RunBuilder = void* (*)(void*, uint32_t, uint64_t, const void*, size_t, void*, size_t, void*, size_t, const void*);
static_assert(std::is_same_v<decltype(&Ajm::BatchJobControlBufferRa), ControlBuilder>);
static_assert(std::is_same_v<decltype(&Ajm::BatchJobRunBufferRa), RunBuilder>);

struct FakeState
{
	uint32_t created_channels = 0;
	uint32_t created_format   = 0;
	uint32_t created_flags    = 0;
	uint32_t initialize_calls = 0;
	uint32_t reset_calls      = 0;
	uint32_t decode_calls     = 0;
	bool     fail_initialize  = false;
	bool     fail_reset       = false;
	bool     dishonest_decode = false;
};

class FakeDecoder final: public Ajm::At9Decoder
{
public:
	FakeDecoder(FakeState* state, uint32_t channels, uint32_t format): m_state(state), m_channels(channels), m_format(format) {}

	int32_t Initialize(const std::array<uint8_t, 4>& config, Ajm::At9CodecInfo* info) override
	{
		m_state->initialize_calls++;
		if (m_state->fail_initialize)
		{
			return -70;
		}
		if (info == nullptr || config[0] != 0xfe)
		{
			return -10;
		}
		info->channels             = m_channels;
		info->channel_config_index = m_channels == 1 ? 0 : 2;
		info->sampling_rate        = 48000;
		info->superframe_size      = 8;
		info->frames_in_superframe = 2;
		info->frame_samples        = 2;
		info->window_length        = 6;
		info->config               = config;
		m_initialized              = true;
		return 0;
	}

	int32_t Reset() override
	{
		m_state->reset_calls++;
		if (m_state->fail_reset)
		{
			return -71;
		}
		return m_initialized ? 0 : -11;
	}

	int32_t DecodeFrame(const uint8_t* input, size_t input_size, void* pcm, size_t pcm_size, uint32_t format,
	                    bool non_interleaved, uint32_t* bytes_used) override
	{
		m_state->decode_calls++;
		const size_t required = static_cast<size_t>(m_channels) * 2u * sizeof(int16_t);
		if (!m_initialized || input == nullptr || input_size < 3 || pcm == nullptr || pcm_size < required ||
		    bytes_used == nullptr || format != m_format || format != 0 || non_interleaved)
		{
			return -12;
		}
		auto* samples = static_cast<int16_t*>(pcm);
		for (uint32_t i = 0; i < m_channels * 2u; i++)
		{
			samples[i] = static_cast<int16_t>(m_state->decode_calls * 100u + i);
		}
		*bytes_used = m_state->dishonest_decode ? 99u : 3u;
		return 0;
	}

private:
	FakeState* m_state       = nullptr;
	uint32_t   m_channels    = 0;
	uint32_t   m_format      = 0;
	bool       m_initialized = false;
};

class FakeFactory final: public Ajm::At9DecoderFactory
{
public:
	explicit FakeFactory(FakeState* state): m_state(state) {}

	bool Available() const override { return true; }

	std::unique_ptr<Ajm::At9Decoder> Create(uint32_t channels, uint32_t format, uint32_t codec_flags) override
	{
		m_state->created_channels = channels;
		m_state->created_format   = format;
		m_state->created_flags    = codec_flags;
		return std::make_unique<FakeDecoder>(m_state, channels, format);
	}

private:
	FakeState* m_state = nullptr;
};

bool CreateFakeInstance(Ajm::Runtime* runtime, uint32_t* context, uint32_t* instance, FakeState* state)
{
	CHECK(runtime != nullptr);
	CHECK(runtime->Initialize(0, context) == Ajm::AJM_OK);
	CHECK(runtime->ModuleRegister(*context, Ajm::AJM_CODEC_AT9, 0) == Ajm::AJM_OK);
	CHECK(runtime->InstanceCreate(*context, Ajm::AJM_CODEC_AT9, 0x411, instance) == Ajm::AJM_OK);
	CHECK((*instance >> 14u) == Ajm::AJM_CODEC_AT9);
	CHECK(state->created_channels == 2);
	CHECK(state->created_format == 0);
	CHECK(state->created_flags == 0);
	return true;
}

bool InitializeInstance(Ajm::Runtime* runtime, uint32_t context, uint32_t instance, Ajm::Result* sideband,
	                    std::array<uint8_t, 4> config = {0xfe, 0x74, 0x1f, 0xe0})
{
	InitializeData init {config, 0};
	alignas(8) std::array<uint8_t, 0x40> batch {};
	CHECK(Ajm::BatchJobControlBufferRa(batch.data(), instance, 0x6001, &init, sizeof(init), sideband, sizeof(*sideband), nullptr) ==
	      batch.data() + batch.size());
	uint32_t        batch_id = 0;
	Ajm::BatchError error {};
	CHECK(runtime->BatchStart(context, batch.data(), static_cast<uint32_t>(batch.size()), 40, &error, &batch_id) == Ajm::AJM_OK);
	CHECK(runtime->BatchWait(context, batch_id, 0, &error) == Ajm::AJM_OK);
	return true;
}

void TestBuilders()
{
	alignas(8) std::array<uint8_t, 0x70> control_batch {};
	control_batch.fill(0xcc);
	InitializeData init {{0xfe, 0x74, 0x1f, 0xe0}, 0};
	Ajm::Result   control_result {};
	const auto*   return_address = reinterpret_cast<const void*>(UINT64_C(0x123456789abcdef0));
	void* control_end = Ajm::BatchJobControlBufferRa(control_batch.data(), 0x4001, 0x200000002001ull, &init, sizeof(init),
	                                                 &control_result, sizeof(control_result), return_address);
	CHECK(control_end == control_batch.data() + 0x40);
	CHECK(ReadU32(control_batch.data()) == 0x00100040u);
	CHECK(ReadU32(control_batch.data() + 4) == 0x38u);
	CHECK(ReadU32(control_batch.data() + 8) == 6u);
	CHECK(ReadU32(control_batch.data() + 12) == 0u);
	CHECK(ReadU64(control_batch.data() + 16) == reinterpret_cast<uintptr_t>(return_address));
	CHECK(ReadU32(control_batch.data() + 24) == 2u);
	CHECK(ReadU32(control_batch.data() + 28) == sizeof(init));
	CHECK(ReadU64(control_batch.data() + 32) == reinterpret_cast<uintptr_t>(&init));
	CHECK((ReadU32(control_batch.data() + 40) & 0x3fu) == 3u);
	CHECK(ReadFlags(control_batch.data() + 40) == 0x200000002001ull);
	CHECK(ReadU32(control_batch.data() + 48) == 18u);
	CHECK(ReadU32(control_batch.data() + 52) == sizeof(control_result));
	CHECK(ReadU64(control_batch.data() + 56) == reinterpret_cast<uintptr_t>(&control_result));
	CHECK(std::all_of(control_batch.begin() + 0x40, control_batch.end(), [](uint8_t value) { return value == 0xcc; }));

	control_batch.fill(0xcc);
	control_end = Ajm::BatchJobControlBufferRa(control_batch.data(), 0x4001, std::numeric_limits<uint64_t>::max(), &init,
	                                           sizeof(init), &control_result, sizeof(control_result), nullptr);
	CHECK(control_end == control_batch.data() + 0x40);
	CHECK(ReadU64(control_batch.data() + 16) == 0);
	CHECK(ReadFlags(control_batch.data() + 40) == UINT64_C(0x000fffffffffffff));
	CHECK((ReadU32(control_batch.data() + 40) >> 26u) == 0);

	alignas(8) std::array<uint8_t, 0x80> run_batch {};
	run_batch.fill(0xcc);
	std::array<uint8_t, 8> input {};
	std::array<uint8_t, 16> output {};
	std::array<uint8_t, 32> run_sideband {};
	void* run_end = Ajm::BatchJobRunBufferRa(run_batch.data(), 0x4001, 0x800000001001ull, input.data(), input.size(),
	                                         output.data(), output.size(), run_sideband.data(), run_sideband.size(), return_address);
	CHECK(run_end == run_batch.data() + 0x50);
	CHECK(ReadU32(run_batch.data()) == 0x00100040u);
	CHECK(ReadU32(run_batch.data() + 4) == 0x48u);
	CHECK(ReadU32(run_batch.data() + 8) == 6u);
	CHECK(ReadU32(run_batch.data() + 24) == 1u);
	CHECK(ReadFlags(run_batch.data() + 40) == 0x800000001001ull);
	CHECK(ReadU32(run_batch.data() + 48) == 17u);
	CHECK(ReadU32(run_batch.data() + 64) == 18u);
	CHECK(std::all_of(run_batch.begin() + 0x50, run_batch.end(), [](uint8_t value) { return value == 0xcc; }));

	run_batch.fill(0xcc);
	run_end = Ajm::BatchJobRunBufferRa(run_batch.data(), 0x4001, 0x801, input.data(), input.size(), output.data(), output.size(),
	                                   run_sideband.data(), run_sideband.size(), nullptr);
	CHECK(run_end == run_batch.data() + 0x50);
	CHECK(ReadU64(run_batch.data() + 16) == 0);
	CHECK(ReadFlags(run_batch.data() + 40) == 0x801);
	CHECK(Ajm::BatchJobRunBufferRa(nullptr, 0x4001, 1, nullptr, 0, nullptr, 0, nullptr, 0, nullptr) == nullptr);

	const auto* overflow_address = reinterpret_cast<const void*>(std::numeric_limits<uintptr_t>::max() - 3u);
	control_batch.fill(0xcc);
	CHECK(Ajm::BatchJobControlBufferRa(control_batch.data(), 0x4001, 1, overflow_address, 8, nullptr, 0, nullptr) == nullptr);
	CHECK(std::all_of(control_batch.begin(), control_batch.end(), [](uint8_t value) { return value == 0xcc; }));
	run_batch.fill(0xcc);
	CHECK(Ajm::BatchJobRunBufferRa(run_batch.data(), 0x4001, 1, nullptr, 0, const_cast<void*>(overflow_address), 8, nullptr, 0,
	                               nullptr) == nullptr);
	CHECK(std::all_of(run_batch.begin(), run_batch.end(), [](uint8_t value) { return value == 0xcc; }));
	CHECK(Ajm::BatchJobRunBufferRa(const_cast<void*>(overflow_address), 0x4001, 1, nullptr, 0, nullptr, 0, nullptr, 0, nullptr) ==
	      nullptr);
}

void TestInstanceFlagLayout()
{
	FakeState   state {};
	FakeFactory factory(&state);
	Ajm::Runtime runtime(&factory);
	uint32_t     context = 0;
	CHECK(runtime.Initialize(0, &context) == Ajm::AJM_OK);
	CHECK(runtime.ModuleRegister(context, Ajm::AJM_CODEC_AT9, 0) == Ajm::AJM_OK);

	const auto check_create = [&](uint64_t flags, uint32_t channels, uint32_t format) {
		uint32_t instance = 0xdeadbeefu;
		CHECK(runtime.InstanceCreate(context, Ajm::AJM_CODEC_AT9, flags, &instance) == Ajm::AJM_OK);
		CHECK(instance != 0xdeadbeefu);
		CHECK(state.created_channels == channels);
		CHECK(state.created_format == format);
		CHECK(state.created_flags == 0);
		CHECK(runtime.InstanceDestroy(context, instance) == Ajm::AJM_OK);
	};

	check_create(0x9, 1, 0);
	check_create(0x11, 2, 0);
	check_create(0x41, 8, 0);
	check_create(0x89, 1, 1);
	check_create(0x91, 2, 1);
	check_create(0x109, 1, 2);
	check_create(0x111, 2, 2);
	check_create(0x409, 1, 0);
	check_create(0x411, 2, 0);

	uint32_t untouched = 0xfeedbeefu;
	CHECK(runtime.InstanceCreate(context, Ajm::AJM_CODEC_AT9, 0x211, &untouched) == Ajm::AJM_ERROR_INVALID_PARAMETER);
	CHECK(untouched == 0xfeedbeefu);
	CHECK(runtime.InstanceCreate(context, Ajm::AJM_CODEC_AT9, 0x811, &untouched) == Ajm::AJM_ERROR_FLAG_NOT_SUPPORTED);
	CHECK(untouched == 0xfeedbeefu);

	CHECK(runtime.ModuleUnregister(context, Ajm::AJM_CODEC_AT9) == Ajm::AJM_OK);
	CHECK(runtime.Finalize(context) == Ajm::AJM_OK);
}

void TestLifecycleAndSidebands()
{
	FakeState   state {};
	FakeFactory factory(&state);
	Ajm::Runtime runtime(&factory);

	uint32_t untouched = 0xfeedbeefu;
	CHECK(runtime.Initialize(1, &untouched) == Ajm::AJM_ERROR_INVALID_PARAMETER);
	CHECK(untouched == 0xfeedbeefu);
	CHECK(runtime.Initialize(0, nullptr) == Ajm::AJM_ERROR_INVALID_PARAMETER);

	uint32_t context1 = 0;
	uint32_t context2 = 0;
	CHECK(runtime.Initialize(0, &context1) == Ajm::AJM_OK);
	CHECK(runtime.Initialize(0, &context2) == Ajm::AJM_OK);
	CHECK(context1 == 1);
	CHECK(context2 != 0 && context2 != context1);

	uint32_t instance = 0xdeadbeefu;
	CHECK(runtime.InstanceCreate(context1, Ajm::AJM_CODEC_AT9, 0x411, &instance) == Ajm::AJM_ERROR_CODEC_NOT_REGISTERED);
	CHECK(instance == 0xdeadbeefu);
	CHECK(runtime.ModuleRegister(context1, Ajm::AJM_CODEC_AT9, 0) == Ajm::AJM_OK);
	CHECK(runtime.ModuleRegister(context1, Ajm::AJM_CODEC_AT9, 0) == Ajm::AJM_ERROR_CODEC_ALREADY_REGISTERED);
	CHECK(runtime.ModuleRegister(context2, Ajm::AJM_CODEC_AAC, 0) == Ajm::AJM_ERROR_CODEC_NOT_SUPPORTED);
	CHECK(runtime.InstanceCreate(context1, Ajm::AJM_CODEC_AT9, 0x410, &instance) == Ajm::AJM_ERROR_WRONG_REVISION_FLAG);
	CHECK(runtime.InstanceCreate(context1, Ajm::AJM_CODEC_AT9, 0xc11, &instance) == Ajm::AJM_ERROR_FLAG_NOT_SUPPORTED);
	CHECK(runtime.InstanceCreate(context1, Ajm::AJM_CODEC_AT9, 0x411, &instance) == Ajm::AJM_OK);
	CHECK((instance >> 14u) == Ajm::AJM_CODEC_AT9);
	CHECK(state.created_channels == 2 && state.created_format == 0);
	CHECK(runtime.ModuleUnregister(context1, Ajm::AJM_CODEC_AT9) == Ajm::AJM_ERROR_BUSY);

	Ajm::Result init_result {123, 456};
	CHECK(InitializeInstance(&runtime, context1, instance, &init_result));
	CHECK(init_result.result == Ajm::AJM_OK);
	CHECK(init_result.internal_result == Ajm::AJM_OK);
	CHECK(state.initialize_calls == 1);

	alignas(8) std::array<uint8_t, 0x50> info_batch {};
	std::array<uint8_t, sizeof(Ajm::Result) + sizeof(Ajm::At9Info)> info_sideband {};
	CHECK(Ajm::BatchJobRunBufferRa(info_batch.data(), instance, 0x801, nullptr, 0, nullptr, 0, info_sideband.data(),
	                              info_sideband.size(), nullptr) == info_batch.data() + info_batch.size());
	uint32_t        info_batch_id = 0;
	Ajm::BatchError error {};
	CHECK(runtime.BatchStart(context1, info_batch.data(), info_batch.size(), 90, &error, &info_batch_id) == Ajm::AJM_OK);
	Ajm::Result info_result {};
	Ajm::At9Info info {};
	std::memcpy(&info_result, info_sideband.data(), sizeof(info_result));
	std::memcpy(&info, info_sideband.data() + sizeof(info_result), sizeof(info));
	CHECK(info_result.result == Ajm::AJM_OK);
	CHECK(info.superframe_size == 8);
	CHECK(info.frames_in_superframe == 2);
	CHECK(info.next_frame_size == 8);
	CHECK(info.frame_samples == 2);
	CHECK(runtime.BatchWait(context1, info_batch_id, std::numeric_limits<uint32_t>::max(), &error) == Ajm::AJM_OK);
	CHECK(runtime.BatchWait(context1, info_batch_id, 0, &error) == Ajm::AJM_ERROR_INVALID_BATCH);

	alignas(8) std::array<uint8_t, 0x50> run_batch {};
	std::array<uint8_t, 8> input {1, 2, 3, 4, 5, 6, 7, 8};
	std::array<uint8_t, 16> output {};
	std::array<uint8_t, sizeof(Ajm::Result) + sizeof(Ajm::Stream) + sizeof(Ajm::MultipleFrame)> run_sideband {};
	CHECK(Ajm::BatchJobRunBufferRa(run_batch.data(), instance, 0x800000001001ull, input.data(), input.size(), output.data(),
	                              output.size(), run_sideband.data(), run_sideband.size(), nullptr) ==
	      run_batch.data() + run_batch.size());
	uint32_t run_batch_id = 0;
	CHECK(runtime.BatchStart(context1, run_batch.data(), run_batch.size(), 40, &error, &run_batch_id) == Ajm::AJM_OK);
	Ajm::Result run_result {};
	Ajm::Stream stream {};
	Ajm::MultipleFrame multiple {};
	std::memcpy(&run_result, run_sideband.data(), sizeof(run_result));
	std::memcpy(&stream, run_sideband.data() + sizeof(run_result), sizeof(stream));
	std::memcpy(&multiple, run_sideband.data() + sizeof(run_result) + sizeof(stream), sizeof(multiple));
	CHECK(run_result.result == Ajm::AJM_OK);
	CHECK(stream.input_consumed == 8);
	CHECK(stream.output_written == 16);
	CHECK(stream.total_decoded_samples == 4);
	CHECK(multiple.num_frames == 2);
	CHECK(state.decode_calls == 2);
	CHECK(runtime.BatchWait(context1, run_batch_id, 0, &error) == Ajm::AJM_OK);

	InitializeData unsupported_init {{0xfe, 0x74, 0x1f, 0xe0}, 0};
	Ajm::Result unsupported_result {};
	alignas(8) std::array<uint8_t, 0x40> unsupported_batch {};
	const uint32_t init_calls_before  = state.initialize_calls;
	const uint32_t reset_calls_before = state.reset_calls;
	CHECK(Ajm::BatchJobControlBufferRa(unsupported_batch.data(), instance, 0x6009, &unsupported_init, sizeof(unsupported_init),
	                                  &unsupported_result, sizeof(unsupported_result), nullptr) ==
	      unsupported_batch.data() + unsupported_batch.size());
	uint32_t unsupported_id = 0;
	CHECK(runtime.BatchStart(context1, unsupported_batch.data(), unsupported_batch.size(), 40, &error, &unsupported_id) == Ajm::AJM_OK);
	CHECK((unsupported_result.result & Ajm::AJM_JOB_UNSUPPORTED_FLAG) != 0);
	CHECK(state.initialize_calls == init_calls_before);
	CHECK(state.reset_calls == reset_calls_before);
	CHECK(runtime.BatchWait(context1, unsupported_id, 0, &error) == Ajm::AJM_OK);

	Ajm::Gapless gapless {3, 1, 0};
	Ajm::Result  gapless_result {};
	alignas(8) std::array<uint8_t, 0x40> gapless_batch {};
	CHECK(Ajm::BatchJobControlBufferRa(gapless_batch.data(), instance, 0x200000002001ull, &gapless, sizeof(gapless),
	                                  &gapless_result, sizeof(gapless_result), nullptr) == gapless_batch.data() + gapless_batch.size());
	uint32_t gapless_id = 0;
	CHECK(runtime.BatchStart(context1, gapless_batch.data(), gapless_batch.size(), 40, &error, &gapless_id) == Ajm::AJM_OK);
	CHECK(gapless_result.result == Ajm::AJM_OK);
	CHECK(state.reset_calls == reset_calls_before + 1);
	CHECK(runtime.BatchWait(context1, gapless_id, 0, &error) == Ajm::AJM_OK);

	output.fill(0);
	run_sideband.fill(0);
	CHECK(Ajm::BatchJobRunBufferRa(run_batch.data(), instance, 0x800000001001ull, input.data(), input.size(), output.data(), 12,
	                              run_sideband.data(), run_sideband.size(), nullptr) == run_batch.data() + run_batch.size());
	CHECK(runtime.BatchStart(context1, run_batch.data(), run_batch.size(), 40, &error, &run_batch_id) == Ajm::AJM_OK);
	std::memcpy(&stream, run_sideband.data() + sizeof(Ajm::Result), sizeof(stream));
	std::memcpy(&multiple, run_sideband.data() + sizeof(Ajm::Result) + sizeof(Ajm::Stream), sizeof(multiple));
	CHECK(stream.input_consumed == 8);
	CHECK(stream.output_written == 12);
	CHECK(stream.total_decoded_samples == 0);
	CHECK(multiple.num_frames == 2);
	CHECK(runtime.BatchWait(context1, run_batch_id, 0, &error) == Ajm::AJM_OK);

	Ajm::Gapless mid_superframe_gapless {1, 0, 0};
	Ajm::Result  mid_superframe_control_result {};
	const uint32_t mid_superframe_resets_before = state.reset_calls;
	CHECK(Ajm::BatchJobControlBufferRa(gapless_batch.data(), instance, 0x200000002001ull, &mid_superframe_gapless,
	                                  sizeof(mid_superframe_gapless), &mid_superframe_control_result,
	                                  sizeof(mid_superframe_control_result), nullptr) == gapless_batch.data() + gapless_batch.size());
	CHECK(runtime.BatchStart(context1, gapless_batch.data(), gapless_batch.size(), 40, &error, &gapless_id) == Ajm::AJM_OK);
	CHECK(mid_superframe_control_result.result == Ajm::AJM_OK);
	CHECK(state.reset_calls == mid_superframe_resets_before + 1);
	CHECK(runtime.BatchWait(context1, gapless_id, 0, &error) == Ajm::AJM_OK);

	std::array<uint8_t, 4> mid_superframe_output {};
	run_sideband.fill(0);
	CHECK(Ajm::BatchJobRunBufferRa(run_batch.data(), instance, 0x800000001001ull, input.data(), input.size(),
	                              mid_superframe_output.data(), mid_superframe_output.size(), run_sideband.data(),
	                              run_sideband.size(), nullptr) == run_batch.data() + run_batch.size());
	CHECK(runtime.BatchStart(context1, run_batch.data(), run_batch.size(), 40, &error, &run_batch_id) == Ajm::AJM_OK);
	std::memcpy(&run_result, run_sideband.data(), sizeof(run_result));
	std::memcpy(&stream, run_sideband.data() + sizeof(Ajm::Result), sizeof(stream));
	std::memcpy(&multiple, run_sideband.data() + sizeof(Ajm::Result) + sizeof(Ajm::Stream), sizeof(multiple));
	CHECK(run_result.result == Ajm::AJM_OK);
	CHECK(stream.input_consumed == 8);
	CHECK(stream.output_written == 4);
	CHECK(stream.total_decoded_samples == 0);
	CHECK(multiple.num_frames == 2);
	CHECK(state.reset_calls == mid_superframe_resets_before + 2);
	CHECK(runtime.BatchWait(context1, run_batch_id, 0, &error) == Ajm::AJM_OK);

	// Repeat the same mid-superframe gapless loop without MULTIPLE_FRAMES so
	// the zero-output drain must survive across two separate Run jobs.
	Ajm::Result split_control_result {};
	const uint32_t split_resets_before = state.reset_calls;
	CHECK(Ajm::BatchJobControlBufferRa(gapless_batch.data(), instance, 0x200000002001ull, &mid_superframe_gapless,
	                                  sizeof(mid_superframe_gapless), &split_control_result,
	                                  sizeof(split_control_result), nullptr) == gapless_batch.data() + gapless_batch.size());
	CHECK(runtime.BatchStart(context1, gapless_batch.data(), gapless_batch.size(), 40, &error, &gapless_id) == Ajm::AJM_OK);
	CHECK(split_control_result.result == Ajm::AJM_OK);
	CHECK(state.reset_calls == split_resets_before + 1);
	CHECK(runtime.BatchWait(context1, gapless_id, 0, &error) == Ajm::AJM_OK);

	mid_superframe_output.fill(0);
	run_sideband.fill(0);
	CHECK(Ajm::BatchJobRunBufferRa(run_batch.data(), instance, 0x800000000001ull, input.data(), input.size(),
	                              mid_superframe_output.data(), mid_superframe_output.size(), run_sideband.data(),
	                              run_sideband.size(), nullptr) == run_batch.data() + run_batch.size());
	CHECK(runtime.BatchStart(context1, run_batch.data(), run_batch.size(), 40, &error, &run_batch_id) == Ajm::AJM_OK);
	std::memcpy(&run_result, run_sideband.data(), sizeof(run_result));
	std::memcpy(&stream, run_sideband.data() + sizeof(Ajm::Result), sizeof(stream));
	CHECK(run_result.result == Ajm::AJM_OK);
	CHECK(stream.input_consumed == 3);
	CHECK(stream.output_written == 4);
	CHECK(stream.total_decoded_samples == 1);
	CHECK(state.reset_calls == split_resets_before + 1);
	CHECK(runtime.BatchWait(context1, run_batch_id, 0, &error) == Ajm::AJM_OK);

	mid_superframe_output.fill(0x5a);
	run_sideband.fill(0);
	CHECK(Ajm::BatchJobRunBufferRa(run_batch.data(), instance, 0x800000000001ull, input.data() + 3, input.size() - 3,
	                              mid_superframe_output.data(), mid_superframe_output.size(), run_sideband.data(),
	                              run_sideband.size(), nullptr) == run_batch.data() + run_batch.size());
	CHECK(runtime.BatchStart(context1, run_batch.data(), run_batch.size(), 40, &error, &run_batch_id) == Ajm::AJM_OK);
	std::memcpy(&run_result, run_sideband.data(), sizeof(run_result));
	std::memcpy(&stream, run_sideband.data() + sizeof(Ajm::Result), sizeof(stream));
	CHECK(run_result.result == Ajm::AJM_OK);
	CHECK(stream.input_consumed == 5);
	CHECK(stream.output_written == 0);
	CHECK(stream.total_decoded_samples == 0);
	CHECK(std::all_of(mid_superframe_output.begin(), mid_superframe_output.end(), [](uint8_t value) { return value == 0x5a; }));
	CHECK(state.reset_calls == split_resets_before + 2);
	CHECK(runtime.BatchWait(context1, run_batch_id, 0, &error) == Ajm::AJM_OK);

	CHECK(runtime.InstanceDestroy(context1, instance) == Ajm::AJM_OK);
	CHECK(runtime.InstanceDestroy(context1, instance) == Ajm::AJM_ERROR_INVALID_INSTANCE);
	uint32_t replacement = 0;
	CHECK(runtime.InstanceCreate(context1, Ajm::AJM_CODEC_AT9, 0x411, &replacement) == Ajm::AJM_OK);
	CHECK(replacement != instance);
	CHECK(runtime.InstanceDestroy(context1, replacement) == Ajm::AJM_OK);
	CHECK(runtime.ModuleUnregister(context1, Ajm::AJM_CODEC_AT9) == Ajm::AJM_OK);
	CHECK(runtime.ModuleUnregister(context1, Ajm::AJM_CODEC_AT9) == Ajm::AJM_ERROR_CODEC_NOT_REGISTERED);
	CHECK(runtime.Finalize(context1) == Ajm::AJM_OK);
	CHECK(runtime.Finalize(context1) == Ajm::AJM_ERROR_INVALID_CONTEXT);
	CHECK(runtime.Finalize(context2) == Ajm::AJM_OK);
}

void TestMalformedAndFailClosed()
{
	FakeState   state {};
	FakeFactory factory(&state);
	Ajm::Runtime runtime(&factory);
	uint32_t context  = 0;
	uint32_t instance = 0;
	CHECK(CreateFakeInstance(&runtime, &context, &instance, &state));

	InitializeData init {{0xfe, 0x74, 0x1f, 0xe0}, 0};
	Ajm::Result   sideband {0x11111111, 0x22222222};
	alignas(8) std::array<uint8_t, 0x40> valid {};
	CHECK(Ajm::BatchJobControlBufferRa(valid.data(), instance, 0x6001, &init, sizeof(init), &sideband, sizeof(sideband),
	                                  reinterpret_cast<void*>(UINT64_C(0x1234))) == valid.data() + valid.size());

	for (uint32_t size = 1; size < valid.size(); size++)
	{
		uint32_t        batch_id = 0xa5a5a5a5u;
		Ajm::BatchError error {};
		CHECK(runtime.BatchStart(context, valid.data(), size, 40, &error, &batch_id) == Ajm::AJM_ERROR_MALFORMED_BATCH);
		CHECK(batch_id == 0xa5a5a5a5u);
		CHECK(state.initialize_calls == 0);
		CHECK(sideband.result == 0x11111111);
	}

	uint32_t        empty_id = 0;
	Ajm::BatchError error {};
	CHECK(runtime.BatchStart(context, nullptr, 0, 40, &error, &empty_id) == Ajm::AJM_OK);
	CHECK(empty_id != 0);
	CHECK(runtime.BatchWait(context, empty_id, 0, &error) == Ajm::AJM_OK);

	alignas(8) std::array<uint8_t, 0x50> two_jobs {};
	std::copy(valid.begin(), valid.end(), two_jobs.begin());
	WriteU32(two_jobs.data() + 0x40, (instance << 6u));
	WriteU32(two_jobs.data() + 0x44, 8);
	WriteU32(two_jobs.data() + 0x48, 55);
	WriteU32(two_jobs.data() + 0x4c, 0);
	uint32_t bad_id = 0xfacefeedu;
	CHECK(runtime.BatchStart(context, two_jobs.data(), two_jobs.size(), 40, &error, &bad_id) == Ajm::AJM_ERROR_MALFORMED_BATCH);
	CHECK(error.error_code == Ajm::AJM_ERROR_INVALID_OPCODE);
	CHECK(error.job_addr == two_jobs.data() + 0x40);
	CHECK(error.cmd_offset == 8);
	CHECK(bad_id == 0xfacefeedu);
	CHECK(state.initialize_calls == 0);
	CHECK(sideband.result == 0x11111111);

	alignas(8) std::array<uint8_t, 0x50> duplicate_ra {};
	std::copy(valid.begin(), valid.begin() + 0x18, duplicate_ra.begin());
	std::copy(valid.begin() + 8, valid.begin() + 0x18, duplicate_ra.begin() + 0x18);
	std::copy(valid.begin() + 0x18, valid.end(), duplicate_ra.begin() + 0x28);
	WriteU32(duplicate_ra.data() + 4, 0x48);
	CHECK(runtime.BatchStart(context, duplicate_ra.data(), duplicate_ra.size(), 40, &error, &bad_id) ==
	      Ajm::AJM_ERROR_MALFORMED_BATCH);
	CHECK(error.job_addr == duplicate_ra.data());
	CHECK(error.cmd_offset == 0x18);
	CHECK(error.job_ra == reinterpret_cast<void*>(UINT64_C(0x1234)));
	CHECK(state.initialize_calls == 0);

	auto reserved = valid;
	WriteU32(reserved.data(), ReadU32(reserved.data()) | (1u << 26u));
	CHECK(runtime.BatchStart(context, reserved.data(), reserved.size(), 40, &error, &bad_id) == Ajm::AJM_ERROR_MALFORMED_BATCH);
	CHECK(state.initialize_calls == 0);

	auto invalid_address = valid;
	WriteU64(invalid_address.data() + 32, 0);
	CHECK(runtime.BatchStart(context, invalid_address.data(), invalid_address.size(), 40, &error, &bad_id) ==
	      Ajm::AJM_ERROR_INVALID_ADDRESS);
	CHECK(error.error_code == Ajm::AJM_ERROR_INVALID_ADDRESS);
	CHECK(state.initialize_calls == 0);

	auto overflowing_address = valid;
	WriteU64(overflowing_address.data() + 32, std::numeric_limits<uintptr_t>::max() - 3u);
	CHECK(runtime.BatchStart(context, overflowing_address.data(), overflowing_address.size(), 40, &error, &bad_id) ==
	      Ajm::AJM_ERROR_INVALID_ADDRESS);
	CHECK(error.error_code == Ajm::AJM_ERROR_INVALID_ADDRESS);
	CHECK(error.cmd_offset == 0x18);
	CHECK(state.initialize_calls == 0);

	bad_id = 0xfacefeedu;
	auto* overflowing_batch = reinterpret_cast<uint8_t*>(std::numeric_limits<uintptr_t>::max() - 3u);
	CHECK(runtime.BatchStart(context, overflowing_batch, 8, 40, &error, &bad_id) == Ajm::AJM_ERROR_INVALID_ADDRESS);
	CHECK(error.error_code == Ajm::AJM_ERROR_INVALID_ADDRESS);
	CHECK(error.job_addr == overflowing_batch);
	CHECK(bad_id == 0xfacefeedu);
	CHECK(state.initialize_calls == 0);

	auto invalid_instance = valid;
	WriteU32(invalid_instance.data(), ((instance + 1u) << 6u));
	CHECK(runtime.BatchStart(context, invalid_instance.data(), invalid_instance.size(), 40, &error, &bad_id) ==
	      Ajm::AJM_ERROR_INVALID_INSTANCE);
	CHECK(error.job_ra == reinterpret_cast<void*>(UINT64_C(0x1234)));
	CHECK(state.initialize_calls == 0);

	CHECK(runtime.BatchStart(context, valid.data(), 64u * 1024u * 1024u + 1u, 40, &error, &bad_id) ==
	      Ajm::AJM_ERROR_BUFFER_TOO_BIG);
	CHECK(state.initialize_calls == 0);

	Ajm::Result unsupported {};
	alignas(8) std::array<uint8_t, 0x40> unsupported_batch {};
	CHECK(Ajm::BatchJobControlBufferRa(unsupported_batch.data(), instance, 0x6009, &init, sizeof(init), &unsupported,
	                                  sizeof(unsupported), nullptr) == unsupported_batch.data() + unsupported_batch.size());
	uint32_t unsupported_id = 0;
	CHECK(runtime.BatchStart(context, unsupported_batch.data(), unsupported_batch.size(), 40, &error, &unsupported_id) == Ajm::AJM_OK);
	CHECK((unsupported.result & Ajm::AJM_JOB_UNSUPPORTED_FLAG) != 0);
	CHECK(state.initialize_calls == 0);
	CHECK(state.reset_calls == 0);
	CHECK(runtime.BatchWait(context, unsupported_id, 0, &error) == Ajm::AJM_OK);

	CHECK(runtime.InstanceDestroy(context, instance) == Ajm::AJM_OK);
	CHECK(runtime.ModuleUnregister(context, Ajm::AJM_CODEC_AT9) == Ajm::AJM_OK);
	CHECK(runtime.Finalize(context) == Ajm::AJM_OK);
}

void TestCodecFailureIsolation()
{
	FakeState   state {};
	FakeFactory factory(&state);
	Ajm::Runtime runtime(&factory);
	uint32_t context  = 0;
	uint32_t instance = 0;
	CHECK(CreateFakeInstance(&runtime, &context, &instance, &state));

	state.fail_initialize = true;
	Ajm::Result init_result {};
	CHECK(InitializeInstance(&runtime, context, instance, &init_result));
	CHECK((init_result.result & Ajm::AJM_JOB_CODEC_ERROR) != 0);
	CHECK(init_result.internal_result == -70);
	CHECK(state.initialize_calls == 1);

	alignas(8) std::array<uint8_t, 0x50> info_batch {};
	std::array<uint8_t, sizeof(Ajm::Result) + sizeof(Ajm::At9Info)> info_sideband {};
	CHECK(Ajm::BatchJobRunBufferRa(info_batch.data(), instance, 0x801, nullptr, 0, nullptr, 0, info_sideband.data(),
	                              info_sideband.size(), nullptr) == info_batch.data() + info_batch.size());
	uint32_t        batch_id = 0;
	Ajm::BatchError error {};
	CHECK(runtime.BatchStart(context, info_batch.data(), info_batch.size(), 40, &error, &batch_id) == Ajm::AJM_OK);
	Ajm::Result info_result {};
	std::memcpy(&info_result, info_sideband.data(), sizeof(info_result));
	CHECK((info_result.result & Ajm::AJM_JOB_NOT_INITIALIZED) != 0);
	CHECK(runtime.BatchWait(context, batch_id, 0, &error) == Ajm::AJM_OK);

	state.fail_initialize = false;
	init_result           = {};
	CHECK(InitializeInstance(&runtime, context, instance, &init_result));
	CHECK(init_result.result == Ajm::AJM_OK);
	CHECK(state.initialize_calls == 2);

	state.fail_reset = true;
	Ajm::Gapless gapless {100, 0, 0};
	Ajm::Result  reset_result {};
	alignas(8) std::array<uint8_t, 0x40> reset_batch {};
	CHECK(Ajm::BatchJobControlBufferRa(reset_batch.data(), instance, 0x200000002001ull, &gapless, sizeof(gapless),
	                                  &reset_result, sizeof(reset_result), nullptr) == reset_batch.data() + reset_batch.size());
	CHECK(runtime.BatchStart(context, reset_batch.data(), reset_batch.size(), 40, &error, &batch_id) == Ajm::AJM_OK);
	CHECK((reset_result.result & Ajm::AJM_JOB_CODEC_ERROR) != 0);
	CHECK(reset_result.internal_result == -71);
	CHECK(runtime.BatchWait(context, batch_id, 0, &error) == Ajm::AJM_OK);

	info_sideband.fill(0);
	CHECK(runtime.BatchStart(context, info_batch.data(), info_batch.size(), 40, &error, &batch_id) == Ajm::AJM_OK);
	std::memcpy(&info_result, info_sideband.data(), sizeof(info_result));
	CHECK((info_result.result & Ajm::AJM_JOB_NOT_INITIALIZED) != 0);
	CHECK(runtime.BatchWait(context, batch_id, 0, &error) == Ajm::AJM_OK);

	state.fail_reset = false;
	init_result      = {};
	CHECK(InitializeInstance(&runtime, context, instance, &init_result));
	CHECK(init_result.result == Ajm::AJM_OK);

	state.dishonest_decode = true;
	std::array<uint8_t, 8> input {1, 2, 3, 4, 5, 6, 7, 8};
	std::array<uint8_t, 16> output {};
	output.fill(0xcc);
	std::array<uint8_t, sizeof(Ajm::Result) + sizeof(Ajm::Stream) + sizeof(Ajm::MultipleFrame)> run_sideband {};
	alignas(8) std::array<uint8_t, 0x50> run_batch {};
	CHECK(Ajm::BatchJobRunBufferRa(run_batch.data(), instance, 0x800000001001ull, input.data(), input.size(), output.data(),
	                              output.size(), run_sideband.data(), run_sideband.size(), nullptr) ==
	      run_batch.data() + run_batch.size());
	CHECK(runtime.BatchStart(context, run_batch.data(), run_batch.size(), 40, &error, &batch_id) == Ajm::AJM_OK);
	Ajm::Result run_result {};
	Ajm::Stream stream {};
	std::memcpy(&run_result, run_sideband.data(), sizeof(run_result));
	std::memcpy(&stream, run_sideband.data() + sizeof(run_result), sizeof(stream));
	CHECK((run_result.result & Ajm::AJM_JOB_CODEC_ERROR) != 0);
	CHECK(run_result.internal_result != 0);
	CHECK(stream.input_consumed == 0);
	CHECK(stream.output_written == 0);
	CHECK(std::all_of(output.begin(), output.end(), [](uint8_t value) { return value == 0xcc; }));
	CHECK(runtime.BatchWait(context, batch_id, 0, &error) == Ajm::AJM_OK);

	CHECK(runtime.InstanceDestroy(context, instance) == Ajm::AJM_OK);
	CHECK(runtime.ModuleUnregister(context, Ajm::AJM_CODEC_AT9) == Ajm::AJM_OK);
	CHECK(runtime.Finalize(context) == Ajm::AJM_OK);
}

void WriteBits(uint8_t* data, int* bit_position, uint32_t value, int count)
{
	for (int bit = count - 1; bit >= 0; bit--)
	{
		const int byte_index = *bit_position / 8;
		const int bit_index  = 7 - (*bit_position % 8);
		data[byte_index] |= static_cast<uint8_t>(((value >> bit) & 1u) << bit_index);
		(*bit_position)++;
	}
}

uint32_t NextRandom(uint32_t* state)
{
	*state ^= *state << 13u;
	*state ^= *state >> 17u;
	*state ^= *state << 5u;
	return *state;
}

void WriteRandomBits(uint8_t* data, int* bit_position, int count, uint32_t* state)
{
	for (int bit = 0; bit < count; bit++)
	{
		WriteBits(data, bit_position, NextRandom(state) & 1u, 1);
	}
}

void WriteStereoFrame(uint8_t* data, int* bit_position, bool first_in_superframe, uint32_t* random_state)
{
	WriteBits(data, bit_position, first_in_superframe ? 0u : 1u, 1);
	WriteBits(data, bit_position, 0, 1);
	WriteBits(data, bit_position, 0, 4);
	WriteBits(data, bit_position, 0, 4);
	WriteBits(data, bit_position, 0, 1);
	WriteBits(data, bit_position, 0, 2);
	WriteBits(data, bit_position, 0, 6);
	WriteBits(data, bit_position, 0, 6);
	WriteBits(data, bit_position, 0, 5);
	WriteBits(data, bit_position, 0, 5);
	WriteBits(data, bit_position, 0, 4);
	WriteBits(data, bit_position, 0, 1);
	WriteBits(data, bit_position, 0, 1);
	WriteBits(data, bit_position, 0, 1);
	WriteBits(data, bit_position, 1, 2);
	WriteBits(data, bit_position, 3, 2);
	for (int factor = 0; factor < 10; factor++)
	{
		WriteBits(data, bit_position, 20, 5);
	}
	WriteRandomBits(data, bit_position, 528, random_state);
	WriteBits(data, bit_position, 1, 2);
	WriteBits(data, bit_position, 3, 2);
	for (int factor = 0; factor < 10; factor++)
	{
		WriteBits(data, bit_position, 0, 3);
	}
	WriteRandomBits(data, bit_position, 528, random_state);
	*bit_position = (*bit_position + 7) & ~7;
}

std::array<uint8_t, 512> MakeStereoAt9Superframe()
{
	std::array<uint8_t, 512> superframe {};
	int                      bit_position = 0;
	uint32_t                 random_state = 0x4b595459u;
	WriteStereoFrame(superframe.data(), &bit_position, true, &random_state);
	CHECK(bit_position / 8 == 149);
	WriteStereoFrame(superframe.data(), &bit_position, false, &random_state);
	WriteRandomBits(superframe.data(), &bit_position, static_cast<int>(superframe.size() * 8u) - bit_position, &random_state);
	return superframe;
}

std::array<uint8_t, 256> MakeStereoAt9Frame()
{
	std::array<uint8_t, 256> frame {};
	int                      bit_position = 0;
	uint32_t                 random_state = 0x4b595459u;
	WriteStereoFrame(frame.data(), &bit_position, true, &random_state);
	CHECK(bit_position / 8 == 149);
	WriteRandomBits(frame.data(), &bit_position, static_cast<int>(frame.size() * 8u) - bit_position, &random_state);
	return frame;
}

std::array<uint8_t, 256> MakeInvalidBexAt9Frame(uint32_t band_delta)
{
	std::array<uint8_t, 256> frame {};
	int                      bit_position = 0;
	WriteBits(frame.data(), &bit_position, 0, 1);
	WriteBits(frame.data(), &bit_position, 0, 1);
	WriteBits(frame.data(), &bit_position, band_delta, 4);
	WriteBits(frame.data(), &bit_position, 0, 4);
	WriteBits(frame.data(), &bit_position, 1, 1);
	WriteBits(frame.data(), &bit_position, band_delta, 4);
	return frame;
}

std::array<uint8_t, 256> MakeWideGradientAt9Frame()
{
	std::array<uint8_t, 256> frame {};
	int                      bit_position = 0;
	WriteBits(frame.data(), &bit_position, 0, 1);
	WriteBits(frame.data(), &bit_position, 0, 1);
	WriteBits(frame.data(), &bit_position, 0, 4);
	WriteBits(frame.data(), &bit_position, 0, 4);
	WriteBits(frame.data(), &bit_position, 0, 1);
	WriteBits(frame.data(), &bit_position, 0, 2);
	WriteBits(frame.data(), &bit_position, 0, 6);
	WriteBits(frame.data(), &bit_position, 46, 6);
	WriteBits(frame.data(), &bit_position, 0, 5);
	WriteBits(frame.data(), &bit_position, 31, 5);
	WriteBits(frame.data(), &bit_position, 0, 4);
	WriteBits(frame.data(), &bit_position, 0, 1);
	WriteBits(frame.data(), &bit_position, 0, 1);
	WriteBits(frame.data(), &bit_position, 0, 1);
	WriteBits(frame.data(), &bit_position, 2, 2);
	frame.back() = 0x80;
	return frame;
}

template <size_t InputSize, size_t OutputSize>
void CheckRealAtrac9Decode(const std::array<uint8_t, 4>& config, const std::array<uint8_t, InputSize>& input,
	                       int32_t expected_input, int32_t expected_output, uint32_t expected_frames,
	                       uint64_t expected_samples, int expected_peak, uint64_t expected_hash)
{
	Ajm::Runtime runtime;
	uint32_t     context  = 0;
	uint32_t     instance = 0;
	CHECK(runtime.Initialize(0, &context) == Ajm::AJM_OK);
	CHECK(runtime.ModuleRegister(context, Ajm::AJM_CODEC_AT9, 0) == Ajm::AJM_OK);
	CHECK(runtime.InstanceCreate(context, Ajm::AJM_CODEC_AT9, 0x411, &instance) == Ajm::AJM_OK);

	Ajm::Result init_result {};
	CHECK(InitializeInstance(&runtime, context, instance, &init_result, config));
	CHECK(init_result.result == Ajm::AJM_OK);

	std::array<uint8_t, OutputSize> output {};
	std::array<uint8_t, sizeof(Ajm::Result) + sizeof(Ajm::Stream) + sizeof(Ajm::MultipleFrame)> sideband {};
	alignas(8) std::array<uint8_t, 0x50> batch {};
	CHECK(Ajm::BatchJobRunBufferRa(batch.data(), instance, 0x800000001001ull, input.data(), input.size(), output.data(),
	                              output.size(), sideband.data(), sideband.size(), nullptr) == batch.data() + batch.size());
	uint32_t        batch_id = 0;
	Ajm::BatchError error {};
	CHECK(runtime.BatchStart(context, batch.data(), batch.size(), 40, &error, &batch_id) == Ajm::AJM_OK);
	Ajm::Result result {};
	Ajm::Stream stream {};
	Ajm::MultipleFrame multiple {};
	std::memcpy(&result, sideband.data(), sizeof(result));
	std::memcpy(&stream, sideband.data() + sizeof(result), sizeof(stream));
	std::memcpy(&multiple, sideband.data() + sizeof(result) + sizeof(stream), sizeof(multiple));
	CHECK(result.result == Ajm::AJM_OK);
	CHECK(result.internal_result == Ajm::AJM_OK);
	CHECK(stream.input_consumed == expected_input);
	CHECK(stream.output_written == expected_output);
	CHECK(stream.total_decoded_samples == expected_samples);
	CHECK(multiple.num_frames == expected_frames);

	uint64_t hash = UINT64_C(14695981039346656037);
	int      peak = 0;
	for (uint8_t byte: output)
	{
		hash ^= byte;
		hash *= UINT64_C(1099511628211);
	}
	for (size_t offset = 0; offset < output.size(); offset += sizeof(int16_t))
	{
		int16_t sample = 0;
		std::memcpy(&sample, output.data() + offset, sizeof(sample));
		const int magnitude = sample < 0 ? -static_cast<int>(sample) : static_cast<int>(sample);
		peak = std::max(peak, magnitude);
	}
	CHECK(peak == expected_peak);
	CHECK(hash == expected_hash);
	CHECK(runtime.BatchWait(context, batch_id, 0, &error) == Ajm::AJM_OK);
	CHECK(runtime.InstanceDestroy(context, instance) == Ajm::AJM_OK);
	CHECK(runtime.ModuleUnregister(context, Ajm::AJM_CODEC_AT9) == Ajm::AJM_OK);
	CHECK(runtime.Finalize(context) == Ajm::AJM_OK);
}

void TestRealAtrac9Decode()
{
	const auto frame = MakeStereoAt9Frame();
	CheckRealAtrac9Decode<256, 1024>({0xfe, 0x74, 0x1f, 0xe0}, frame, 256, 1024, 1, 256, 182,
	                                      UINT64_C(0x68219b61f0dc13c6));

	const auto superframe = MakeStereoAt9Superframe();
	CheckRealAtrac9Decode<512, 2048>({0xfe, 0x74, 0x1f, 0xe8}, superframe, 512, 2048, 2, 512, 182,
	                                       UINT64_C(0x65a7beb84fe6ffa1));
}

void CheckMalformedAtrac9Decode(const std::array<uint8_t, 256>& input)
{
	Ajm::Runtime runtime;
	uint32_t     context  = 0;
	uint32_t     instance = 0;
	CHECK(runtime.Initialize(0, &context) == Ajm::AJM_OK);
	CHECK(runtime.ModuleRegister(context, Ajm::AJM_CODEC_AT9, 0) == Ajm::AJM_OK);
	CHECK(runtime.InstanceCreate(context, Ajm::AJM_CODEC_AT9, 0x411, &instance) == Ajm::AJM_OK);

	Ajm::Result init_result {};
	CHECK(InitializeInstance(&runtime, context, instance, &init_result));
	CHECK(init_result.result == Ajm::AJM_OK);

	std::array<uint8_t, 1024> output {};
	output.fill(0xcc);
	std::array<uint8_t, sizeof(Ajm::Result) + sizeof(Ajm::Stream)> sideband {};
	alignas(8) std::array<uint8_t, 0x50> batch {};
	CHECK(Ajm::BatchJobRunBufferRa(batch.data(), instance, 0x800000000001ull, input.data(), input.size(), output.data(),
	                              output.size(), sideband.data(), sideband.size(), nullptr) == batch.data() + batch.size());
	uint32_t        batch_id = 0;
	Ajm::BatchError error {};
	CHECK(runtime.BatchStart(context, batch.data(), batch.size(), 40, &error, &batch_id) == Ajm::AJM_OK);
	Ajm::Result result {};
	Ajm::Stream stream {};
	std::memcpy(&result, sideband.data(), sizeof(result));
	std::memcpy(&stream, sideband.data() + sizeof(result), sizeof(stream));
	CHECK((result.result & Ajm::AJM_JOB_CODEC_ERROR) != 0);
	CHECK(result.internal_result != 0);
	CHECK(stream.input_consumed == 0);
	CHECK(stream.output_written == 0);
	CHECK(std::all_of(output.begin(), output.end(), [](uint8_t value) { return value == 0xcc; }));
	CHECK(runtime.BatchWait(context, batch_id, 0, &error) == Ajm::AJM_OK);
	CHECK(runtime.InstanceDestroy(context, instance) == Ajm::AJM_OK);
	CHECK(runtime.ModuleUnregister(context, Ajm::AJM_CODEC_AT9) == Ajm::AJM_OK);
	CHECK(runtime.Finalize(context) == Ajm::AJM_OK);
}

void TestMalformedAtrac9Safety()
{
	CheckMalformedAtrac9Decode(MakeInvalidBexAt9Frame(0));
	CheckMalformedAtrac9Decode(MakeInvalidBexAt9Frame(8));
	CheckMalformedAtrac9Decode(MakeWideGradientAt9Frame());

	std::array<uint8_t, 4> high_octet {0x80, 0x12, 0x34, 0x56};
	BitReaderCxt            reader {};
	InitBitReaderCxt(&reader, high_octet.data(), static_cast<int>(high_octet.size()));
	CHECK(static_cast<uint32_t>(PeekInt(&reader, 32)) == UINT32_C(0x80123456));
	CHECK(reader.Position == 0);
}

void TestGlobalRuntimeShutdownLifetime()
{
	auto&        runtime = Ajm::GlobalRuntime();
	uint32_t     context = 0;
	uint32_t     instance = 0;
	CHECK(runtime.Initialize(0, &context) == Ajm::AJM_OK);
	CHECK(runtime.ModuleRegister(context, Ajm::AJM_CODEC_AT9, 0) == Ajm::AJM_OK);
	CHECK(runtime.InstanceCreate(context, Ajm::AJM_CODEC_AT9, 0x411, &instance) == Ajm::AJM_OK);
	Ajm::Result init_result {};
	CHECK(InitializeInstance(&runtime, context, instance, &init_result));
	CHECK(init_result.result == Ajm::AJM_OK);
}

} // namespace

int main()
{
	TestBuilders();
	TestInstanceFlagLayout();
	TestLifecycleAndSidebands();
	TestMalformedAndFailClosed();
	TestCodecFailureIsolation();
	TestRealAtrac9Decode();
	TestMalformedAtrac9Safety();
	TestGlobalRuntimeShutdownLifetime();

	if (!g_ok)
	{
		return 1;
	}
	std::printf("ajm-test: PASS\n");
	return 0;
}
