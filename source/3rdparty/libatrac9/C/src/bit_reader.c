#include "bit_reader.h"
#include "utility.h"
#include <stdint.h>

static uint32_t PeekIntFallback(BitReaderCxt* br, int bitCount);

void InitBitReaderCxt(BitReaderCxt* br, const void * buffer, int bufferSize)
{
	br->Buffer = buffer;
	br->Position = 0;
	br->LengthBits = bufferSize * 8;
}

int ReadInt(BitReaderCxt* br, const int bits)
{
	const int value = PeekInt(br, bits);
	br->Position += bits;
	return value;
}

int ReadSignedInt(BitReaderCxt* br, const int bits)
{
	const int value = PeekInt(br, bits);
	br->Position += bits;
	return SignExtend32(value, bits);
}

int ReadOffsetBinary(BitReaderCxt* br, const int bits)
{
	const int offset = 1 << (bits - 1);
	const int value = PeekInt(br, bits) - offset;
	br->Position += bits;
	return value;
}

int PeekInt(BitReaderCxt* br, const int bits)
{
	if (br->Position >= br->LengthBits)
	{
		return 0;
	}
	const int remaining = br->LengthBits - br->Position;
	if (bits > remaining)
	{
		const int extraBits = bits - remaining;
		return PeekIntFallback(br, remaining) << extraBits;
	}

	const int byteIndex = br->Position / 8;
	const int bitIndex = br->Position % 8;
	const int numBytes = (bitIndex + bits - 1) / 8 + 1;
	const unsigned char* buffer = br->Buffer;

	uint32_t value;
	switch (numBytes) {
	case 1:
		value = buffer[byteIndex];
		value &= 0xFFu >> bitIndex;
		value >>= 8 - bits - bitIndex;
		break;
	case 2:
		value = (uint32_t)buffer[byteIndex] << 8 | (uint32_t)buffer[byteIndex + 1];
		value &= 0xFFFFu >> bitIndex;
		value >>= 16 - bits - bitIndex;
		break;
	case 3:
		value = (uint32_t)buffer[byteIndex] << 16 | (uint32_t)buffer[byteIndex + 1] << 8
			| (uint32_t)buffer[byteIndex + 2];
		value &= 0xFFFFFFu >> bitIndex;
		value >>= 24 - bits - bitIndex;
		break;
	case 4:
		value = (uint32_t)buffer[byteIndex] << 24
			| (uint32_t)buffer[byteIndex + 1] << 16
			| (uint32_t)buffer[byteIndex + 2] << 8
			| (uint32_t)buffer[byteIndex + 3];
		value &= 0xFFFFFFFFu >> bitIndex;
		value >>= 32 - bits - bitIndex;
		break;
	default:
		value = PeekIntFallback(br, bits);
		break;
	}
	return (int)value;
}

void AlignPosition(BitReaderCxt* br, const unsigned int multiple)
{
	const int position = br->Position;
	if (position % multiple == 0)
	{
		return;
	}

	br->Position = position + multiple - position % multiple;
}

static uint32_t PeekIntFallback(BitReaderCxt* br, int bitCount)
{
	uint32_t value = 0;
	int byteIndex = br->Position / 8;
	int bitIndex = br->Position % 8;
	const unsigned char* buffer = br->Buffer;

	while (bitCount > 0)
	{
		if (bitIndex >= 8)
		{
			bitIndex = 0;
			byteIndex++;
		}

		int bitsToRead = bitCount;
		if (bitsToRead > 8 - bitIndex)
		{
			bitsToRead = 8 - bitIndex;
		}

		const int mask = 0xFF >> bitIndex;
		const int currentByte = (mask & buffer[byteIndex]) >> (8 - bitIndex - bitsToRead);

		value = (value << bitsToRead) | currentByte;
		bitIndex += bitsToRead;
		bitCount -= bitsToRead;
	}
	return value;
}
