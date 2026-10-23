#ifndef LUMEN_EMULATOR_GRAPHICS_HANDOFF_TEXEL_READBACK_CACHE_H
#define LUMEN_EMULATOR_GRAPHICS_HANDOFF_TEXEL_READBACK_CACHE_H

#include <cstdint>
#include <cstring>
#include <limits>
#include <list>
#include <vector>

namespace Lumen::Libs::Graphics {

// Native texels only. Row/mip padding and bytes outside the image never enter
// the retained payload: every publication overlays these rows onto its freshly
// captured guest bytes. The owner supplies a lifetime and mutation generation;
// a completed producer is still required on every publication, including hits.
class HandoffTexelReadbackCache
{
public:
	struct Key
	{
		uint64_t lifetime = 0, native_epoch = 0, native_memory = 0, image = 0;
		bool operator==(const Key& other) const
		{
			return lifetime == other.lifetime && native_epoch == other.native_epoch &&
			       native_memory == other.native_memory && image == other.image;
		}
		bool Valid() const { return lifetime != 0 && native_epoch != 0 && native_memory != 0 && image != 0; }
	};
	struct Rows
	{
		uint64_t offset = 0, row_bytes = 0, stride = 0, height = 0;
		bool operator==(const Rows& other) const
		{
			return offset == other.offset && row_bytes == other.row_bytes && stride == other.stride && height == other.height;
		}
	};
	static constexpr uint64_t MAX_RETAINED_BYTES = 32u * 1024u * 1024u;
	static constexpr size_t MAX_RETAINED_ENTRIES = 8;

	void Invalidate(uint64_t lifetime = 0)
	{
		for (auto entry = m_entries.begin(); entry != m_entries.end();)
		{
			if (lifetime == 0 || lifetime == entry->key.lifetime)
			{
				m_retained_bytes -= entry->texels.size();
				entry = m_entries.erase(entry);
			} else ++entry;
		}
	}

	template <typename Readback>
	bool Merge(const Key& key, const std::vector<Rows>& rows, std::vector<uint8_t>* guest, Readback&& readback,
	           bool* cache_hit = nullptr)
	{
		if (cache_hit != nullptr) *cache_hit = false;
		if (guest == nullptr || guest->empty() || rows.empty() || rows.size() > 16) return false;
		const uint64_t bytes = guest->size();
		uint64_t texel_bytes = 0, previous_end = 0;
		for (const auto& row: rows)
		{
			if (row.height == 0 || row.row_bytes == 0 || row.row_bytes > row.stride || row.offset < previous_end ||
			    row.offset > bytes || row.row_bytes > bytes - row.offset ||
			    row.height - 1 > (bytes - row.offset - row.row_bytes) / row.stride ||
			    row.height > (std::numeric_limits<uint64_t>::max() - texel_bytes) / row.row_bytes) return false;
			previous_end = row.offset + (row.height - 1) * row.stride + row.row_bytes;
			texel_bytes += row.height * row.row_bytes;
		}
		auto cached = m_entries.end();
		if (key.Valid())
			for (auto entry = m_entries.begin(); entry != m_entries.end(); ++entry)
				if (key == entry->key && bytes == entry->bytes && rows == entry->rows && entry->texels.size() == texel_bytes)
				{
					cached = entry;
					break;
				}
		const bool hit = cached != m_entries.end();
		std::vector<uint8_t> captured;
		if (hit)
		{
			// Reorder nodes, without copying the retained texels or allocating.
			m_entries.splice(m_entries.begin(), m_entries, cached);
		} else
		{
			// A failed or short capture cannot modify the guest snapshot or install
			// a cache entry. Preserve all entries until the new capture succeeds.
			captured.resize(static_cast<size_t>(bytes), 0);
			if (!readback(&captured) || captured.size() != bytes) return false;
			// Keep at most one version of each owner; a changed image, layout or
			// epoch must not leave an older version available for later reuse.
			if (key.lifetime != 0) Invalidate(key.lifetime);
			if (key.Valid() && texel_bytes <= MAX_RETAINED_BYTES)
			{
				while (m_entries.size() >= MAX_RETAINED_ENTRIES || m_retained_bytes > MAX_RETAINED_BYTES - texel_bytes)
				{
					m_retained_bytes -= m_entries.back().texels.size();
					m_entries.pop_back();
				}
				auto& entry = m_entries.emplace_front();
				entry.texels.resize(static_cast<size_t>(texel_bytes));
				size_t cursor = 0;
				for (const auto& row: rows)
					for (uint64_t y = 0; y < row.height; ++y)
					{
						std::memcpy(entry.texels.data() + cursor, captured.data() + row.offset + y * row.stride, row.row_bytes);
						cursor += row.row_bytes;
					}
				entry.key = key;
				entry.bytes = bytes;
				entry.rows = rows;
				m_retained_bytes += texel_bytes;
			}
		}
		size_t cursor = 0;
		for (const auto& row: rows)
			for (uint64_t y = 0; y < row.height; ++y)
			{
				const auto* source = hit ? cached->texels.data() + cursor : captured.data() + row.offset + y * row.stride;
				std::memcpy(guest->data() + row.offset + y * row.stride, source, row.row_bytes);
				cursor += row.row_bytes;
			}
		if (cache_hit != nullptr) *cache_hit = hit;
		return true;
	}

private:
	struct Entry
	{
		Key key;
		uint64_t bytes = 0;
		std::vector<Rows> rows;
		std::vector<uint8_t> texels;
	};
	// Most recently used first. The byte limit is shared by all entries.
	std::list<Entry> m_entries;
	uint64_t m_retained_bytes = 0;
};

} // namespace Lumen::Libs::Graphics

#endif
