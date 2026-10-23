#include "decinit.h"
#include "decoder.h"
#include "libatrac9.h"
#include "structures.h"
#include <stdlib.h>
#include <string.h>

void* Atrac9GetHandle()
{
	return calloc(1, sizeof(Atrac9Handle));
}

void Atrac9ReleaseHandle(void* handle)
{
	free(handle);
}

// Original LUMEN extension. All mutable decoder storage belongs to this one
// allocation. Copy it, then rebind its internal graph without retaining any
// pointer into the source handle. MDCT table references remain shared.
void* Atrac9CloneHandle(const void* handle)
{
	const Atrac9Handle* source = (const Atrac9Handle*)handle;
	if (source == NULL || !source->Initialized ||
		source->Config.ChannelCount < 1 || source->Config.ChannelCount > MAX_CHANNEL_COUNT ||
		source->Config.ChannelConfig.BlockCount < 1 || source->Config.ChannelConfig.BlockCount > MAX_BLOCK_COUNT ||
		source->Frame.Config != &source->Config)
	{
		return NULL;
	}
	int channelCount = 0;
	for (int blockIndex = 0; blockIndex < source->Config.ChannelConfig.BlockCount; ++blockIndex)
	{
		const Block* block = &source->Frame.Blocks[blockIndex];
		if (block->ChannelCount < 1 || block->ChannelCount > MAX_BLOCK_CHANNEL_COUNT ||
			block->Frame != &source->Frame || block->Config != &source->Config)
		{
			return NULL;
		}
		for (int channelIndex = 0; channelIndex < block->ChannelCount; ++channelIndex)
		{
			const Channel* channel = &block->Channels[channelIndex];
			if (channelCount >= MAX_CHANNEL_COUNT ||
				channel->Frame != &source->Frame || channel->Block != block ||
				channel->Config != &source->Config || source->Frame.Channels[channelCount] != channel)
			{
				return NULL;
			}
			++channelCount;
		}
	}
	if (channelCount != source->Config.ChannelCount) return NULL;

	Atrac9Handle* clone = (Atrac9Handle*)malloc(sizeof(Atrac9Handle));
	if (clone == NULL) return NULL;
	memcpy(clone, source, sizeof(Atrac9Handle));
	clone->Frame.Config = &clone->Config;
	for (int blockIndex = 0; blockIndex < MAX_BLOCK_COUNT; ++blockIndex)
	{
		const Block* sourceBlock = &source->Frame.Blocks[blockIndex];
		Block* cloneBlock = &clone->Frame.Blocks[blockIndex];
		if (sourceBlock->Frame != NULL)
		{
			if (sourceBlock->Frame != &source->Frame) goto invalid;
			cloneBlock->Frame = &clone->Frame;
		}
		if (sourceBlock->Config != NULL)
		{
			if (sourceBlock->Config != &source->Config) goto invalid;
			cloneBlock->Config = &clone->Config;
		}
		for (int channelIndex = 0; channelIndex < MAX_BLOCK_CHANNEL_COUNT; ++channelIndex)
		{
			const Channel* sourceChannel = &sourceBlock->Channels[channelIndex];
			Channel* cloneChannel = &cloneBlock->Channels[channelIndex];
			if (sourceChannel->Frame != NULL)
			{
				if (sourceChannel->Frame != &source->Frame) goto invalid;
				cloneChannel->Frame = &clone->Frame;
			}
			if (sourceChannel->Block != NULL)
			{
				if (sourceChannel->Block != sourceBlock) goto invalid;
				cloneChannel->Block = cloneBlock;
			}
			if (sourceChannel->Config != NULL)
			{
				if (sourceChannel->Config != &source->Config) goto invalid;
				cloneChannel->Config = &clone->Config;
			}
		}
	}
	for (int channelIndex = 0; channelIndex < MAX_CHANNEL_COUNT; ++channelIndex)
	{
		const Channel* sourceChannel = source->Frame.Channels[channelIndex];
		if (sourceChannel == NULL) continue;
		int found = 0;
		for (int blockIndex = 0; blockIndex < MAX_BLOCK_COUNT && !found; ++blockIndex)
		{
			for (int blockChannel = 0; blockChannel < MAX_BLOCK_CHANNEL_COUNT; ++blockChannel)
			{
				if (sourceChannel == &source->Frame.Blocks[blockIndex].Channels[blockChannel])
				{
					clone->Frame.Channels[channelIndex] = &clone->Frame.Blocks[blockIndex].Channels[blockChannel];
					found = 1;
					break;
				}
			}
		}
		if (!found) goto invalid;
	}
	return clone;

invalid:
	free(clone);
	return NULL;
}

int Atrac9InitDecoder(void* handle, unsigned char * pConfigData)
{
	return InitDecoder(handle, pConfigData, 16);
}

int Atrac9Decode(void* handle, const unsigned char *pAtrac9Buffer, int atrac9BufferSize, short *pPcmBuffer, int *pNBytesUsed, int nointerleave)
{
	return Decode(handle, pAtrac9Buffer, atrac9BufferSize, (unsigned char*)pPcmBuffer, pNBytesUsed, nointerleave);
}

int Atrac9DecodeS32(void* handle, const unsigned char *pAtrac9Buffer, int atrac9BufferSize, int *pPcmBuffer, int *pNBytesUsed, int nointerleave)
{
	return DecodeS32(handle, pAtrac9Buffer, atrac9BufferSize, pPcmBuffer, pNBytesUsed, nointerleave);
}

int Atrac9DecodeF32(void* handle, const unsigned char *pAtrac9Buffer, int atrac9BufferSize, float *pPcmBuffer, int *pNBytesUsed, int nointerleave)
{
	return DecodeF32(handle, pAtrac9Buffer, atrac9BufferSize, pPcmBuffer, pNBytesUsed, nointerleave);
}

int Atrac9GetCodecInfo(void* handle, Atrac9CodecInfo * pCodecInfo)
{
	return GetCodecInfo(handle, (CodecInfo*)pCodecInfo);
}
