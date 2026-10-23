#ifndef LUMEN_CORE_PROC_MAPS_READER_H
#define LUMEN_CORE_PROC_MAPS_READER_H

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#if defined(__unix__) || defined(__APPLE__)
#include <cerrno>
#include <unistd.h>
#endif

#if defined(__linux__)
#include <cerrno>
#include <linux/fs.h>
#include <sys/ioctl.h>
#endif

namespace Lumen::Core::ProcMaps {

struct Record
{
	uint64_t begin = 0;
	uint64_t end = 0;
	bool readable = false;
	bool writable = false;
};

enum class RangeQueryResult { Allowed, Denied, Unsupported };

#if defined(PROCMAP_QUERY)
// Ask the kernel for each covering VMA directly. No permissions or mapping
// answers survive this query; the caller must hold its existing mapping lease.
// Linux before 6.11 and Android policies denying this ioctl keep the fresh
// text route. Unavailable ioctl transport never grants VMA permissions.
inline RangeQueryResult QueryRange(int fd, uint64_t begin, uint64_t end,
                                  uint32_t rights, uint32_t* observed, int* error)
{
	if (error != nullptr) *error = 0;
	if (observed != nullptr) *observed = 0;
	if (begin >= end || (rights & ~3U) != 0 || rights == 0)
		return RangeQueryResult::Denied;
	while (begin < end)
	{
		procmap_query query {};
		query.size = sizeof(query);
		query.query_addr = begin;
		int result;
		do { result = ::ioctl(fd, PROCMAP_QUERY, &query); } while (result < 0 && errno == EINTR);
		if (result < 0)
		{
			const int failure = errno;
			if (error != nullptr) *error = failure;
			return failure == ENOTTY || failure == EOPNOTSUPP ||
			       failure == EACCES || failure == EPERM
			    ? RangeQueryResult::Unsupported : RangeQueryResult::Denied;
		}
		const uint32_t actual = static_cast<uint32_t>(query.vma_flags & 3U);
		if (observed != nullptr) *observed = actual;
		if (query.vma_start > begin || query.vma_end <= begin || (actual & rights) != rights)
			return RangeQueryResult::Denied;
		begin = (std::min)(end, static_cast<uint64_t>(query.vma_end));
	}
	return RangeQueryResult::Allowed;
}
#endif

// /proc maps uses hexadecimal addresses followed by four permission bytes.
// Parse only that fixed prefix, without locale conversion or pathname work.
inline bool ParseRecord(const char* line, size_t length, Record* result)
{
	if (line == nullptr || result == nullptr) return false;
	size_t cursor = 0;
	const auto hex = [&](uint64_t* output) {
		uint64_t value = 0;
		size_t digits = 0;
		while (cursor < length)
		{
			const unsigned char c = static_cast<unsigned char>(line[cursor]);
			unsigned digit;
			if (c >= '0' && c <= '9') digit = c - '0';
			else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
			else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
			else break;
			if (digits == 16) return false;
			value = (value << 4) | digit;
			++digits;
			++cursor;
		}
		if (digits == 0) return false;
		*output = value;
		return true;
	};
	Record candidate;
	if (!hex(&candidate.begin) || cursor == length || line[cursor++] != '-' ||
	    !hex(&candidate.end) || candidate.begin >= candidate.end) return false;
	const auto space = [](char c) { return c == ' ' || c == '\t'; };
	if (cursor == length || !space(line[cursor])) return false;
	while (cursor < length && space(line[cursor])) ++cursor;
	if (length - cursor < 4) return false;
	const char* permissions = line + cursor;
	if ((permissions[0] != 'r' && permissions[0] != '-') ||
	    (permissions[1] != 'w' && permissions[1] != '-') ||
	    (permissions[2] != 'x' && permissions[2] != '-') ||
	    (permissions[3] != 'p' && permissions[3] != 's')) return false;
	cursor += 4;
	if (cursor < length && !space(line[cursor]) && line[cursor] != '\n') return false;
	candidate.readable = permissions[0] == 'r';
	candidate.writable = permissions[1] == 'w';
	*result = candidate;
	return true;
}

// A reader belongs to one freshly opened maps query. It carries no permission
// authority into the next lease. Consume complete lines even when a pathname
// exceeds the buffer, so a pathname fragment cannot become a mapping record.
class Reader
{
public:
	explicit Reader(std::FILE* file): m_file(file) {}
#if defined(__unix__) || defined(__APPLE__)
	using DescriptorRead = std::ptrdiff_t (*)(int, void*, size_t, void*);
	// The caller owns this freshly opened descriptor until parsing finishes.
	// read() feeds the parser directly: no second stdio buffer or retained VMA.
	explicit Reader(int descriptor, DescriptorRead read = ReadDescriptor, void* context = nullptr)
	    : m_descriptor(descriptor), m_descriptor_read(read), m_read_context(context) {}
#endif
	bool Next(Record* result, uint64_t minimum_end = 0)
	{
		if (result == nullptr || !HasInput()) return false;
		char prefix[64];
		size_t length = 0;
		for (;;)
		{
			if (m_cursor == m_size)
			{
				if (m_finished) return false;
				// Early VMAs need only a short prefix. Requesting 4 KiB first
				// makes procfs format many unused mappings for a scalar borrow.
				// Later refills retain the larger buffer for distant addresses.
				m_size = Refill(m_first_refill ? 512 : sizeof(m_buffer));
				m_first_refill = false;
				m_cursor = 0;
				if (m_size == 0)
				{
					m_finished = true;
					return !m_read_failed && length != 0 && ParseAfter(prefix, length, minimum_end, result);
				}
			}
			const char* begin = m_buffer + m_cursor;
			const auto* newline = static_cast<const char*>(std::memchr(begin, '\n', m_size - m_cursor));
			const size_t bytes = newline != nullptr ? static_cast<size_t>(newline - begin) : m_size - m_cursor;
			if (newline != nullptr && length == 0)
			{
				// Most kernel records fit in this refill. Parse their bounded view
				// directly instead of copying every address and pathname prefix.
				m_cursor += bytes + 1;
				if (ParseAfter(begin, bytes, minimum_end, result)) return true;
				continue;
			}
			const size_t copied = (std::min)(bytes, sizeof(prefix) - length);
			std::memcpy(prefix + length, begin, copied);
			length += copied;
			m_cursor += bytes;
			if (newline != nullptr)
			{
				++m_cursor;
				if (ParseAfter(prefix, length, minimum_end, result)) return true;
				length = 0;
			}
		}
	}

private:
	bool HasInput() const
	{
#if defined(__unix__) || defined(__APPLE__)
		if (m_descriptor >= 0 && m_descriptor_read != nullptr) return true;
#endif
		return m_file != nullptr;
	}
	size_t Refill(size_t bytes)
	{
#if defined(__unix__) || defined(__APPLE__)
		if (m_descriptor >= 0 && m_descriptor_read != nullptr)
		{
			std::ptrdiff_t count;
			do { count = m_descriptor_read(m_descriptor, m_buffer, bytes, m_read_context); }
			while (count < 0 && errno == EINTR);
			m_read_failed = count < 0 || static_cast<size_t>(count) > bytes;
			return m_read_failed ? 0 : static_cast<size_t>(count);
		}
#endif
		const size_t count = std::fread(m_buffer, 1, bytes, m_file);
		m_read_failed = std::ferror(m_file) != 0;
		return count;
	}
#if defined(__unix__) || defined(__APPLE__)
	static std::ptrdiff_t ReadDescriptor(int descriptor, void* buffer, size_t bytes, void*)
	{
		return ::read(descriptor, buffer, bytes);
	}
#endif
	static bool ParseAfter(const char* line, size_t length, uint64_t minimum_end, Record* result)
	{
		if (minimum_end != 0)
		{
			// A range query can discard earlier VMAs after decoding only their
			// end. This supplies no permission proof: every candidate covering
			// the requested address still passes the full record parser.
			const auto* separator = static_cast<const char*>(std::memchr(line, '-', (std::min)(length, size_t {17})));
			if (separator != nullptr)
			{
				uint64_t end = 0;
				const char* digits = separator + 1;
				const auto parsed = std::from_chars(digits, line + length, end, 16);
				if (parsed.ec == std::errc {} && parsed.ptr - digits <= 16 &&
				    parsed.ptr != line + length && (*parsed.ptr == ' ' || *parsed.ptr == '\t') && end <= minimum_end)
					return false;
			}
		}
		return ParseRecord(line, length, result);
	}

	std::FILE* m_file = nullptr;
#if defined(__unix__) || defined(__APPLE__)
	int m_descriptor = -1;
	DescriptorRead m_descriptor_read = nullptr;
	void* m_read_context = nullptr;
#endif
	char m_buffer[4096];
	size_t m_cursor = 0;
	size_t m_size = 0;
	bool m_finished = false;
	bool m_first_refill = true;
	bool m_read_failed = false;
};

} // namespace Lumen::Core::ProcMaps

#endif
