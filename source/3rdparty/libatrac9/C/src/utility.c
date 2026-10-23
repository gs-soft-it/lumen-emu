#include "utility.h"
#include <limits.h>
#include <stdint.h>

int Max(int a, int b) { return a > b ? a : b; }
int Min(int a, int b) { return a > b ? b : a; }

unsigned int BitReverse32(unsigned int value, int bitCount)
{
	if (bitCount == 0)
	{
		return 0;
	}

	value = ((value & 0xaaaaaaaa) >> 1) | ((value & 0x55555555) << 1);
	value = ((value & 0xcccccccc) >> 2) | ((value & 0x33333333) << 2);
	value = ((value & 0xf0f0f0f0) >> 4) | ((value & 0x0f0f0f0f) << 4);
	value = ((value & 0xff00ff00) >> 8) | ((value & 0x00ff00ff) << 8);
	value = (value >> 16) | (value << 16);
	return value >> (32 - bitCount);
}

int SignExtend32(int value, int bits)
{
	if (bits <= 0)
	{
		return 0;
	}
	if (bits >= 32)
	{
		return value;
	}

	const uint32_t sign_bit   = UINT32_C(1) << (bits - 1);
	const uint32_t value_mask = (UINT32_C(1) << bits) - 1;
	const uint32_t normalized = (uint32_t)value & value_mask;
	if ((normalized & sign_bit) != 0)
	{
		return (int)((int64_t)normalized - (INT64_C(1) << bits));
	}
	return (int)normalized;
}

short Clamp16(int value)
{
	if (value > SHRT_MAX)
		return SHRT_MAX;
	if (value < SHRT_MIN)
		return SHRT_MIN;
	return (short)value;
}

int Round(double x)
{
	x += 0.5;
	return (int)x - (x < (int)x);
}
