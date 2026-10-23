#include "Emulator/Graphics/Tile.h"

#include "Lumen/Core/DbgAssert.h"
#include "Lumen/Core/String.h"
#include "Lumen/Core/Threads.h"

#include "Emulator/Graphics/AsyncJob.h"
#include "Emulator/Graphics/TextureBc7Contract.h"
#include "Emulator/Profiler.h"

#if LUMEN_COMPILER != LUMEN_COMPILER_CLANG && LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS
#include <intrin.h>
#endif

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iterator>

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs::Graphics {

static uint32_t IntLog2(uint32_t i)
{
#if LUMEN_COMPILER == LUMEN_COMPILER_CLANG || LUMEN_PLATFORM != LUMEN_PLATFORM_WINDOWS
	return 31 - __builtin_clz(i | 1u);
#else
	unsigned long temp;
	_BitScanReverse(&temp, i | 1u);
	return temp;
#endif
}

static bool IsPowerOfTwo(uint32_t x)
{
	return (x != 0) && ((x & (x - 1)) == 0);
}

struct Uint128
{
	uint64_t n[2];
};

struct Uint256
{
	Uint128 n[2];
};

class Tiler
{
public:
	Tiler(): m_job1(nullptr), m_job2(nullptr) /*, m_job3(nullptr), m_job4(nullptr)*/
	{
		EXIT_NOT_IMPLEMENTED(!Core::Thread::IsMainThread());
	}
	virtual ~Tiler() { LUMEN_NOT_IMPLEMENTED; }

	LUMEN_CLASS_NO_COPY(Tiler);

	Core::Mutex m_mutex;

	AsyncJob m_job1;
	AsyncJob m_job2;
	// AsyncJob m_job3;
	// AsyncJob m_job4;
};

class Tiler32
{
public:
	uint32_t m_macro_tile_height = 0;
	uint32_t m_bank_height       = 0;
	uint32_t m_num_banks         = 0;
	uint32_t m_num_pipes         = 0;
	uint32_t m_padded_width      = 0;
	uint32_t m_padded_height     = 0;
	uint32_t m_pipe_bits         = 0;
	uint32_t m_bank_bits         = 0;

	void Init(uint32_t width, uint32_t height, bool neo)
	{
		m_macro_tile_height = (neo ? 128 : 64);
		m_bank_height       = neo ? 2 : 1;
		m_num_banks         = neo ? 8 : 16;
		m_num_pipes         = neo ? 16 : 8;
		m_padded_width      = width;
		if (height == 1080)
		{
			m_padded_height = neo ? 1152 : 1088;
		}
		if (height == 720)
		{
			m_padded_height = 768;
		}
		m_pipe_bits = neo ? 4 : 3;
		m_bank_bits = neo ? 3 : 4;
	}

	static uint32_t GetElementIndex(uint32_t x, uint32_t y)
	{
		uint32_t elem = 0;
		elem |= ((x >> 0u) & 0x1u) << 0u;
		elem |= ((x >> 1u) & 0x1u) << 1u;
		elem |= ((y >> 0u) & 0x1u) << 2u;
		elem |= ((x >> 2u) & 0x1u) << 3u;
		elem |= ((y >> 1u) & 0x1u) << 4u;
		elem |= ((y >> 2u) & 0x1u) << 5u;

		return elem;
	}

	static uint32_t GetPipeIndex(uint32_t x, uint32_t y, bool neo)
	{
		uint32_t pipe = 0;

		if (!neo)
		{
			pipe |= (((x >> 3u) ^ (y >> 3u) ^ (x >> 4u)) & 0x1u) << 0u;
			pipe |= (((x >> 4u) ^ (y >> 4u)) & 0x1u) << 1u;
			pipe |= (((x >> 5u) ^ (y >> 5u)) & 0x1u) << 2u;
		} else
		{
			pipe |= (((x >> 3u) ^ (y >> 3u) ^ (x >> 4u)) & 0x1u) << 0u;
			pipe |= (((x >> 4u) ^ (y >> 4u)) & 0x1u) << 1u;
			pipe |= (((x >> 5u) ^ (y >> 5u)) & 0x1u) << 2u;
			pipe |= (((x >> 6u) ^ (y >> 5u)) & 0x1u) << 3u;
		}

		return pipe;
	}

	static uint32_t GetBankIndex(uint32_t x, uint32_t y, uint32_t bank_width, uint32_t bank_height, uint32_t num_banks, uint32_t num_pipes)
	{
		const uint32_t x_shift_offset = IntLog2(bank_width * num_pipes);
		const uint32_t y_shift_offset = IntLog2(bank_height);
		const uint32_t xs             = x >> x_shift_offset;
		const uint32_t ys             = y >> y_shift_offset;
		uint32_t       bank           = 0;
		switch (num_banks)
		{
			case 8:
				bank |= (((xs >> 3u) ^ (ys >> 5u)) & 0x1u) << 0u;
				bank |= (((xs >> 4u) ^ (ys >> 4u) ^ (ys >> 5u)) & 0x1u) << 1u;
				bank |= (((xs >> 5u) ^ (ys >> 3u)) & 0x1u) << 2u;
				break;
			case 16:
				bank |= (((xs >> 3u) ^ (ys >> 6u)) & 0x1u) << 0u;
				bank |= (((xs >> 4u) ^ (ys >> 5u) ^ (ys >> 6u)) & 0x1u) << 1u;
				bank |= (((xs >> 5u) ^ (ys >> 4u)) & 0x1u) << 2u;
				bank |= (((xs >> 6u) ^ (ys >> 3u)) & 0x1u) << 3u;
				break;
			default:;
		}

		return bank;
	}

	[[nodiscard]] uint64_t GetTiledOffset(uint32_t x, uint32_t y, bool neo) const
	{
		uint64_t element_index = GetElementIndex(x, y);

		uint32_t xh               = x;
		uint32_t yh               = y;
		uint64_t pipe             = GetPipeIndex(xh, yh, neo);
		uint64_t bank             = GetBankIndex(xh, yh, 1, m_bank_height, m_num_banks, m_num_pipes);
		uint32_t tile_bytes       = (8 * 8 * 32 + 7) / 8;
		uint64_t element_offset   = (element_index * 32);
		uint64_t tile_split_slice = 0;

		if (tile_bytes > 512)
		{
			tile_split_slice = element_offset / (static_cast<uint64_t>(512) * 8);
			element_offset %= (static_cast<uint64_t>(512) * 8);
			tile_bytes = 512;
		}

		uint64_t macro_tile_bytes        = (128 / 8) * (m_macro_tile_height / 8) * tile_bytes / (m_num_pipes * m_num_banks);
		uint64_t macro_tiles_per_row     = m_padded_width / 128;
		uint64_t macro_tile_row_index    = y / m_macro_tile_height;
		uint64_t macro_tile_column_index = x / 128;
		uint64_t macro_tile_index        = (macro_tile_row_index * macro_tiles_per_row) + macro_tile_column_index;
		uint64_t macro_tile_offset       = macro_tile_index * macro_tile_bytes;
		uint64_t macro_tiles_per_slice   = macro_tiles_per_row * (m_padded_height / m_macro_tile_height);
		uint64_t slice_bytes             = macro_tiles_per_slice * macro_tile_bytes;
		uint64_t slice_offset            = tile_split_slice * slice_bytes;
		uint64_t tile_row_index          = (y / 8) % m_bank_height;
		uint64_t tile_index              = tile_row_index;
		uint64_t tile_offset             = tile_index * tile_bytes;

		uint64_t tile_split_slice_rotation = ((m_num_banks / 2) + 1) * tile_split_slice;
		bank ^= tile_split_slice_rotation;
		bank &= (m_num_banks - 1);

		uint64_t total_offset = (slice_offset + macro_tile_offset + tile_offset) * 8 + element_offset;
		uint64_t bit_offset   = total_offset & 0x7u;
		total_offset /= 8;

		uint64_t pipe_interleave_offset = total_offset & 0xffu;
		uint64_t offset                 = total_offset >> 8u;
		uint64_t byte_offset =
		    pipe_interleave_offset | (pipe << (8u)) | (bank << (8u + m_pipe_bits)) | (offset << (8u + m_pipe_bits + m_bank_bits));

		return ((byte_offset << 3u) | bit_offset) / 8;
	}
};

// Tiler2d: generalized 2D macro-tiled address calculator for tile mode 14 (ARRAY_2D_TILED_THIN1, THIN)
// Uses NON-DISPLAYABLE micro-tile element ordering: x0,x1,x2,y1,y0,y2 (same for all bpp)
// From AMD addrlib ComputePixelIndexWithinMicroTile (ADDR_NON_DISPLAYABLE case)
// Note: tile mode 13 is actually 1D tiled (ARRAY_1D_TILED_THIN1), handled by Tiler1d
class Tiler2d
{
public:
	uint32_t m_width             = 0;
	uint32_t m_height            = 0;
	uint32_t m_pitch             = 0;
	uint32_t m_bits_per_element  = 0;
	uint32_t m_macro_tile_width  = 0;
	uint32_t m_macro_tile_height = 0;
	uint32_t m_bank_height       = 0;
	uint32_t m_num_banks         = 0;
	uint32_t m_num_pipes         = 0;
	uint32_t m_padded_width      = 0;
	uint32_t m_padded_height     = 0;
	uint32_t m_pipe_bits         = 0;
	uint32_t m_bank_bits         = 0;
	uint32_t m_tile_split        = 0;

	void Init(uint32_t dfmt, uint32_t nfmt, uint32_t width, uint32_t height, uint32_t pitch, bool neo)
	{
		m_width  = width;
		m_height = height;
		m_pitch  = pitch;

		if ((nfmt == 0 && dfmt == 1))
		{
			m_bits_per_element = 8;
		} else if ((nfmt == 0 && dfmt == 3))
		{
			m_bits_per_element = 16;
		} else if ((nfmt == 9 && dfmt == 10) || (nfmt == 0 && dfmt == 10))
		{
			m_bits_per_element = 32;
		} else if ((nfmt == 9 && dfmt == 37) || (nfmt == 0 && dfmt == 37) || (nfmt == 0 && dfmt == 36))
		{
			// BC2 or BC3
			m_bits_per_element = 128;
			m_width            = std::max((m_width + 3) / 4, 1U);
			m_height           = std::max((m_height + 3) / 4, 1U);
			m_pitch            = std::max((m_pitch + 3) / 4, 1U);
		} else if ((nfmt == 0 && dfmt == 35))
		{
			// BC1
			m_bits_per_element = 64;
			m_width            = std::max((m_width + 3) / 4, 1U);
			m_height           = std::max((m_height + 3) / 4, 1U);
			m_pitch            = std::max((m_pitch + 3) / 4, 1U);
		} else
		{
			EXIT("Tiler2d: unknown format: nfmt = %u, dfmt = %u\n", nfmt, dfmt);
		}

		// Tile mode 13 (2D_TILED_THIN1, NON_DISPLAYABLE) parameters
		// Verified: same macro-tile geometry as Tiler32 (tile mode 10).
		// For 32bpp: max tiled offset for 512x768 = 1572860+4 = 1572864 = exact table match.
		// tile_split=2048 (vs 512 for mode 10) but no split for bpp<=128 since tile_bytes<=1024<2048.
		m_num_pipes         = neo ? 16 : 8;
		m_num_banks         = neo ? 8 : 16;
		m_pipe_bits         = neo ? 4 : 3;
		m_bank_bits         = neo ? 3 : 4;
		m_bank_height       = neo ? 2 : 1;
		m_tile_split        = 2048;
		m_macro_tile_width  = 128;
		m_macro_tile_height = neo ? 128 : 64;

		m_padded_width  = ((m_pitch + m_macro_tile_width - 1) / m_macro_tile_width) * m_macro_tile_width;
		m_padded_height = ((m_height + m_macro_tile_height - 1) / m_macro_tile_height) * m_macro_tile_height;
	}

	// Non-displayable micro-tile element index (from AMD addrlib ComputePixelIndexWithinMicroTile)
	// ADDR_NON_DISPLAYABLE: same pattern for all bpp: x0, x1, x2, y1, y0, y2
	[[nodiscard]] static uint32_t GetElementIndex(uint32_t x, uint32_t y)
	{
		uint32_t elem = 0;
		elem |= ((x >> 0u) & 0x1u) << 0u;
		elem |= ((x >> 1u) & 0x1u) << 1u;
		elem |= ((x >> 2u) & 0x1u) << 2u;
		elem |= ((y >> 1u) & 0x1u) << 3u;
		elem |= ((y >> 0u) & 0x1u) << 4u;
		elem |= ((y >> 2u) & 0x1u) << 5u;
		return elem;
	}

	static uint32_t GetPipeIndex(uint32_t x, uint32_t y, bool neo)
	{
		uint32_t pipe = 0;
		if (!neo)
		{
			pipe |= (((x >> 3u) ^ (y >> 3u) ^ (x >> 4u)) & 0x1u) << 0u;
			pipe |= (((x >> 4u) ^ (y >> 4u)) & 0x1u) << 1u;
			pipe |= (((x >> 5u) ^ (y >> 5u)) & 0x1u) << 2u;
		} else
		{
			pipe |= (((x >> 3u) ^ (y >> 3u) ^ (x >> 4u)) & 0x1u) << 0u;
			pipe |= (((x >> 4u) ^ (y >> 4u)) & 0x1u) << 1u;
			pipe |= (((x >> 5u) ^ (y >> 5u)) & 0x1u) << 2u;
			pipe |= (((x >> 6u) ^ (y >> 5u)) & 0x1u) << 3u;
		}
		return pipe;
	}

	static uint32_t GetBankIndex(uint32_t x, uint32_t y, uint32_t bank_width, uint32_t bank_height, uint32_t num_banks, uint32_t num_pipes)
	{
		const uint32_t x_shift_offset = IntLog2(bank_width * num_pipes);
		const uint32_t y_shift_offset = IntLog2(bank_height);
		const uint32_t xs             = x >> x_shift_offset;
		const uint32_t ys             = y >> y_shift_offset;
		uint32_t       bank           = 0;
		switch (num_banks)
		{
			case 8:
				bank |= (((xs >> 3u) ^ (ys >> 5u)) & 0x1u) << 0u;
				bank |= (((xs >> 4u) ^ (ys >> 4u) ^ (ys >> 5u)) & 0x1u) << 1u;
				bank |= (((xs >> 5u) ^ (ys >> 3u)) & 0x1u) << 2u;
				break;
			case 16:
				bank |= (((xs >> 3u) ^ (ys >> 6u)) & 0x1u) << 0u;
				bank |= (((xs >> 4u) ^ (ys >> 5u) ^ (ys >> 6u)) & 0x1u) << 1u;
				bank |= (((xs >> 5u) ^ (ys >> 4u)) & 0x1u) << 2u;
				bank |= (((xs >> 6u) ^ (ys >> 3u)) & 0x1u) << 3u;
				break;
			default:;
		}
		return bank;
	}

	[[nodiscard]] uint64_t GetTiledOffset(uint32_t x, uint32_t y, bool neo) const
	{
		uint64_t element_index = GetElementIndex(x, y);

		uint64_t pipe             = GetPipeIndex(x, y, neo);
		uint64_t bank             = GetBankIndex(x, y, 1, m_bank_height, m_num_banks, m_num_pipes);
		uint32_t tile_bytes       = (8 * 8 * m_bits_per_element + 7) / 8;
		uint64_t element_offset   = (element_index * m_bits_per_element);
		uint64_t tile_split_slice = 0;

		if (tile_bytes > m_tile_split)
		{
			tile_split_slice = element_offset / (static_cast<uint64_t>(m_tile_split) * 8);
			element_offset %= (static_cast<uint64_t>(m_tile_split) * 8);
			tile_bytes = m_tile_split;
		}

		uint64_t macro_tile_bytes =
		    (static_cast<uint64_t>(m_macro_tile_width) / 8) * (m_macro_tile_height / 8) * tile_bytes / (m_num_pipes * m_num_banks);
		uint64_t macro_tiles_per_row     = m_padded_width / m_macro_tile_width;
		uint64_t macro_tile_row_index    = y / m_macro_tile_height;
		uint64_t macro_tile_column_index = x / m_macro_tile_width;
		uint64_t macro_tile_index        = (macro_tile_row_index * macro_tiles_per_row) + macro_tile_column_index;
		uint64_t macro_tile_offset       = macro_tile_index * macro_tile_bytes;
		uint64_t macro_tiles_per_slice   = macro_tiles_per_row * (m_padded_height / m_macro_tile_height);
		uint64_t slice_bytes             = macro_tiles_per_slice * macro_tile_bytes;
		uint64_t slice_offset            = tile_split_slice * slice_bytes;
		uint64_t tile_row_index          = (y / 8) % m_bank_height;
		uint64_t tile_index              = tile_row_index;
		uint64_t tile_offset             = tile_index * tile_bytes;

		uint64_t tile_split_slice_rotation = ((m_num_banks / 2) + 1) * tile_split_slice;
		bank ^= tile_split_slice_rotation;
		bank &= (m_num_banks - 1);

		uint64_t total_offset = (slice_offset + macro_tile_offset + tile_offset) * 8 + element_offset;
		uint64_t bit_offset   = total_offset & 0x7u;
		total_offset /= 8;

		uint64_t pipe_interleave_offset = total_offset & 0xffu;
		uint64_t offset                 = total_offset >> 8u;
		uint64_t byte_offset =
		    pipe_interleave_offset | (pipe << (8u)) | (bank << (8u + m_pipe_bits)) | (offset << (8u + m_pipe_bits + m_bank_bits));

		return ((byte_offset << 3u) | bit_offset) / 8;
	}
};

class Tiler1d
{
public:
	uint32_t m_width            = 0;
	uint32_t m_height           = 0;
	uint32_t m_pitch            = 0;
	uint32_t m_bits_per_element = 0;
	uint32_t m_tile_bytes       = 0;
	uint32_t m_tiles_per_row    = 0;
	bool     m_displayable      = true;

	void Init(uint32_t dfmt, uint32_t nfmt, uint32_t width, uint32_t height, uint32_t pitch, uint32_t padded_width,
	          uint32_t /*padded_height*/, bool /*neo*/, bool displayable = true)
	{
		m_width  = width;
		m_height = height;
		m_pitch  = pitch;

		if ((nfmt == 0 && dfmt == 1))
		{
			// R8
			m_bits_per_element = 8;
		} else if ((nfmt == 9 && dfmt == 10) || (nfmt == 0 && dfmt == 10))
		{
			// R8G8B8A8
			m_bits_per_element = 32;
		} else if ((nfmt == 0 && dfmt == 3))
		{
			// R16
			m_bits_per_element = 16;
		} else if ((nfmt == 9 && dfmt == 37) || (nfmt == 0 && dfmt == 37) || (nfmt == 0 && dfmt == 36) || (nfmt == 0 && dfmt == 41))
		{
			// BC2, BC3 or BC7 opaque 128-bit blocks
			m_bits_per_element = 128;
			m_width            = std::max((m_width + 3) / 4, 1U);
			m_height           = std::max((m_height + 3) / 4, 1U);
			m_pitch            = std::max((m_pitch + 3) / 4, 1U);
		} else if ((nfmt == 0 && dfmt == 35))
		{
			// BC1
			m_bits_per_element = 64;
			m_width            = std::max((m_width + 3) / 4, 1U);
			m_height           = std::max((m_height + 3) / 4, 1U);
			m_pitch            = std::max((m_pitch + 3) / 4, 1U);
		} else
		{
			EXIT("unknown format: nfmt = %u, dfmt = %u\n", nfmt, dfmt);
		}

		m_tile_bytes    = (8 * 8 * 1 * m_bits_per_element + 7) / 8;
		m_tiles_per_row = padded_width / 8;
		m_displayable   = displayable;
	}

	// Micro-tile element index (from AMD GCN addrlib):
	// THIN / Morton Z-order (tile mode 13): x0,y0,x1,y1,x2,y2 (same for all bpp)
	// DISPLAYABLE (tile mode 10, 2D macro): per-bpp patterns (handled by Tiler32)
	[[nodiscard]] uint32_t GetElementIndex(uint32_t x, uint32_t y) const
	{
		uint32_t elem = 0;
		if (m_displayable)
		{
			// DISPLAYABLE: x0, y0, x1, y1, x2, y2 (same for all bpp)
			elem |= ((x >> 0u) & 0x1u) << 0u;
			elem |= ((y >> 0u) & 0x1u) << 1u;
			elem |= ((x >> 1u) & 0x1u) << 2u;
			elem |= ((y >> 1u) & 0x1u) << 3u;
			elem |= ((x >> 2u) & 0x1u) << 4u;
			elem |= ((y >> 2u) & 0x1u) << 5u;
		} else
		{
			// NON_DISPLAYABLE (ADDR_SURF_THIN_MICRO_TILING): same pattern for all bpp
			// From AMD addrlib ComputePixelIndexWithinMicroTile: x0, x1, x2, y1, y0, y2
			elem |= ((x >> 0u) & 0x1u) << 0u;
			elem |= ((x >> 1u) & 0x1u) << 1u;
			elem |= ((x >> 2u) & 0x1u) << 2u;
			elem |= ((y >> 1u) & 0x1u) << 3u;
			elem |= ((y >> 0u) & 0x1u) << 4u;
			elem |= ((y >> 2u) & 0x1u) << 5u;
		}
		return elem;
	}

	[[nodiscard]] uint64_t GetTiledOffset(uint32_t x, uint32_t y, bool /*neo*/) const
	{
		uint64_t element_index = GetElementIndex(x & 7u, y & 7u);

		uint64_t tile_row_index    = y / 8;
		uint64_t tile_column_index = x / 8;
		uint64_t tile_offset       = ((tile_row_index * m_tiles_per_row) + tile_column_index) * m_tile_bytes;
		uint64_t element_offset    = element_index * m_bits_per_element;
		uint64_t offset            = tile_offset * 8 + element_offset;
		return offset / 8;
	}
};

static Tiler* g_tiler = nullptr;

static void init_maps();

void TileInit()
{
	EXIT_IF(g_tiler != nullptr);

	g_tiler = new Tiler;

	init_maps();
}

// NOLINTNEXTLINE(readability-non-const-parameter)
static void Detile32(const Tiler32* t, uint32_t width, uint32_t height, uint32_t dst_pitch, uint8_t* dst, const uint8_t* src, bool neo)
{
	EXIT_IF(g_tiler == nullptr);

	Core::LockGuard lock(g_tiler->m_mutex);

	struct DetileParams
	{
		const Tiler32* t;
		uint32_t       start_y;
		uint32_t       width;
		uint32_t       height;
		uint32_t       dst_pitch;
		uint8_t*       dst;
		const uint8_t* src;
		bool           neo;
	};

	auto func = [](void* args)
	{
		auto* p = static_cast<DetileParams*>(args);

		auto*          dst       = p->dst;
		const auto*    src       = p->src;
		const Tiler32* t         = p->t;
		uint32_t       start_y   = p->start_y;
		uint32_t       width     = p->width;
		uint32_t       height    = p->height;
		uint64_t       dst_pitch = p->dst_pitch;
		bool           neo       = p->neo;

		for (uint32_t y = start_y; y < height; y++)
		{
			uint32_t x             = 0;
			uint64_t linear_offset = y * dst_pitch * 4;

			for (; x + 1 < width; x += 2)
			{
				auto tiled_offset = t->GetTiledOffset(x, y, neo);

				*reinterpret_cast<uint64_t*>(dst + linear_offset) = *reinterpret_cast<const uint64_t*>(src + tiled_offset);
				linear_offset += 8;
			}
			if (x < width)
			{
				auto tiled_offset = t->GetTiledOffset(x, y, neo);

				*reinterpret_cast<uint32_t*>(dst + linear_offset) = *reinterpret_cast<const uint32_t*>(src + tiled_offset);
			}
		}
	};

	DetileParams p1 {t, 0, width, height / 4, dst_pitch, dst, src, neo};
	DetileParams p2 {t, p1.height, width, /*(height * 2) / 4*/ height, dst_pitch, dst, src, neo};
	// DetileParams p3 {t, p2.height, width, (height * 3) / 4, dst_pitch, dst, src, neo};
	// DetileParams p4 {t, p3.height, width, height, dst_pitch, dst, src, neo};

	g_tiler->m_job1.Execute(func, &p1);
	g_tiler->m_job2.Execute(func, &p2);
	// g_tiler->m_job3.Execute(func, &p3);
	// g_tiler->m_job4.Execute(func, &p4);

	g_tiler->m_job1.Wait();
	g_tiler->m_job2.Wait();
	// g_tiler->m_job3.Wait();
	// g_tiler->m_job4.Wait();

	//	Core::Thread t1(func, &p1);
	//	Core::Thread t2(func, &p2);
	//	Core::Thread t3(func, &p3);
	//	Core::Thread t4(func, &p4);
	//
	//	t1.Join();
	//	t2.Join();
	//	t3.Join();
	//	t4.Join();
}

static void Detile32(const Tiler1d* t, uint32_t width, uint32_t height, uint32_t dst_pitch, uint8_t* dst, const uint8_t* src, bool neo)
{
	for (uint32_t y = 0; y < height; y++)
	{
		uint32_t x             = 0;
		uint64_t linear_offset = y * static_cast<uint64_t>(dst_pitch) * 4;

		for (; x + 1 < width; x += 2)
		{
			auto tiled_offset = t->GetTiledOffset(x, y, neo);

			*reinterpret_cast<uint64_t*>(dst + linear_offset) = *reinterpret_cast<const uint64_t*>(src + tiled_offset);
			linear_offset += 8;
		}
		if (x < width)
		{
			auto tiled_offset = t->GetTiledOffset(x, y, neo);

			*reinterpret_cast<uint32_t*>(dst + linear_offset) = *reinterpret_cast<const uint32_t*>(src + tiled_offset);
		}
	}
}

static void Detile64(const Tiler1d* t, uint32_t width, uint32_t height, uint32_t dst_pitch, uint8_t* dst, const uint8_t* src, bool neo)
{
	for (uint32_t y = 0; y < height; y++)
	{
		uint32_t x             = 0;
		uint64_t linear_offset = y * static_cast<uint64_t>(dst_pitch) * 8;

		for (; x + 1 < width; x += 2)
		{
			auto tiled_offset = t->GetTiledOffset(x, y, neo);

			*reinterpret_cast<Uint128*>(dst + linear_offset) = *reinterpret_cast<const Uint128*>(src + tiled_offset);
			linear_offset += 16;
		}
		if (x < width)
		{
			auto tiled_offset = t->GetTiledOffset(x, y, neo);

			*reinterpret_cast<uint64_t*>(dst + linear_offset) = *reinterpret_cast<const uint64_t*>(src + tiled_offset);
		}
	}
}

static void Detile128(const Tiler1d* t, uint32_t width, uint32_t height, uint32_t dst_pitch, uint8_t* dst, const uint8_t* src, bool neo)
{
	for (uint32_t y = 0; y < height; y++)
	{
		uint64_t linear_offset = y * static_cast<uint64_t>(dst_pitch) * 16;

		for (uint32_t x = 0; x < width; x++)
		{
			auto tiled_offset = t->GetTiledOffset(x, y, neo);

			*reinterpret_cast<Uint128*>(dst + linear_offset) = *reinterpret_cast<const Uint128*>(src + tiled_offset);
			linear_offset += 16;
		}
	}
}

static void Detile8(const Tiler1d* t, uint32_t width, uint32_t height, uint32_t dst_pitch, uint8_t* dst, const uint8_t* src, bool neo)
{
	for (uint32_t y = 0; y < height; y++)
	{
		uint64_t linear_offset = y * static_cast<uint64_t>(dst_pitch);

		for (uint32_t x = 0; x < width; x++)
		{
			auto tiled_offset = t->GetTiledOffset(x, y, neo);

			dst[linear_offset] = src[tiled_offset];
			linear_offset++;
		}
	}
}

static void Detile16(const Tiler1d* t, uint32_t width, uint32_t height, uint32_t dst_pitch, uint8_t* dst, const uint8_t* src, bool neo)
{
	for (uint32_t y = 0; y < height; y++)
	{
		uint64_t linear_offset = y * static_cast<uint64_t>(dst_pitch) * 2;

		for (uint32_t x = 0; x < width; x++)
		{
			auto tiled_offset = t->GetTiledOffset(x, y, neo);

			*reinterpret_cast<uint16_t*>(dst + linear_offset) = *reinterpret_cast<const uint16_t*>(src + tiled_offset);
			linear_offset += 2;
		}
	}
}

static void Detile1d(const Tiler1d* t, uint8_t* dst, const uint8_t* src, bool neo)
{
	if (t->m_bits_per_element == 8)
	{
		Detile8(t, t->m_width, t->m_height, t->m_pitch, dst, src, neo);
	} else if (t->m_bits_per_element == 16)
	{
		Detile16(t, t->m_width, t->m_height, t->m_pitch, dst, src, neo);
	} else if (t->m_bits_per_element == 32)
	{
		Detile32(t, t->m_width, t->m_height, t->m_pitch, dst, src, neo);
	} else if (t->m_bits_per_element == 64)
	{
		Detile64(t, t->m_width, t->m_height, t->m_pitch, dst, src, neo);
	} else if (t->m_bits_per_element == 128)
	{
		Detile128(t, t->m_width, t->m_height, t->m_pitch, dst, src, neo);
	} else
	{
		EXIT("Unknown size");
	}
}

static bool Retile1dInBounds(const Tiler1d& tiler, bool neo, uint64_t level_size)
{
	const uint64_t element_bytes = tiler.m_bits_per_element / 8u;
	if (element_bytes == 0 || tiler.m_bits_per_element % 8u != 0 || tiler.m_width == 0 || tiler.m_height == 0 ||
	    tiler.m_pitch < tiler.m_width || level_size < element_bytes)
	{
		return false;
	}

	for (uint32_t y = 0; y < tiler.m_height; ++y)
	{
		const uint64_t linear_offset = static_cast<uint64_t>(y) * tiler.m_pitch * element_bytes;
		const uint64_t row_size      = static_cast<uint64_t>(tiler.m_width) * element_bytes;
		if (linear_offset > level_size || row_size > level_size - linear_offset)
		{
			return false;
		}

		for (uint32_t x = 0; x < tiler.m_width; ++x)
		{
			const uint64_t tiled_offset = tiler.GetTiledOffset(x, y, neo);
			if (tiled_offset > level_size || element_bytes > level_size - tiled_offset)
			{
				return false;
			}
		}
	}
	return true;
}

static void Retile1d(const Tiler1d& tiler, uint8_t* dst, const uint8_t* src, bool neo)
{
	const uint64_t element_bytes = tiler.m_bits_per_element / 8u;
	for (uint32_t y = 0; y < tiler.m_height; ++y)
	{
		uint64_t linear_offset = static_cast<uint64_t>(y) * tiler.m_pitch * element_bytes;
		for (uint32_t x = 0; x < tiler.m_width; ++x)
		{
			const uint64_t tiled_offset = tiler.GetTiledOffset(x, y, neo);
			std::memcpy(dst + tiled_offset, src + linear_offset, element_bytes);
			linear_offset += element_bytes;
		}
	}
}

template <bool Neo>
static uint64_t Thin2d32BlockOffset(uint32_t x, uint32_t y, uint32_t pitch)
{
	// At an aligned 8x8 origin GetElementIndex is zero and a 32-bit display
	// tile never splits. Reduce Tiler32::GetTiledOffset to its pipe/bank and
	// macro-tile terms, with constant divisors for each PS4 layout.
	constexpr uint32_t macro_height = Neo ? 128u : 64u;
	constexpr uint32_t bank_height = Neo ? 2u : 1u;
	constexpr uint32_t num_pipes = Neo ? 16u : 8u;
	constexpr uint32_t num_banks = Neo ? 8u : 16u;
	const uint64_t macro_index = static_cast<uint64_t>(y / macro_height) * (pitch / 128u) + x / 128u;
	const uint64_t block_index = macro_index * bank_height + (y / 8u) % bank_height;
	const uint64_t pipe = Tiler32::GetPipeIndex(x, y, Neo);
	const uint64_t bank = Tiler32::GetBankIndex(x, y, 1, bank_height, num_banks, num_pipes);
	return (block_index << 15u) | (bank << (Neo ? 12u : 11u)) | (pipe << 8u);
}

template <bool ToTiled>
static void CopyDisplay2dThinBlock(uint8_t* dst, const uint8_t* src, uint64_t tiled_offset,
                                  uint64_t linear_offset, uint64_t row_bytes)
{
	// x0/x1 are the lowest element bits. Each four-texel half-row is therefore
	// contiguous; each pair of rows occupies [row0-left,row1-left,row0-right,
	// row1-right]. Fixed 16-byte memcpy also supports unaligned caller buffers.
	for (uint32_t pair = 0; pair < 4; ++pair)
	{
		if constexpr (ToTiled)
		{
			std::memcpy(dst + tiled_offset, src + linear_offset, 16);
			std::memcpy(dst + tiled_offset + 32u, src + linear_offset + 16u, 16);
			std::memcpy(dst + tiled_offset + 16u, src + linear_offset + row_bytes, 16);
			std::memcpy(dst + tiled_offset + 48u, src + linear_offset + row_bytes + 16u, 16);
		} else
		{
			std::memcpy(dst + linear_offset, src + tiled_offset, 16);
			std::memcpy(dst + linear_offset + 16u, src + tiled_offset + 32u, 16);
			std::memcpy(dst + linear_offset + row_bytes, src + tiled_offset + 16u, 16);
			std::memcpy(dst + linear_offset + row_bytes + 16u, src + tiled_offset + 48u, 16);
		}
		tiled_offset += 64u;
		linear_offset += row_bytes * 2u;
	}
}

struct Display2dThinRows
{
	uint8_t* destination;
	const uint8_t* source;
	uint32_t width;
	uint32_t pitch;
	uint32_t begin_y;
	uint32_t end_y;
};

template <bool Neo, bool ToTiled>
static void ConvertDisplay2dThinRows(void* argument)
{
	const auto& rows = *static_cast<const Display2dThinRows*>(argument);
	auto* destination = rows.destination;
	const auto* source = rows.source;
	const uint32_t width = rows.width;
	const uint32_t pitch = rows.pitch;
	const uint32_t end_y = rows.end_y;
	const uint64_t row_bytes = static_cast<uint64_t>(rows.pitch) * 4u;
	for (uint64_t tile_y = rows.begin_y; tile_y < end_y; tile_y += 8u)
	{
		const uint32_t active_height = static_cast<uint32_t>(std::min<uint64_t>(8u, end_y - tile_y));
		for (uint64_t tile_x = 0; tile_x < width; tile_x += 8u)
		{
			const uint32_t active_width = static_cast<uint32_t>(std::min<uint64_t>(8u, width - tile_x));
			const uint64_t tile_base = Thin2d32BlockOffset<Neo>(static_cast<uint32_t>(tile_x),
			                                                        static_cast<uint32_t>(tile_y), pitch);
			if (active_width == 8u && active_height == 8u)
			{
				CopyDisplay2dThinBlock<ToTiled>(destination, source, tile_base, tile_y * row_bytes + tile_x * 4u, row_bytes);
				continue;
			}
			for (uint32_t local_y = 0; local_y < active_height; ++local_y)
			{
				uint64_t linear_offset = (tile_y + local_y) * row_bytes + tile_x * 4u;
				for (uint32_t local_x = 0; local_x < active_width; ++local_x, linear_offset += 4u)
				{
					const uint64_t tiled_offset = tile_base + Tiler32::GetElementIndex(local_x, local_y) * 4u;
					std::memcpy(destination + (ToTiled ? tiled_offset : linear_offset),
					            source + (ToTiled ? linear_offset : tiled_offset), 4u);
				}
			}
		}
	}
}

static bool Display2dThinParallelEnabled()
{
	static const bool enabled = [] {
		const char* value = std::getenv("LUMEN_EXPERIMENTAL_GFX_TILE_PARALLEL");
		return value == nullptr || std::strcmp(value, "1") == 0;
	}();
	return enabled;
}

template <bool Neo, bool ToTiled>
static bool ConvertDisplay2dThinImpl(void* dst, uint64_t dst_size, const void* src, uint64_t src_size,
                                     uint32_t width, uint32_t height, uint32_t pitch)
{
	TileSizeAlign layout;
	if (dst == nullptr || src == nullptr ||
	    !TileTryGetDisplay2dThinTextureSize(10, 0, width, height, pitch, 1, 10, Neo, false, &layout, nullptr, nullptr))
	{
		return false;
	}
	const uint64_t row_bytes = static_cast<uint64_t>(pitch) * 4u;
	const uint64_t linear_span = static_cast<uint64_t>(height - 1u) * row_bytes + static_cast<uint64_t>(width) * 4u;
	const uint64_t required_src = ToTiled ? linear_span : layout.size;
	const uint64_t required_dst = ToTiled ? layout.size : linear_span;
	const auto source_begin = reinterpret_cast<uintptr_t>(src);
	const auto destination_begin = reinterpret_cast<uintptr_t>(dst);
	if (src_size < required_src || dst_size < required_dst || src_size > UINTPTR_MAX - source_begin ||
	    dst_size > UINTPTR_MAX - destination_begin)
	{
		return false;
	}
	const auto source_end = source_begin + src_size;
	const auto destination_end = destination_begin + dst_size;
	if (source_begin < destination_end && destination_begin < source_end) return false;

	// Prove the bounds of every 8x8 micro-tile before touching dst, without
	// visiting them twice. In Thin2d32BlockOffset the pipe/bank bits are
	// confined to bits 8..14; their contribution is at most 32768 - 256.
	// A macro-tile contains one such 32 KiB group in base mode, two in Neo.
	// Thus each micro-tile is 256-byte aligned and its end is no later than
	// the end of its macro-tile, including partially active edge blocks.
	// The macro index increases with x and y, so its maximum is at the last
	// active texel. The validated pitch/height make this arithmetic fit in
	// uint64_t, and the whole physical slice already fits in uint32_t.
	constexpr uint64_t macro_height = Neo ? 128u : 64u;
	constexpr uint64_t macro_bytes = 128u * macro_height * 4u;
	const uint64_t last_macro = (static_cast<uint64_t>(height - 1u) / macro_height) * (pitch / 128u) +
	                            (width - 1u) / 128u;
	if ((last_macro + 1u) * macro_bytes > layout.size) return false;

	Display2dThinRows rows {static_cast<uint8_t*>(dst), static_cast<const uint8_t*>(src), width, pitch, 0, height};
	const uint64_t active_bytes = static_cast<uint64_t>(width) * height * 4u;
	const uint64_t macro_rows = (static_cast<uint64_t>(height) + macro_height - 1u) / macro_height;
	if (Display2dThinParallelEnabled() && g_tiler != nullptr && active_bytes >= 64u * 1024u * 1024u &&
	    macro_rows >= 3u && g_tiler->m_mutex.TryLock())
	{
		// Share only the existing Tiler workers. A busy pool, including a call
		// from one of its workers, falls back without acquiring another lock.
		// Split at macro-row boundaries: each chunk owns disjoint macro indices
		// in tiled memory and disjoint rows in linear memory. Only the last
		// chunk can contain the partial bottom edge; all chunks preserve partial
		// right edges. All spans were validated above.
		const auto first_end = static_cast<uint32_t>((macro_rows / 3u) * macro_height);
		const auto second_end = static_cast<uint32_t>(((macro_rows * 2u) / 3u) * macro_height);
		Display2dThinRows first {rows.destination, rows.source, width, pitch, 0, first_end};
		Display2dThinRows second {rows.destination, rows.source, width, pitch, first_end, second_end};
		Display2dThinRows last {rows.destination, rows.source, width, pitch, second_end, height};
		g_tiler->m_job1.Execute(ConvertDisplay2dThinRows<Neo, ToTiled>, &first);
		g_tiler->m_job2.Execute(ConvertDisplay2dThinRows<Neo, ToTiled>, &second);
		ConvertDisplay2dThinRows<Neo, ToTiled>(&last);
		g_tiler->m_job1.Wait();
		g_tiler->m_job2.Wait();
		// Stack arguments, mapped source and destination remain owned until
		// both joins. No worker enters the dispatcher or acquires tracker locks.
		g_tiler->m_mutex.Unlock();
		static std::atomic_uint64_t completed {0};
		const auto count = completed.fetch_add(1, std::memory_order_relaxed) + 1u;
		if ((count & (count - 1u)) == 0)
			std::fprintf(stderr, "[GFX_TILE_PARALLEL] enabled=1 chunks=3 calls=%llu neo=%u direction=%s active_bytes=%llu\n",
			             static_cast<unsigned long long>(count), Neo ? 1u : 0u, ToTiled ? "retile" : "detile",
			             static_cast<unsigned long long>(active_bytes));
	} else
	{
		ConvertDisplay2dThinRows<Neo, ToTiled>(&rows);
	}
	return true;
}

static bool ConvertDisplay2dThin(void* dst, uint64_t dst_size, const void* src, uint64_t src_size,
                                 uint32_t width, uint32_t height, uint32_t pitch, bool neo, bool to_tiled)
{
	if (neo)
		return to_tiled ? ConvertDisplay2dThinImpl<true, true>(dst, dst_size, src, src_size, width, height, pitch)
		                : ConvertDisplay2dThinImpl<true, false>(dst, dst_size, src, src_size, width, height, pitch);
	return to_tiled ? ConvertDisplay2dThinImpl<false, true>(dst, dst_size, src, src_size, width, height, pitch)
	                : ConvertDisplay2dThinImpl<false, false>(dst, dst_size, src, src_size, width, height, pitch);
}

bool TileTryConvertDisplay2dThinToLinear(void* dst, uint64_t dst_size, const void* src, uint64_t src_size,
                                       uint32_t width, uint32_t height, uint32_t pitch, bool neo)
{
	return ConvertDisplay2dThin(dst, dst_size, src, src_size, width, height, pitch, neo, false);
}

bool TileTryConvertLinearToDisplay2dThin(void* dst, uint64_t dst_size, const void* src, uint64_t src_size,
                                       uint32_t width, uint32_t height, uint32_t pitch, bool neo)
{
	return ConvertDisplay2dThin(dst, dst_size, src, src_size, width, height, pitch, neo, true);
}

bool TileTryGetColor2dThinTextureSize(uint32_t dfmt, uint32_t nfmt, uint32_t width, uint32_t height, uint32_t pitch,
                                     uint32_t levels, uint32_t tile, bool neo, bool pow2_pad, TileSizeAlign* total_size,
                                     TileSizeOffset* level_sizes, TilePaddedSize* padded_size)
{
	if (dfmt != 10 || nfmt != 0 || levels != 1 || tile != 14 || pow2_pad || width == 0 || height == 0 ||
	    width > pitch || (pitch & 127u) != 0) return false;
	const uint64_t macro_height = neo ? 128u : 64u;
	const uint64_t physical_height = (static_cast<uint64_t>(height) + macro_height - 1u) & ~(macro_height - 1u);
	const uint64_t row_bytes = static_cast<uint64_t>(pitch) * 4u;
	if (physical_height == 0 || physical_height > UINT32_MAX || row_bytes == 0 || row_bytes > UINT32_MAX ||
	    physical_height > UINT32_MAX / row_bytes) return false;
	const auto size = static_cast<uint32_t>(physical_height * row_bytes);
	// This is the existing Tiler2d 32-bit slice model: no tile split, one
	// 32 KiB macro group in base mode and two in Neo. Keep it independent
	// of sampled-texture descriptor ranges and their existing alias contracts.
	if (total_size != nullptr) *total_size = {size, neo ? 65536u : 32768u};
	if (level_sizes != nullptr) level_sizes[0] = {size, 0};
	if (padded_size != nullptr) padded_size[0] = {pitch, static_cast<uint32_t>(physical_height)};
	return true;
}

template <bool Neo, bool ToTiled>
static bool ConvertColor2dThin(void* dst, uint64_t dst_size, const void* src, uint64_t src_size,
                                uint32_t width, uint32_t height, uint32_t pitch)
{
	TileSizeAlign layout {};
	if (dst == nullptr || src == nullptr ||
	    !TileTryGetColor2dThinTextureSize(10, 0, width, height, pitch, 1, 14, Neo, false, &layout, nullptr, nullptr)) return false;
	const uint64_t row_bytes = static_cast<uint64_t>(pitch) * 4u;
	const uint64_t linear_span = static_cast<uint64_t>(height - 1u) * row_bytes + static_cast<uint64_t>(width) * 4u;
	const uint64_t required_source = ToTiled ? linear_span : layout.size;
	const uint64_t required_destination = ToTiled ? layout.size : linear_span;
	const auto source = reinterpret_cast<uintptr_t>(src);
	const auto destination = reinterpret_cast<uintptr_t>(dst);
	if (src_size < required_source || dst_size < required_destination || src_size > UINTPTR_MAX - source ||
	    dst_size > UINTPTR_MAX - destination) return false;
	if (source < destination + dst_size && destination < source + src_size) return false;

	// GetElementIndex is at most 63, so each texel lies within its aligned
	// 256-byte micro-tile. Pipe/bank bits select disjoint micro-tiles within
	// a 32 KiB group; the macro index is monotone in x/y. Checking the last
	// active macro proves every complete texel is in the resolved slice before
	// the first write, including partial right/bottom tiles.
	constexpr uint64_t macro_height = Neo ? 128u : 64u;
	constexpr uint64_t macro_bytes = 128u * macro_height * 4u;
	const uint64_t last_macro = (static_cast<uint64_t>(height - 1u) / macro_height) * (pitch / 128u) +
	                            (width - 1u) / 128u;
	if ((last_macro + 1u) * macro_bytes > layout.size) return false;
	for (uint64_t tile_y = 0; tile_y < height; tile_y += 8u)
	{
		const auto active_height = static_cast<uint32_t>(std::min(uint64_t {8}, height - tile_y));
		for (uint64_t tile_x = 0; tile_x < width; tile_x += 8u)
		{
			const auto active_width = static_cast<uint32_t>(std::min(uint64_t {8}, width - tile_x));
			const uint64_t tile_base = Thin2d32BlockOffset<Neo>(static_cast<uint32_t>(tile_x),
			                                                    static_cast<uint32_t>(tile_y), pitch);
			for (uint32_t y = 0; y < active_height; ++y)
			{
				uint64_t linear_offset = (tile_y + y) * row_bytes + tile_x * 4u;
				for (uint32_t x = 0; x < active_width; ++x, linear_offset += 4u)
				{
					const uint64_t tiled_offset = tile_base + Tiler2d::GetElementIndex(x, y) * 4u;
					std::memcpy(static_cast<uint8_t*>(dst) + (ToTiled ? tiled_offset : linear_offset),
					            static_cast<const uint8_t*>(src) + (ToTiled ? linear_offset : tiled_offset), 4u);
				}
			}
		}
	}
	return true;
}

bool TileTryConvertColor2dThinToLinear(void* dst, uint64_t dst_size, const void* src, uint64_t src_size,
                                     uint32_t width, uint32_t height, uint32_t pitch, bool neo)
{
	return neo ? ConvertColor2dThin<true, false>(dst, dst_size, src, src_size, width, height, pitch)
	           : ConvertColor2dThin<false, false>(dst, dst_size, src, src_size, width, height, pitch);
}

bool TileTryConvertLinearToColor2dThin(void* dst, uint64_t dst_size, const void* src, uint64_t src_size,
                                     uint32_t width, uint32_t height, uint32_t pitch, bool neo)
{
	return neo ? ConvertColor2dThin<true, true>(dst, dst_size, src, src_size, width, height, pitch)
	           : ConvertColor2dThin<false, true>(dst, dst_size, src, src_size, width, height, pitch);
}

void TileConvertTiledToLinear(void* dst, const void* src, TileMode mode, uint32_t width, uint32_t height, bool neo)
{
	LUMEN_PROFILER_FUNCTION();

	EXIT_NOT_IMPLEMENTED(mode != TileMode::VideoOutTiled);

	Tiler32 t;
	t.Init(width, height, neo);

	Detile32(&t, width, height, width, static_cast<uint8_t*>(dst), static_cast<const uint8_t*>(src), neo);
}

[[maybe_unused]] static void Detile2dGeneric(const Tiler2d* t, uint8_t* dst, const uint8_t* src, bool neo)
{
	uint32_t elem_bytes = t->m_bits_per_element / 8;

	for (uint32_t y = 0; y < t->m_height; y++)
	{
		uint64_t linear_offset = static_cast<uint64_t>(y) * t->m_pitch * elem_bytes;

		for (uint32_t x = 0; x < t->m_width; x++)
		{
			auto tiled_offset = t->GetTiledOffset(x, y, neo);
			memcpy(dst + linear_offset, src + tiled_offset, elem_bytes);
			linear_offset += elem_bytes;
		}
	}
}

void TileConvertTiledToLinear(void* dst, const void* src, TileMode mode, uint32_t dfmt, uint32_t nfmt, uint32_t width, uint32_t height,
                              uint32_t pitch, uint32_t levels, bool neo, uint32_t tile, uint64_t buffer_size, bool pow2_pad)
{
	EXIT_NOT_IMPLEMENTED(mode != TileMode::TextureTiled);
	if (dfmt == 41)
	{
		Bc7Thin1dLayout layout {};
		EXIT_NOT_IMPLEMENTED(!ResolveBc7Thin1dLayout(dfmt, nfmt, width, height, pitch, 0, levels, tile, pow2_pad, &layout) ||
		                     dst == nullptr || src == nullptr || buffer_size < layout.bytes);
		const auto destination_begin = reinterpret_cast<uintptr_t>(dst);
		const auto source_begin = reinterpret_cast<uintptr_t>(src);
		EXIT_NOT_IMPLEMENTED(destination_begin % alignof(Uint128) != 0 || source_begin % alignof(Uint128) != 0 ||
		                     layout.bytes > UINTPTR_MAX - destination_begin || layout.bytes > UINTPTR_MAX - source_begin ||
		                     (destination_begin < source_begin + layout.bytes && source_begin < destination_begin + layout.bytes));
	}

	if (tile == 10)
	{
		EXIT_NOT_IMPLEMENTED(!TileTryGetDisplay2dThinTextureSize(dfmt, nfmt, width, height, pitch, levels, tile, neo, pow2_pad,
		                                                       nullptr, nullptr, nullptr));
		EXIT_NOT_IMPLEMENTED(!TileTryConvertDisplay2dThinToLinear(dst, buffer_size, src, buffer_size, width, height, pitch, neo));
		return;
	}

	auto*       dstptr = static_cast<uint8_t*>(dst);
	const auto* srcptr = static_cast<const uint8_t*>(src);

	TilePaddedSize padded_sizes[16] {};
	TileSizeOffset level_sizes[16] {};
	TileGetTextureSize(dfmt, nfmt, width, height, pitch, levels, tile, neo, nullptr, level_sizes, padded_sizes, pow2_pad);
	if (buffer_size != 0)
	{
		EXIT_NOT_IMPLEMENTED(levels == 0 || levels > 16);
		const uint64_t required_size = static_cast<uint64_t>(level_sizes[levels - 1].offset) + level_sizes[levels - 1].size;
		EXIT_NOT_IMPLEMENTED(required_size == 0 || required_size > buffer_size);
	}

	uint32_t mip_width  = width;
	uint32_t mip_height = height;
	uint32_t mip_pitch  = pitch;

	for (uint32_t l = 0; l < levels; l++)
	{
		// Determine padded sizes: use table values if available, otherwise compute
		uint32_t pw = padded_sizes[l].width;
		uint32_t ph = padded_sizes[l].height;
		if (pw == 0 || ph == 0)
		{
			bool     is_bc = (dfmt == 35 || dfmt == 36 || dfmt == 37 || dfmt == 41);
			uint32_t ew    = is_bc ? std::max((mip_pitch + 3) / 4, 1u) : mip_pitch;
			uint32_t eh    = is_bc ? std::max((mip_height + 3) / 4, 1u) : mip_height;
			pw             = ((ew + 7) / 8) * 8;
			ph             = ((eh + 7) / 8) * 8;
		}

		if (tile == 8)
		{
			// Tile mode 8 = DisplayLinearAligned (ArrayLinearAligned)
			// Linear layout with pitch alignment — no micro-tile reordering needed
			memcpy(dstptr + level_sizes[l].offset, srcptr + level_sizes[l].offset, level_sizes[l].size);
		} else
		{
			// Tile mode 13 = Thin1DThin (Array1DTiledThin1, THIN micro-tiling)
			// Uses Morton/Z-order element index: x0,y0,x1,y1,x2,y2
			Tiler1d t;
			t.Init(dfmt, nfmt, mip_width, mip_height, mip_pitch, pw, ph, neo, /*displayable=*/true);
			Detile1d(&t, dstptr + level_sizes[l].offset, srcptr + level_sizes[l].offset, neo);
		}

		if (mip_width > 1)
		{
			mip_width /= 2;
		}
		if (mip_height > 1)
		{
			mip_height /= 2;
		}
		if (mip_pitch > 1)
		{
			mip_pitch /= 2;
		}
	}
}

void TileConvertLinearToTiled(void* dst, const void* src, TileMode mode, uint32_t dfmt, uint32_t nfmt, uint32_t width, uint32_t height,
                              uint32_t pitch, uint32_t levels, bool neo, uint32_t tile, uint64_t buffer_size, bool pow2_pad)
{
	EXIT_IF(dst == nullptr);
	EXIT_IF(src == nullptr);
	EXIT_NOT_IMPLEMENTED(mode != TileMode::TextureTiled);
	if (dfmt == 41)
	{
		Bc7Thin1dLayout layout {};
		EXIT_NOT_IMPLEMENTED(!ResolveBc7Thin1dLayout(dfmt, nfmt, width, height, pitch, 0, levels, tile, pow2_pad, &layout) ||
		                     dst == nullptr || src == nullptr || buffer_size < layout.bytes);
		const auto destination_begin = reinterpret_cast<uintptr_t>(dst);
		const auto source_begin = reinterpret_cast<uintptr_t>(src);
		EXIT_NOT_IMPLEMENTED(destination_begin % alignof(Uint128) != 0 || source_begin % alignof(Uint128) != 0 ||
		                     layout.bytes > UINTPTR_MAX - destination_begin || layout.bytes > UINTPTR_MAX - source_begin ||
		                     (destination_begin < source_begin + layout.bytes && source_begin < destination_begin + layout.bytes));
	}

	if (tile == 10)
	{
		EXIT_NOT_IMPLEMENTED(!TileTryGetDisplay2dThinTextureSize(dfmt, nfmt, width, height, pitch, levels, tile, neo, pow2_pad,
		                                                       nullptr, nullptr, nullptr));
		EXIT_NOT_IMPLEMENTED(!TileTryConvertLinearToDisplay2dThin(dst, buffer_size, src, buffer_size, width, height, pitch, neo));
		return;
	}
	EXIT_NOT_IMPLEMENTED(tile != 8 && tile != 13);
	EXIT_NOT_IMPLEMENTED(width == 0 || height == 0 || pitch < width || levels == 0 || levels > 16 || buffer_size == 0);

	TilePaddedSize padded_sizes[16] {};
	TileSizeOffset level_sizes[16] {};
	TileGetTextureSize(dfmt, nfmt, width, height, pitch, levels, tile, neo, nullptr, level_sizes, padded_sizes, pow2_pad);

	const uint64_t required_size = static_cast<uint64_t>(level_sizes[levels - 1].offset) + level_sizes[levels - 1].size;
	EXIT_NOT_IMPLEMENTED(required_size == 0 || required_size > buffer_size);
	const auto destination_begin = reinterpret_cast<uintptr_t>(dst);
	const auto source_begin      = reinterpret_cast<uintptr_t>(src);
	EXIT_NOT_IMPLEMENTED(required_size > UINTPTR_MAX - destination_begin || required_size > UINTPTR_MAX - source_begin);
	const auto destination_end = destination_begin + required_size;
	const auto source_end      = source_begin + required_size;
	EXIT_NOT_IMPLEMENTED(destination_begin < source_end && source_begin < destination_end);

	auto*       dstptr = static_cast<uint8_t*>(dst);
	const auto* srcptr = static_cast<const uint8_t*>(src);
	Tiler1d     tilers[16];
	uint32_t    mip_width  = width;
	uint32_t    mip_height = height;
	uint32_t    mip_pitch  = pitch;

	// Validate every level before writing any destination byte. In particular,
	// an unsupported later mip cannot leave a partially re-tiled image behind.
	uint64_t previous_level_end = 0;
	for (uint32_t level = 0; level < levels; ++level)
	{
		const uint64_t level_end = static_cast<uint64_t>(level_sizes[level].offset) + level_sizes[level].size;
		EXIT_NOT_IMPLEMENTED(level_sizes[level].size == 0 || level_sizes[level].offset < previous_level_end ||
		                     level_end > required_size);
		if (tile == 13)
		{
			uint32_t padded_width  = padded_sizes[level].width;
			uint32_t padded_height = padded_sizes[level].height;
			if (padded_width == 0 || padded_height == 0)
			{
				const bool compressed = dfmt == 35 || dfmt == 36 || dfmt == 37 || dfmt == 41;
				const uint32_t element_width  = compressed ? std::max((mip_pitch + 3) / 4, 1u) : mip_pitch;
				const uint32_t element_height = compressed ? std::max((mip_height + 3) / 4, 1u) : mip_height;
				padded_width                   = ((element_width + 7) / 8) * 8;
				padded_height                  = ((element_height + 7) / 8) * 8;
			}
			EXIT_NOT_IMPLEMENTED(padded_width < 8 || padded_width % 8u != 0 || padded_height < 8 || padded_height % 8u != 0);

			tilers[level].Init(dfmt, nfmt, mip_width, mip_height, mip_pitch, padded_width, padded_height, neo,
			                   /*displayable=*/true);
			EXIT_NOT_IMPLEMENTED(!Retile1dInBounds(tilers[level], neo, level_sizes[level].size));
		}

		mip_width  = std::max(mip_width / 2u, 1u);
		mip_height = std::max(mip_height / 2u, 1u);
		mip_pitch  = std::max(mip_pitch / 2u, 1u);
		previous_level_end = level_end;
	}

	for (uint32_t level = 0; level < levels; ++level)
	{
		if (tile == 8)
		{
			std::memcpy(dstptr + level_sizes[level].offset, srcptr + level_sizes[level].offset, level_sizes[level].size);
		} else
		{
			Retile1d(tilers[level], dstptr + level_sizes[level].offset, srcptr + level_sizes[level].offset, neo);
		}
	}
}

bool TileGetDepthSize(uint32_t width, uint32_t height, uint32_t pitch, uint32_t z_format, uint32_t stencil_format, bool htile, bool neo,
                      TileSizeAlign* stencil_size, TileSizeAlign* htile_size, TileSizeAlign* depth_size)
{
	struct DepthInfo
	{
		uint32_t      width          = 0;
		uint32_t      height         = 0;
		uint32_t      z_format       = 0;
		uint32_t      stencil_format = 0;
		bool          tile           = false;
		bool          neo            = false;
		uint32_t      pitch          = 0;
		TileSizeAlign stencil        = {};
		TileSizeAlign htile          = {};
		TileSizeAlign depth          = {};
	};

	static const DepthInfo infos_base[] = {
	    {3840, 2160, 3, 0, true, false, 3840, {0, 0}, {655360, 2048}, {33423360, 32768}},
	    {3840, 2160, 3, 0, false, false, 3840, {0, 0}, {0, 0}, {33423360, 32768}},
	    {1920, 1080, 3, 0, true, false, 2048, {0, 0}, {196608, 2048}, {9437184, 32768}},
	    {1920, 1080, 3, 0, false, false, 2048, {0, 0}, {0, 0}, {9437184, 32768}},
	    {1280, 720, 3, 0, true, false, 1280, {0, 0}, {98304, 2048}, {3932160, 32768}},
	    {1280, 720, 3, 0, false, false, 1280, {0, 0}, {0, 0}, {3932160, 32768}},
	    {3840, 2160, 1, 0, true, false, 3840, {0, 0}, {655360, 2048}, {16711680, 32768}},
	    {3840, 2160, 1, 0, false, false, 3840, {0, 0}, {0, 0}, {16711680, 32768}},
	    {1920, 1080, 1, 0, true, false, 2048, {0, 0}, {196608, 2048}, {4718592, 32768}},
	    {1920, 1080, 1, 0, false, false, 2048, {0, 0}, {0, 0}, {4718592, 32768}},
	    {1280, 720, 1, 0, true, false, 1280, {0, 0}, {98304, 2048}, {1966080, 32768}},
	    {1280, 720, 1, 0, false, false, 1280, {0, 0}, {0, 0}, {1966080, 32768}},
	    {3840, 2160, 0, 1, true, false, 3840, {8355840, 32768}, {655360, 2048}, {0, 0}},
	    {3840, 2160, 0, 1, false, false, 3840, {8355840, 32768}, {0, 0}, {0, 0}},
	    {1920, 1080, 0, 1, true, false, 2048, {2359296, 32768}, {196608, 2048}, {0, 0}},
	    {1920, 1080, 0, 1, false, false, 2048, {2359296, 32768}, {0, 0}, {0, 0}},
	    {1280, 720, 0, 1, true, false, 1280, {983040, 32768}, {98304, 2048}, {0, 0}},
	    {1280, 720, 0, 1, false, false, 1280, {983040, 32768}, {0, 0}, {0, 0}},
	    {3840, 2160, 3, 1, true, false, 3840, {8355840, 32768}, {655360, 2048}, {33423360, 32768}},
	    {3840, 2160, 3, 1, false, false, 3840, {8355840, 32768}, {0, 0}, {33423360, 32768}},
	    {1920, 1080, 3, 1, true, false, 2048, {2359296, 32768}, {196608, 2048}, {9437184, 32768}},
	    {1920, 1080, 3, 1, false, false, 2048, {2359296, 32768}, {0, 0}, {9437184, 32768}},
	    {1280, 720, 3, 1, true, false, 1280, {983040, 32768}, {98304, 2048}, {3932160, 32768}},
	    {1280, 720, 3, 1, false, false, 1280, {983040, 32768}, {0, 0}, {3932160, 32768}},
	    {3840, 2160, 1, 1, true, false, 3840, {8355840, 32768}, {655360, 2048}, {16711680, 32768}},
	    {3840, 2160, 1, 1, false, false, 3840, {8355840, 32768}, {0, 0}, {16711680, 32768}},
	    {1920, 1080, 1, 1, true, false, 2048, {2359296, 32768}, {196608, 2048}, {4718592, 32768}},
	    {1920, 1080, 1, 1, false, false, 2048, {2359296, 32768}, {0, 0}, {4718592, 32768}},
	    {1280, 720, 1, 1, true, false, 1280, {983040, 32768}, {98304, 2048}, {1966080, 32768}},
	    {1280, 720, 1, 1, false, false, 1280, {983040, 32768}, {0, 0}, {1966080, 32768}},
	};

	static const DepthInfo infos_neo[] = {
	    {3840, 2160, 3, 0, true, true, 3840, {0, 0}, {655360, 4096}, {33423360, 65536}},
	    {3840, 2160, 3, 0, false, true, 3840, {0, 0}, {0, 0}, {33423360, 65536}},
	    {1920, 1080, 3, 0, true, true, 1920, {0, 0}, {196608, 4096}, {8847360, 65536}},
	    {1920, 1080, 3, 0, false, true, 1920, {0, 0}, {0, 0}, {8847360, 65536}},
	    {1920, 1080, 3, 0, true, true, 2048, {0, 0}, {196608, 4096}, {9437184, 65536}},
	    {1920, 1080, 3, 0, false, true, 2048, {0, 0}, {0, 0}, {9437184, 65536}},
	    {1280, 720, 3, 0, true, true, 1280, {0, 0}, {131072, 4096}, {3932160, 65536}},
	    {1280, 720, 3, 0, false, true, 1280, {0, 0}, {0, 0}, {3932160, 65536}},
	    {3840, 2160, 1, 0, true, true, 3840, {0, 0}, {655360, 4096}, {16711680, 65536}},
	    {3840, 2160, 1, 0, false, true, 3840, {0, 0}, {0, 0}, {16711680, 65536}},
	    {1920, 1080, 1, 0, true, true, 2048, {0, 0}, {196608, 4096}, {4718592, 65536}},
	    {1920, 1080, 1, 0, false, true, 2048, {0, 0}, {0, 0}, {4718592, 65536}},
	    {1280, 720, 1, 0, true, true, 1280, {0, 0}, {131072, 4096}, {1966080, 65536}},
	    {1280, 720, 1, 0, false, true, 1280, {0, 0}, {0, 0}, {1966080, 65536}},
	    {3840, 2160, 0, 1, true, true, 3840, {8355840, 32768}, {655360, 4096}, {0, 0}},
	    {3840, 2160, 0, 1, false, true, 3840, {8355840, 32768}, {0, 0}, {0, 0}},
	    {1920, 1080, 0, 1, true, true, 2048, {2359296, 32768}, {196608, 4096}, {0, 0}},
	    {1920, 1080, 0, 1, false, true, 2048, {2359296, 32768}, {0, 0}, {0, 0}},
	    {1280, 720, 0, 1, true, true, 1280, {983040, 32768}, {131072, 4096}, {0, 0}},
	    {1280, 720, 0, 1, false, true, 1280, {983040, 32768}, {0, 0}, {0, 0}},
	    {3840, 2160, 3, 1, true, true, 3840, {8355840, 32768}, {655360, 4096}, {33423360, 65536}},
	    {3840, 2160, 3, 1, false, true, 3840, {8355840, 32768}, {0, 0}, {33423360, 65536}},
	    {1920, 1080, 3, 1, true, true, 2048, {2359296, 32768}, {196608, 4096}, {9437184, 65536}},
	    {1920, 1080, 3, 1, false, true, 2048, {2359296, 32768}, {0, 0}, {9437184, 65536}},
	    {1280, 720, 3, 1, true, true, 1280, {983040, 32768}, {131072, 4096}, {3932160, 65536}},
	    {1280, 720, 3, 1, false, true, 1280, {983040, 32768}, {0, 0}, {3932160, 65536}},
	    {3840, 2160, 1, 1, true, true, 3840, {8355840, 32768}, {655360, 4096}, {16711680, 65536}},
	    {3840, 2160, 1, 1, false, true, 3840, {8355840, 32768}, {0, 0}, {16711680, 65536}},
	    {1920, 1080, 1, 1, true, true, 2048, {2359296, 32768}, {196608, 4096}, {4718592, 65536}},
	    {1920, 1080, 1, 1, false, true, 2048, {2359296, 32768}, {0, 0}, {4718592, 65536}},
	    {1280, 720, 1, 1, true, true, 1280, {983040, 32768}, {131072, 4096}, {1966080, 65536}},
	    {1280, 720, 1, 1, false, true, 1280, {983040, 32768}, {0, 0}, {1966080, 65536}},
	};

	EXIT_IF(depth_size == nullptr);
	EXIT_IF(htile_size == nullptr);
	EXIT_IF(stencil_size == nullptr);

	if (neo)
	{
		for (const auto& i: infos_neo)
		{
			if (i.width == width && i.height == height && i.pitch == pitch && i.tile == htile && i.z_format == z_format &&
			    i.stencil_format == stencil_format)
			{
				*depth_size   = i.depth;
				*htile_size   = i.htile;
				*stencil_size = i.stencil;
				return true;
			}
		}
	} else
	{
		for (const auto& i: infos_base)
		{
			if (i.width == width && i.height == height && i.pitch == pitch && i.tile == htile && i.z_format == z_format &&
			    i.stencil_format == stencil_format)
			{
				*depth_size   = i.depth;
				*htile_size   = i.htile;
				*stencil_size = i.stencil;
				return true;
			}
		}
	}
	*depth_size   = TileSizeAlign();
	*htile_size   = TileSizeAlign();
	*stencil_size = TileSizeAlign();
	return false;
}

void TileGetVideoOutSize(uint32_t width, uint32_t height, uint32_t pitch, bool tile, bool neo, TileSizeAlign* size)
{
	EXIT_IF(size == nullptr);

	uint32_t ret_size  = 0;
	uint32_t ret_align = 0;

	if (pitch == 3840)
	{
		if (width == 3840 && height == 2160 && tile && !neo)
		{
			ret_size  = 33423360;
			ret_align = 32768;
		}
		if (width == 3840 && height == 2160 && tile && neo)
		{
			ret_size  = 33423360;
			ret_align = 65536;
		}
		if (width == 3840 && height == 2160 && !tile && !neo)
		{
			ret_size  = 33177600;
			ret_align = 256;
		}
		if (width == 3840 && height == 2160 && !tile && neo)
		{
			ret_size  = 33177600;
			ret_align = 256;
		}
	}

	if (pitch == 1920)
	{
		if (width == 1920 && height == 1080 && tile && !neo)
		{
			ret_size  = 8355840;
			ret_align = 32768;
		}
		if (width == 1920 && height == 1080 && tile && neo)
		{
			ret_size  = 8847360;
			ret_align = 65536;
		}
		if (width == 1920 && height == 1080 && !tile && !neo)
		{
			ret_size  = 8294400;
			ret_align = 256;
		}
		if (width == 1920 && height == 1080 && !tile && neo)
		{
			ret_size  = 8294400;
			ret_align = 256;
		}
	}

	if (pitch == 1280)
	{
		if (width == 1280 && height == 720 && tile && !neo)
		{
			ret_size  = 3932160;
			ret_align = 32768;
		}
		if (width == 1280 && height == 720 && tile && neo)
		{
			ret_size  = 3932160;
			ret_align = 65536;
		}
		if (width == 1280 && height == 720 && !tile && !neo)
		{
			ret_size  = 3686400;
			ret_align = 256;
		}
		if (width == 1280 && height == 720 && !tile && neo)
		{
			ret_size  = 3686400;
			ret_align = 256;
		}
	}

	size->size  = ret_size;
	size->align = ret_align;
}

struct TextureInfoPadded
{
	uint32_t width  = 0;
	uint32_t height = 0;
};

struct TextureInfoSize
{
	uint32_t total_size    = 0;
	uint32_t total_align   = 0;
	uint32_t mipmap_offset = 0;
	uint32_t mipmap_size   = 0;
};

struct TextureInfo
{
	uint32_t          dfmt   = 0;
	uint32_t          nfmt   = 0;
	uint32_t          width  = 0;
	uint32_t          height = 0;
	uint32_t          pitch  = 0;
	uint32_t          levels = 0;
	uint32_t          tile   = 0;
	bool              neo    = false;
	TextureInfoSize   size[16];
	TextureInfoPadded padded[16];
};

#include "Tables/TileTextureInfo_10_10_0.inc"
#include "Tables/TileTextureInfo_13_10_0.inc"
#include "Tables/TileTextureInfo_13_10_9.inc"
#include "Tables/TileTextureInfo_13_35_0.inc"
#include "Tables/TileTextureInfo_13_36_0.inc"
#include "Tables/TileTextureInfo_13_37_0.inc"
#include "Tables/TileTextureInfo_13_37_9.inc"
#include "Tables/TileTextureInfo_14_10_0.inc"
#include "Tables/TileTextureInfo_2_4_7.inc"
#include "Tables/TileTextureInfo_8_10_0.inc"
#include "Tables/TileTextureInfo_8_10_9.inc"
#include "Tables/TileTextureInfo_8_1_0.inc"

static const TextureInfo* g_info_map_10_10_0[16][16][16][2] = {};
static const TextureInfo* g_info_map_13_10_0[16][16][16][2] = {};
static const TextureInfo* g_info_map_13_10_9[16][16][16][2] = {};
static const TextureInfo* g_info_map_13_35_0[16][16][16][2] = {};
static const TextureInfo* g_info_map_13_36_0[16][16][16][2] = {};
static const TextureInfo* g_info_map_13_37_0[16][16][16][2] = {};
static const TextureInfo* g_info_map_13_37_9[16][16][16][2] = {};
static const TextureInfo* g_info_map_14_10_0[16][16][16][2] = {};
static const TextureInfo* g_info_map_2_4_7[16][16][16][2]   = {};
static const TextureInfo* g_info_map_8_1_0[16][16][16][2]   = {};
static const TextureInfo* g_info_map_8_10_0[16][16][16][2]  = {};
static const TextureInfo* g_info_map_8_10_9[16][16][16][2]  = {};

template <class Map>
static void init_map(Map& map, const TextureInfo* info, int num)
{
	for (int w = 0; w < 16; w++)
	{
		for (int h = 0; h < 16; h++)
		{
			for (int p = 0; p < 16; p++)
			{
				for (int n = 0; n < 2; n++)
				{
					map[w][h][p][n] = nullptr;
				}
			}
		}
	}

	for (int index = 0; index < num; index++)
	{
		const auto& i = info[index];
		if (!(i.width == 0 && i.height == 0 && i.pitch == 0))
		{
			EXIT_IF(!(IsPowerOfTwo(i.width) && IsPowerOfTwo(i.height) && IsPowerOfTwo(i.pitch)));
			auto w = IntLog2(i.width);
			auto h = IntLog2(i.height);
			auto p = IntLog2(i.pitch);
			auto n = i.neo ? 1 : 0;
			EXIT_IF(w >= 16 || h >= 16 || p >= 16 || n >= 2);
			EXIT_IF(map[w][h][p][n] != nullptr);
			map[w][h][p][n] = &i;
		}
	}
}

template <class Map>
static const TextureInfo* check_map(Map& map, uint32_t width, uint32_t height, uint32_t pitch, bool neo)
{
	auto w = IntLog2(width);
	auto h = IntLog2(height);
	auto p = IntLog2(pitch);
	auto n = neo ? 1 : 0;
	if (w >= 16 || h >= 16 || p >= 16)
	{
		return nullptr;
	}
	return map[w][h][p][n];
}

static void init_maps()
{
	init_map(g_info_map_10_10_0, infos_10_10_0_pow2, std::size(infos_10_10_0_pow2));
	init_map(g_info_map_13_10_0, infos_13_10_0_pow2, std::size(infos_13_10_0_pow2));
	init_map(g_info_map_13_10_9, infos_13_10_9_pow2, std::size(infos_13_10_9_pow2));
	init_map(g_info_map_13_35_0, infos_13_35_0_pow2, std::size(infos_13_35_0_pow2));
	init_map(g_info_map_13_36_0, infos_13_36_0_pow2, std::size(infos_13_36_0_pow2));
	init_map(g_info_map_13_37_0, infos_13_37_0_pow2, std::size(infos_13_37_0_pow2));
	init_map(g_info_map_13_37_9, infos_13_37_9_pow2, std::size(infos_13_37_9_pow2));
	init_map(g_info_map_14_10_0, infos_14_10_0_pow2, std::size(infos_14_10_0_pow2));
	init_map(g_info_map_2_4_7, infos_2_4_7_pow2, std::size(infos_2_4_7_pow2));
	init_map(g_info_map_8_1_0, infos_8_1_0_pow2, std::size(infos_8_1_0_pow2));
	init_map(g_info_map_8_10_0, infos_8_10_0_pow2, std::size(infos_8_10_0_pow2));
	init_map(g_info_map_8_10_9, infos_8_10_9_pow2, std::size(infos_8_10_9_pow2));
}

#define LUMEN_TEXTURE_CHECK(n)                                                                                                              \
	if (pow2)                                                                                                                              \
	{                                                                                                                                      \
		if (const auto* i = check_map(g_info_map_##n, width, height, pitch, neo); i != nullptr)                                            \
		{                                                                                                                                  \
			*info = i;                                                                                                                     \
			*num  = 1;                                                                                                                     \
		} else                                                                                                                             \
		{                                                                                                                                  \
			*info = nullptr;                                                                                                               \
			*num  = 0;                                                                                                                     \
		}                                                                                                                                  \
	} else                                                                                                                                 \
	{                                                                                                                                      \
		*info = infos_##n;                                                                                                                 \
		*num  = std::size(infos_##n);                                                                                                      \
	}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
static void FindTextureInfo(uint32_t dfmt, uint32_t nfmt, uint32_t width, uint32_t height, uint32_t pitch, uint32_t tile, bool neo,
                            const TextureInfo** info, int* num, bool pow2)
{
	EXIT_IF(info == nullptr);
	EXIT_IF(num == nullptr);

	switch (tile)
	{
		case 8:
			if (dfmt == 1 && nfmt == 0)
			{
				LUMEN_TEXTURE_CHECK(8_1_0);
			} else if (dfmt == 10 && nfmt == 0)
			{
				LUMEN_TEXTURE_CHECK(8_10_0);
			} else if (dfmt == 10 && nfmt == 9)
			{
				LUMEN_TEXTURE_CHECK(8_10_9);
			}
			break;
		case 13:
			if (dfmt == 10 && nfmt == 0)
			{
				LUMEN_TEXTURE_CHECK(13_10_0);
			} else if (dfmt == 10 && nfmt == 9)
			{
				LUMEN_TEXTURE_CHECK(13_10_9);
			} else if (dfmt == 35 && nfmt == 0)
			{
				LUMEN_TEXTURE_CHECK(13_35_0);
			} else if (dfmt == 36 && nfmt == 0)
			{
				LUMEN_TEXTURE_CHECK(13_36_0);
			} else if (dfmt == 37 && nfmt == 0)
			{
				LUMEN_TEXTURE_CHECK(13_37_0);
			} else if (dfmt == 37 && nfmt == 9)
			{
				LUMEN_TEXTURE_CHECK(13_37_9);
			}
			break;
		case 14:
			if (dfmt == 10 && nfmt == 0)
			{
				LUMEN_TEXTURE_CHECK(14_10_0);
			}
			break;
		case 10:
			if (dfmt == 10 && nfmt == 0)
			{
				LUMEN_TEXTURE_CHECK(10_10_0);
			}
			break;
		case 2:
			if (dfmt == 4 && nfmt == 7)
			{
				LUMEN_TEXTURE_CHECK(2_4_7);
			}
			break;
		default: *info = nullptr; *num = 0;
	}
}

static bool TryNextPowerOfTwo(uint32_t value, uint32_t* result)
{
	if (value == 0 || result == nullptr || value > 0x80000000u)
	{
		return false;
	}
	value--;
	value |= value >> 1u;
	value |= value >> 2u;
	value |= value >> 4u;
	value |= value >> 8u;
	value |= value >> 16u;
	*result = value + 1u;
	return true;
}

bool TileTryGetDisplayLinearAlignedTextureSize(uint32_t dfmt, uint32_t nfmt, uint32_t width, uint32_t height, uint32_t pitch,
                                               uint32_t levels, uint32_t tile, bool neo, bool pow2_pad, TileSizeAlign* total_size,
                                               TileSizeOffset* level_sizes, TilePaddedSize* padded_size)
{
	// POW2_PAD defines a power-of-two memory footprint. The `_pow2` fixture
	// suffix describes the resulting physical geometry, not the descriptor bit.
	// Keep this resolver on the exact observed RGBA8 DisplayLinearAligned profile.
	if (!pow2_pad || tile != 8 || dfmt != 10 || nfmt != 0 || width == 0 || height == 0 || levels < 2 || levels >= 16)
	{
		return false;
	}

	uint32_t maximum_levels = 1;
	for (uint32_t dimension = std::max(width, height); dimension > 1; dimension >>= 1u)
	{
		maximum_levels++;
	}
	if (levels > maximum_levels)
	{
		return false;
	}

	uint32_t padded_width  = 0;
	uint32_t padded_height = 0;
	if (!TryNextPowerOfTwo(width, &padded_width) || !TryNextPowerOfTwo(height, &padded_height) || pitch != padded_width)
	{
		return false;
	}

	for (uint32_t level = 0; level < levels; ++level)
	{
		const uint32_t logical_width   = std::max(width >> level, 1u);
		const uint32_t logical_height  = std::max(height >> level, 1u);
		const uint32_t physical_width  = std::max(padded_width >> level, 1u);
		const uint32_t physical_height = std::max(padded_height >> level, 1u);
		const uint64_t physical_row    = static_cast<uint64_t>(physical_width) * 4u;
		uint32_t       independently_padded_width  = 0;
		uint32_t       independently_padded_height = 0;
		if (!TryNextPowerOfTwo(logical_width, &independently_padded_width) ||
		    !TryNextPowerOfTwo(logical_height, &independently_padded_height) ||
		    independently_padded_width != physical_width || independently_padded_height != physical_height || physical_row < 64u ||
		    physical_row * physical_height < 256u)
		{
			return false;
		}
	}

	const TextureInfo* oracle = nullptr;
	for (const auto& info: infos_8_10_0_pow2)
	{
		if (info.neo == neo && info.width == padded_width && info.height == padded_height && info.pitch == pitch &&
		    info.levels >= levels)
		{
			oracle = &info;
			break;
		}
	}
	if (oracle == nullptr)
	{
		return false;
	}

	uint64_t expected_offset = 0;
	for (uint32_t level = 0; level < levels; ++level)
	{
		const uint64_t physical_width  = std::max(padded_width >> level, 1u);
		const uint64_t physical_height = std::max(padded_height >> level, 1u);
		const uint64_t expected_size   = physical_width * 4u * physical_height;
		if (expected_size > UINT32_MAX || oracle->size[level].mipmap_offset != expected_offset ||
		    oracle->size[level].mipmap_size != expected_size || oracle->size[level].total_align != 256u)
		{
			return false;
		}
		expected_offset += expected_size;
	}
	if (expected_offset > UINT32_MAX || oracle->size[levels - 1].total_size != expected_offset)
	{
		return false;
	}

	if (total_size != nullptr)
	{
		total_size->size  = oracle->size[levels - 1].total_size;
		total_size->align = oracle->size[levels - 1].total_align;
	}
	if (level_sizes != nullptr)
	{
		for (uint32_t level = 0; level < levels; ++level)
		{
			level_sizes[level].offset = oracle->size[level].mipmap_offset;
			level_sizes[level].size   = oracle->size[level].mipmap_size;
		}
	}
	if (padded_size != nullptr)
	{
		for (uint32_t level = 0; level < levels; ++level)
		{
			padded_size[level].width  = oracle->padded[level].width;
			padded_size[level].height = oracle->padded[level].height;
		}
	}
	return true;
}

bool TileTryGetThin1dPow2PaddedTextureSize(uint32_t dfmt, uint32_t nfmt, uint32_t width, uint32_t height, uint32_t pitch,
                                          uint32_t levels, uint32_t tile, bool pow2_pad, TileSizeAlign* total_size,
                                          TileSizeOffset* level_sizes, TilePaddedSize* padded_size)
{
	// This is the narrow Addrlib Thin1D profile for which the descriptor asks
	// that the allocation footprint of every mip be padded to power-of-two
	// dimensions. Keep custom pitches and other formats on the table/fail-closed
	// paths until their layout has an equally strong derivation.
	uint32_t canonical_pitch = 0;
	if (!pow2_pad || tile != 13 || dfmt != 10 || (nfmt != 0 && nfmt != 9) || width == 0 || height == 0 || levels < 2 ||
	    levels >= 16 || !TryNextPowerOfTwo(width, &canonical_pitch))
	{
		return false;
	}
	uint32_t maximum_levels = 1;
	for (uint32_t dimension = std::max(width, height); dimension > 1; dimension >>= 1u)
	{
		maximum_levels++;
	}
	if (levels > maximum_levels)
	{
		return false;
	}
	canonical_pitch = std::max(canonical_pitch, 8u);
	if (pitch != canonical_pitch)
	{
		return false;
	}

	TileSizeOffset local_levels[16] {};
	TilePaddedSize local_padded[16] {};
	uint64_t       total = 0;
	for (uint32_t level = 0; level < levels; ++level)
	{
		const uint32_t logical_pitch  = std::max(pitch >> level, 1u);
		const uint32_t logical_height = std::max(height >> level, 1u);
		uint32_t       physical_pitch = 0;
		uint32_t       physical_height = 0;
		if (!TryNextPowerOfTwo(logical_pitch, &physical_pitch) || !TryNextPowerOfTwo(logical_height, &physical_height))
		{
			return false;
		}
		physical_pitch  = std::max(physical_pitch, 8u);
		physical_height = std::max(physical_height, 8u);

		const uint64_t level_size = static_cast<uint64_t>(physical_pitch) * physical_height * 4u;
		if (level_size == 0 || level_size > UINT32_MAX || total > UINT32_MAX - level_size)
		{
			return false;
		}
		local_levels[level].offset = static_cast<uint32_t>(total);
		local_levels[level].size   = static_cast<uint32_t>(level_size);
		local_padded[level].width  = physical_pitch;
		// Generated Addrlib fixtures retain the base level's logical Thin1D
		// height in this metadata even though POW2_PAD expands its footprint.
		// Tiler1d consumes the padded width for addressing; keeping this value
		// preserves table parity for diagnostics and tests.
		const uint64_t base_padded_height = (static_cast<uint64_t>(logical_height) + 7u) & ~uint64_t {7};
		if (base_padded_height > UINT32_MAX)
		{
			return false;
		}
		local_padded[level].height = level == 0 ? static_cast<uint32_t>(base_padded_height) : physical_height;
		total += level_size;
	}

	if (total_size != nullptr)
	{
		total_size->size  = static_cast<uint32_t>(total);
		total_size->align = 256;
	}
	if (level_sizes != nullptr)
	{
		for (uint32_t level = 0; level < levels; ++level)
		{
			level_sizes[level] = local_levels[level];
		}
	}
	if (padded_size != nullptr)
	{
		for (uint32_t level = 0; level < levels; ++level)
		{
			padded_size[level] = local_padded[level];
		}
	}
	return true;
}

bool TileTryGetDisplay2dThinTextureSize(uint32_t dfmt, uint32_t nfmt, uint32_t width, uint32_t height, uint32_t pitch,
	                                    uint32_t levels, uint32_t tile, bool neo, bool pow2_pad, TileSizeAlign* total_size,
	                                    TileSizeOffset* level_sizes, TilePaddedSize* padded_size)
{
	if (dfmt != 10 || nfmt != 0 || levels != 1 || tile != 10 || pow2_pad || width == 0 || height == 0 || pitch == 0 ||
	    width > pitch || (pitch & 127u) != 0)
	{
		return false;
	}

	const uint64_t macro_height  = neo ? 128u : 64u;
	const uint64_t padded_height = (static_cast<uint64_t>(height) + macro_height - 1u) & ~(macro_height - 1u);
	const uint64_t row_size      = static_cast<uint64_t>(pitch) * 4u;
	if (padded_height == 0 || padded_height > UINT32_MAX || row_size == 0 || row_size > UINT32_MAX ||
	    padded_height > UINT32_MAX / row_size)
	{
		return false;
	}
	const auto size = static_cast<uint32_t>(padded_height * row_size);

	if (total_size != nullptr)
	{
		total_size->size  = size;
		total_size->align = neo ? 65536u : 32768u;
	}
	if (level_sizes != nullptr)
	{
		level_sizes[0].size   = size;
		level_sizes[0].offset = 0;
	}
	if (padded_size != nullptr)
	{
		// Generated tile-10 fixtures expose no conversion-oriented padded
		// dimensions. Preserve that contract for the calculated fallback.
		padded_size[0] = {};
	}
	return true;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void TileGetTextureSize(uint32_t dfmt, uint32_t nfmt, uint32_t width, uint32_t height, uint32_t pitch, uint32_t levels, uint32_t tile,
                        bool neo, TileSizeAlign* total_size, TileSizeOffset* level_sizes, TilePaddedSize* padded_size, bool pow2_pad)
{
	LUMEN_PROFILER_FUNCTION();

	if (dfmt == 41)
	{
		Bc7Thin1dLayout layout {};
		EXIT_NOT_IMPLEMENTED(!ResolveBc7Thin1dLayout(dfmt, nfmt, width, height, pitch, 0, levels, tile, pow2_pad, &layout));
		if (total_size != nullptr) *total_size = {layout.bytes, layout.alignment};
		if (level_sizes != nullptr) level_sizes[0] = {layout.bytes, 0};
		if (padded_size != nullptr) padded_size[0] = {layout.padded_block_width, layout.padded_block_height};
		return;
	}

	const TextureInfo* infos = nullptr;
	int                num   = 0;

	bool pow2 = (IsPowerOfTwo(width) && IsPowerOfTwo(height) && IsPowerOfTwo(pitch));

	FindTextureInfo(dfmt, nfmt, width, height, pitch, tile, neo, &infos, &num, pow2);

	const bool thin_1d_pow2_fallback =
	    TileTryGetThin1dPow2PaddedTextureSize(dfmt, nfmt, width, height, pitch, levels, tile, pow2_pad, nullptr, nullptr, nullptr);
	const bool display_2d_thin_fallback =
	    TileTryGetDisplay2dThinTextureSize(dfmt, nfmt, width, height, pitch, levels, tile, neo, pow2_pad, nullptr, nullptr, nullptr);
	bool thin_1d_fallback =
	    (levels == 1 && tile == 13 &&
	     ((dfmt == 1 && nfmt == 0) || (dfmt == 3 && nfmt == 0) || (dfmt == 10 && (nfmt == 0 || nfmt == 9)) ||
	      (dfmt == 35 && nfmt == 0) || (dfmt == 36 && nfmt == 0) || (dfmt == 37 && (nfmt == 0 || nfmt == 9)))) ||
	    thin_1d_pow2_fallback;

	if (tile != 31 && tile != 8 && infos == nullptr && !thin_1d_fallback && !display_2d_thin_fallback)
	{
		fprintf(stderr, "[TILE] Unsupported tile mode %u for dfmt=%u nfmt=%u %ux%u pitch=%u levels=%u neo=%d pow2=%d — treating as linear\n",
		        tile, dfmt, nfmt, width, height, pitch, levels, neo ? 1 : 0, pow2 ? 1 : 0);
		fflush(stderr);
		// Fallback: treat unsupported tile modes as linear (tile 31)
		// Texture data will be visually wrong but game can continue
		FindTextureInfo(dfmt, nfmt, width, height, pitch, 31, neo, &infos, &num, pow2);
	}
	// EXIT_NOT_IMPLEMENTED(tile != 31 && tile != 8 && infos == nullptr);

	EXIT_IF(levels == 0 || levels > 16);

	for (int index = 0; index < num; index++)
	{
		const auto& i = infos[index];
		if (i.dfmt == dfmt && i.nfmt == nfmt && i.width == width && i.pitch == pitch && i.height == height && i.levels >= levels &&
		    i.tile == tile && i.neo == neo)
		{
			if (total_size != nullptr)
			{
				total_size->size  = i.size[levels - 1].total_size;
				total_size->align = i.size[levels - 1].total_align;
			}

			if (level_sizes != nullptr)
			{
				if (levels == 1)
				{
					level_sizes[0].size   = i.size[0].total_size;
					level_sizes[0].offset = 0;
				} else
				{
					for (uint32_t l = 0; l < levels; l++)
					{
						level_sizes[l].size   = i.size[l].mipmap_size;
						level_sizes[l].offset = i.size[l].mipmap_offset;
					}
				}
			}

			if (padded_size != nullptr)
			{
				for (uint32_t l = 0; l < levels; l++)
				{
					padded_size[l].width  = i.padded[l].width;
					padded_size[l].height = i.padded[l].height;
				}
			}
			return;
		}
	}

	if (thin_1d_pow2_fallback)
	{
		const bool calculated = TileTryGetThin1dPow2PaddedTextureSize(dfmt, nfmt, width, height, pitch, levels, tile, pow2_pad,
		                                                              total_size, level_sizes, padded_size);
		EXIT_IF(!calculated);
		return;
	}

	if (display_2d_thin_fallback)
	{
		const bool calculated = TileTryGetDisplay2dThinTextureSize(dfmt, nfmt, width, height, pitch, levels, tile, neo, pow2_pad,
		                                                           total_size, level_sizes, padded_size);
		EXIT_IF(!calculated);
		return;
	}

	if (TileTryGetDisplayLinearAlignedTextureSize(dfmt, nfmt, width, height, pitch, levels, tile, neo, pow2_pad, total_size,
	                                              level_sizes, padded_size))
	{
		return;
	}

	if (levels == 1)
	{
		uint32_t size              = 0;
		uint32_t bytes_per_element = 0;
		uint64_t element_width     = pitch;
		uint64_t element_height    = height;

		if ((dfmt == 10 && nfmt == 9) || (dfmt == 10 && nfmt == 0))
		{
			bytes_per_element = 4;
		} else if ((dfmt == 3 && nfmt == 0))
		{
			bytes_per_element = 2;
		} else if ((dfmt == 1 && nfmt == 0))
		{
			bytes_per_element = 1;
		} else if ((dfmt == 37 && nfmt == 0) || (dfmt == 37 && nfmt == 9) || (dfmt == 36 && nfmt == 0))
		{
			// BC2/BC3 compressed: 128 bits per 4x4 block
			element_width     = std::max((element_width + 3) / 4, uint64_t {1});
			element_height    = std::max((element_height + 3) / 4, uint64_t {1});
			bytes_per_element = 16;
		} else if ((dfmt == 35 && nfmt == 0))
		{
			// BC1 compressed: 64 bits per 4x4 block
			element_width     = std::max((element_width + 3) / 4, uint64_t {1});
			element_height    = std::max((element_height + 3) / 4, uint64_t {1});
			bytes_per_element = 8;
		}

		if (bytes_per_element != 0)
		{
			if (tile == 13)
			{
				// Thin 1D textures are stored as complete 8x8 micro-tiles.
				// For block-compressed formats the elements are 4x4 blocks,
				// not individual texels.
				element_width  = (element_width + 7) & ~uint64_t {7};
				element_height = (element_height + 7) & ~uint64_t {7};

				// Addrlib also requires the slice to respect the 256-byte
				// base alignment. Increase the pitch by whole micro-tiles;
				// RGBA8 and BC formats already satisfy this after one 8x8
				// tile, while smaller elements can require more columns.
				while (((element_width * element_height * bytes_per_element) & uint64_t {0xff}) != 0)
				{
					element_width += 8;
				}
			}

			EXIT_IF(element_width == 0 || element_height == 0 || element_width > UINT32_MAX || element_height > UINT32_MAX ||
			        element_width > UINT32_MAX / bytes_per_element);
			uint64_t row_size = element_width * bytes_per_element;
			EXIT_IF(element_height > UINT32_MAX / row_size);
			uint64_t size64 = row_size * element_height;
			size = static_cast<uint32_t>(size64);
		}

		if (total_size != nullptr)
		{
			total_size->size  = size;
			total_size->align = 256;
		}
		if (level_sizes != nullptr)
		{
			level_sizes[0].size   = size;
			level_sizes[0].offset = 0;
		}
		if (padded_size != nullptr)
		{
			padded_size[0].width  = static_cast<uint32_t>(element_width);
			padded_size[0].height = static_cast<uint32_t>(element_height);
		}
	}

	if (total_size != nullptr && total_size->size == 0)
	{
		Core::StringList list;
		list.Add(String::FromPrintf("dfmt   = %u", dfmt));
		list.Add(String::FromPrintf("nfmt   = %u", nfmt));
		list.Add(String::FromPrintf("width  = %u", width));
		list.Add(String::FromPrintf("height = %u", height));
		list.Add(String::FromPrintf("pitch  = %u", pitch));
		list.Add(String::FromPrintf("levels = %u", levels));
		list.Add(String::FromPrintf("tile   = %u", tile));
		list.Add(String::FromPrintf("neo    = %s", neo ? "true" : "false"));
		list.Add(String::FromPrintf("pow2   = %s", pow2_pad ? "true" : "false"));
		EXIT("unknown format:\n%s\n", list.Concat(U'\n').C_Str());
	}
}

} // namespace Lumen::Libs::Graphics

#endif // LUMEN_EMU_ENABLED
