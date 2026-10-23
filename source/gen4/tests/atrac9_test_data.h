// SPDX-FileCopyrightText: Copyright 2026 Lumen Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <vector>
static void Ngs2TestWriteBits(uint8_t* data, int* bit_position, uint32_t value, int count)
{
	for (int bit = count - 1; bit >= 0; --bit)
	{
		const int byte_index = *bit_position / 8;
		const int bit_index  = 7 - (*bit_position % 8);
		data[byte_index] |= static_cast<uint8_t>(((value >> bit) & 1u) << bit_index);
		++*bit_position;
	}
}

static uint32_t Ngs2TestNextRandom(uint32_t* state)
{
	*state ^= *state << 13u;
	*state ^= *state >> 17u;
	*state ^= *state << 5u;
	return *state;
}

static void Ngs2TestWriteRandomBits(uint8_t* data, int* bit_position, int count, uint32_t* state)
{
	for (int bit = 0; bit < count; ++bit)
		Ngs2TestWriteBits(data, bit_position, Ngs2TestNextRandom(state) & 1u, 1);
}

static void Ngs2TestWriteStereoAtrac9Frame(uint8_t* data, int* bit_position,
	                                       bool first_in_superframe, uint32_t* random_state)
{
	Ngs2TestWriteBits(data, bit_position, first_in_superframe ? 0u : 1u, 1);
	Ngs2TestWriteBits(data, bit_position, 0, 1);
	Ngs2TestWriteBits(data, bit_position, 0, 4);
	Ngs2TestWriteBits(data, bit_position, 0, 4);
	Ngs2TestWriteBits(data, bit_position, 0, 1);
	Ngs2TestWriteBits(data, bit_position, 0, 2);
	Ngs2TestWriteBits(data, bit_position, 0, 6);
	Ngs2TestWriteBits(data, bit_position, 0, 6);
	Ngs2TestWriteBits(data, bit_position, 0, 5);
	Ngs2TestWriteBits(data, bit_position, 0, 5);
	Ngs2TestWriteBits(data, bit_position, 0, 4);
	Ngs2TestWriteBits(data, bit_position, 0, 1);
	Ngs2TestWriteBits(data, bit_position, 0, 1);
	Ngs2TestWriteBits(data, bit_position, 0, 1);
	Ngs2TestWriteBits(data, bit_position, 1, 2);
	Ngs2TestWriteBits(data, bit_position, 3, 2);
	for (int factor = 0; factor < 10; ++factor)
		Ngs2TestWriteBits(data, bit_position, 20, 5);
	Ngs2TestWriteRandomBits(data, bit_position, 528, random_state);
	Ngs2TestWriteBits(data, bit_position, 1, 2);
	Ngs2TestWriteBits(data, bit_position, 3, 2);
	for (int factor = 0; factor < 10; ++factor)
		Ngs2TestWriteBits(data, bit_position, 0, 3);
	Ngs2TestWriteRandomBits(data, bit_position, 528, random_state);
	*bit_position = (*bit_position + 7) & ~7;
}

static std::vector<uint8_t> Ngs2TestMakeStereoAtrac9Superframe(uint32_t seed)
{
	std::vector<uint8_t> encoded(1024);
	int bit_position = 0;
	for (uint32_t frame = 0; frame < 4; ++frame)
		Ngs2TestWriteStereoAtrac9Frame(encoded.data(), &bit_position, frame == 0, &seed);
	Ngs2TestWriteRandomBits(encoded.data(), &bit_position,
	                        static_cast<int>(encoded.size() * 8u) - bit_position, &seed);
	return encoded;
}


