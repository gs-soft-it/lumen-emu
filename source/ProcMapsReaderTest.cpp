#include "Lumen/Core/ProcMapsReader.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#if defined(__linux__)
#include <sys/mman.h>
#endif
#if defined(PROCMAP_QUERY) || defined(__ANDROID__)
#include <unistd.h>
#endif

// Standalone correctness gate; timing requires explicit --benchmark. This fixture does not access
// guest memory: every query consumes the current contents of a newly opened file.
// Example: clang++ -std=c++17 -O2 -I source/include source/ProcMapsReaderTest.cpp -o proc-maps-test
namespace {

namespace Maps = Lumen::Core::ProcMaps;
using File = std::unique_ptr<std::FILE, decltype(&std::fclose)>;
unsigned checks = 0;
unsigned failures = 0;
volatile uint64_t benchmark_sink = 0;

void Check(bool condition, const char* description)
{
    ++checks;
    if (!condition)
    {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", description);
    }
}

bool Same(const Maps::Record& left, const Maps::Record& right)
{
    return left.begin == right.begin && left.end == right.end &&
           left.readable == right.readable && left.writable == right.writable;
}

bool LegacyParse(const char* line, Maps::Record* record)
{
    uint64_t begin = 0, end = 0;
    char protection[5] {};
    if (std::sscanf(line, "%" SCNx64 "-%" SCNx64 " %4s", &begin, &end, protection) != 3) return false;
    *record = {begin, end, protection[0] == 'r', protection[1] == 'w'};
    return true;
}

bool LegacyNext(std::FILE* file, Maps::Record* record)
{
    char line[1024] {};
    while (std::fgets(line, sizeof(line), file) != nullptr)
        if (LegacyParse(line, record)) return true;
    return false;
}

File FixtureFile(const std::string& contents)
{
    File file(std::tmpfile(), &std::fclose);
#if defined(__ANDROID__)
    // Bionic tmpfile uses /data/local/tmp, which an app uid cannot write.
    // run-as starts in the app's writable data directory. Keep the fixture
    // unnamed and private there so the same gate works in the app policy.
    if (file == nullptr)
    {
        char path[] = ".lumen-proc-maps-XXXXXX";
        const int descriptor = ::mkstemp(path);
        if (descriptor >= 0)
        {
            ::unlink(path);
            file.reset(::fdopen(descriptor, "w+b"));
            if (file == nullptr) ::close(descriptor);
        }
    }
#endif
    if (file != nullptr && (std::fwrite(contents.data(), 1, contents.size(), file.get()) != contents.size() ||
                           std::fseek(file.get(), 0, SEEK_SET) != 0)) file.reset();
    return file;
}

std::string KernelLine(uint64_t begin, uint64_t end, const char* permissions, const std::string& path = {})
{
    char prefix[128] {};
    std::snprintf(prefix, sizeof(prefix), "%016" PRIx64 "-%016" PRIx64 " %s 00000000 00:00 0 ",
                  begin, end, permissions);
    return std::string(prefix) + path + '\n';
}

std::vector<Maps::Record> ReadRecords(Maps::Reader& reader)
{
    Maps::Record record {};
    std::vector<Maps::Record> records;
    while (reader.Next(&record)) records.push_back(record);
    return records;
}

std::vector<Maps::Record> ReadRecords(std::FILE* file)
{
    Maps::Reader reader(file);
    return ReadRecords(reader);
}

void CheckDescriptorRecords(const std::string& contents, const std::vector<Maps::Record>& expected)
{
#if defined(__unix__) || defined(__APPLE__)
    // A separate stream supplies the descriptor before any buffered read.
    // Do not mix stdio offsets and descriptor reads on one active reader.
    File file = FixtureFile(contents);
    Check(file != nullptr, "descriptor fixture available");
    if (file == nullptr) return;
    Maps::Reader reader(::fileno(file.get()));
    const auto actual = ReadRecords(reader);
    Check(actual.size() == expected.size() && std::equal(actual.begin(), actual.end(), expected.begin(), Same),
          "direct descriptor records equal the complete stdio records");
#else
    (void)contents;
    (void)expected;
#endif
}

void TestDescriptorTransport()
{
#if defined(__unix__) || defined(__APPLE__)
    struct Source
    {
        std::string bytes;
        std::vector<int> script;
        size_t offset = 0, step = 0, chunk = 7;
        std::vector<size_t> requests;
        static std::ptrdiff_t Read(int, void* output, size_t capacity, void* context)
        {
            auto& source = *static_cast<Source*>(context);
            source.requests.push_back(capacity);
            const int action = source.step < source.script.size() ? source.script[source.step++] :
                static_cast<int>(source.chunk);
            if (action < 0) { errno = -action; return -1; }
            const size_t count = std::min({capacity, static_cast<size_t>(action), source.bytes.size() - source.offset});
            std::memcpy(output, source.bytes.data() + source.offset, count);
            source.offset += count;
            return static_cast<std::ptrdiff_t>(count);
        }
    };
    const std::string text = KernelLine(0x1000, 0x2000, "r--p", std::string(5000, 'x')) +
        KernelLine(0x3000, 0x4000, "rw-p") + "5000-6000 ---p";
    File reference = FixtureFile(text);
    Check(reference != nullptr, "transport reference available");
    if (reference == nullptr) return;
    const auto expected = ReadRecords(reference.get());
    CheckDescriptorRecords(text, expected);
    for (const size_t chunk: {size_t(1), size_t(7), size_t(511), size_t(4096)})
    {
        Source source {text, {-EINTR, -EINTR, 13, -EINTR}, 0, 0, chunk, {}};
        Maps::Reader reader(0, Source::Read, &source);
        const auto actual = ReadRecords(reader);
        Check(actual.size() == expected.size() && std::equal(actual.begin(), actual.end(), expected.begin(), Same) &&
              source.offset == text.size(), "EINTR and short reads preserve every complete record and final EOF record");
        Check(source.requests.size() > 3 && source.requests[0] == 512 && source.requests[1] == 512 &&
              source.requests[2] == 512 && source.requests[3] == 4096,
              "interrupted first read retries the same bounded request");
    }
    for (const std::string& prefix: {std::string("1000-2000 rw-p"), std::string("1000-20")})
    {
        Source source {prefix, {static_cast<int>(prefix.size()), -EIO}, 0, 0, 7, {}};
        Maps::Reader reader(0, Source::Read, &source);
        Maps::Record record {};
        Check(!reader.Next(&record) && !reader.Next(&record) && source.requests.size() == 2,
              "read error never promotes a partial physical line to EOF or retries another transport");
    }
    for (const int error: {EBADF, EACCES, EIO})
    {
        Source source {text, {-error}, 0, 0, 7, {}};
        Maps::Reader reader(0, Source::Read, &source);
        Maps::Record record {};
        Check(!reader.Next(&record) && !reader.Next(&record) && source.requests.size() == 1,
              "terminal descriptor errors remain denied without repeated reads");
    }
    Source empty {text, {0}, 0, 0, 7, {}};
    Maps::Reader empty_reader(0, Source::Read, &empty);
    Maps::Record record {};
    Check(!empty_reader.Next(&record) && !empty_reader.Next(&record) && empty.offset == 0 && empty.requests.size() == 1,
          "clean empty EOF is terminal and consumes no hidden bytes");
    Source partial {"1000-20", {}, 0, 0, 1, {}};
    Maps::Reader partial_reader(0, Source::Read, &partial);
    Check(!partial_reader.Next(&record), "truncated mapping prefix at clean EOF remains invalid");
    Maps::Reader invalid(-1);
    Check(!invalid.Next(&record), "invalid descriptor has no readable input");
#endif
}

void TestRecords()
{
    const char* const valid[] = {
        "0-1 ---p 00000000 00:00 0",
        "1000-2000 r--p 00000000 00:00 0 [anon]",
        "1000-2000 -w-p 00000000 00:00 0",
        "1000-2000 --xp 00000000 00:00 0",
        "1000-2000 rwxp 00000000 00:00 0 /file (deleted)",
        "1000-2000 rw-s 00000000 00:00 0 /a path with spaces",
        "0000000000001000-0000000000002000 r-xp 00000000 00:00 0 [vdso]\n",
        "ABCDEF-ABCFFF r--p 00000000 00:00 0",
        "fffffffffffffffe-ffffffffffffffff rw-p 00000000 00:00 0",
    };
    for (const char* line: valid)
    {
        Maps::Record old_record {}, new_record {};
        Check(LegacyParse(line, &old_record) && Maps::ParseRecord(line, std::strlen(line), &new_record) &&
              Same(old_record, new_record), "valid kernel record matches sscanf");
    }

    const char* const invalid[] = {
        "", "\n", "1000", "1000-", "1000-2000", "1000-2000 r", "1000-2000 rw-",
        "1000-1000 rw-p", "2000-1000 rw-p", "-1000-2000 rw-p", "+1000-2000 rw-p",
        "10000000000000000-20000000000000000 rw-p", "1-10000000000000000 rw-p",
        "1000-2000 qw-p", "1000-2000 rq-p", "1000-2000 rwqp", "1000-2000 rw-q",
        "1000-2000 rw-pa", "0x1000-0x2000 rw-p", "1000:2000 rw-p",
    };
    for (const char* line: invalid)
    {
        Maps::Record record {};
        Check(!Maps::ParseRecord(line, std::strlen(line), &record), "malformed record denied");
    }
    Maps::Record record {};
    Check(!Maps::ParseRecord(nullptr, 1, &record), "null input denied");
    Check(!Maps::ParseRecord(valid[1], std::strlen(valid[1]), nullptr), "null output denied");
    Check(!Maps::ParseRecord(valid[1], 4, &record), "provided length bounds parsing");
    const std::array<char, 16> no_terminator {{'1', '0', '0', '0', '-', '2', '0', '0', '0', ' ', 'r', 'w', '-', 'p', ' ', '0'}};
    Check(Maps::ParseRecord(no_terminator.data(), no_terminator.size(), &record) &&
          record.begin == 0x1000 && record.end == 0x2000 && record.readable && record.writable,
          "non-NUL-terminated bounded prefix parses");
}

void TestReaderBoundaries()
{
    Maps::Record unused {};
    Maps::Reader null_reader(nullptr);
    Check(!null_reader.Next(&unused), "null input stream denied");
    File empty = FixtureFile("");
    Check(empty != nullptr, "empty stream fixture available");
    if (empty != nullptr)
    {
        Maps::Reader reader(empty.get());
        Check(!reader.Next(&unused) && !reader.Next(&unused), "empty stream and repeated EOF denied");
    }

    // Exercise short first refills, subsequent 4 KiB refills and the span
    // around a 16 KiB host page. Cover both ordinary and short-first readers.
    // Each address/permission position crosses the boundary at some offset.
    for (const size_t boundary: {size_t(512), size_t(1024), size_t(4096), size_t(4608),
                                size_t(5120), size_t(16384), size_t(16896), size_t(17408)})
    {
        for (int displacement = -48; displacement <= 8; ++displacement)
        {
            const size_t target_start = static_cast<size_t>(static_cast<int>(boundary) + displacement);
            std::string contents = KernelLine(0x1000, 0x2000, "r--p");
            contents.insert(contents.size() - 1, target_start - contents.size(), 'a');
            contents += KernelLine(0x2000, 0x3000, "rw-p", std::string(boundary + 2000, 'b'));
            contents += "malformed " + std::string(boundary + 3, 'c') + '\n';
            contents += KernelLine(0x4000, 0x5000, "---p");
            contents.pop_back(); // A final record without newline is still complete.
            File file = FixtureFile(contents);
            Check(file != nullptr, "boundary fixture file available");
            if (file == nullptr) continue;
            const auto records = ReadRecords(file.get());
            CheckDescriptorRecords(contents, records);
            Check(records.size() == 3 && Same(records[0], {0x1000, 0x2000, true, false}) &&
                  Same(records[1], {0x2000, 0x3000, true, true}) &&
                  Same(records[2], {0x4000, 0x5000, false, false}),
                  "split records and long paths preserve complete physical lines");
            Check(std::ferror(file.get()) == 0, "boundary fixture stream has no read error");
            std::rewind(file.get());
            Maps::Reader filtered(file.get());
            Maps::Record record;
            Check(filtered.Next(&record, 0x3000) && Same(record, {0x4000, 0x5000, false, false}) &&
                  !filtered.Next(&record, 0x3000), "address filter skips complete earlier VMAs across refill boundaries");
        }
    }

    // Address-looking path text must never become another mapping when fgets
    // would have split the same physical line into several 1023-byte chunks.
    std::string contents = KernelLine(0x1000, 0x2000, "r--p");
    contents.insert(contents.size() - 1, 1023 - (contents.size() - 1), 'a');
    contents.insert(contents.size() - 1, "3000-4000 rw-p 00000000 00:00 0 false-mapping");
    contents += KernelLine(0x5000, 0x6000, "rw-p");
    File file = FixtureFile(contents);
    Check(file != nullptr, "long path injection fixture file available");
    if (file != nullptr)
    {
        const auto records = ReadRecords(file.get());
        CheckDescriptorRecords(contents, records);
        Check(records.size() == 2 && records[0].begin == 0x1000 && records[1].begin == 0x5000,
              "address-like long pathname is not a mapping");
    }
}

template<class Next>
bool ScanRange(Next&& next, uint64_t address, uint64_t end, bool require_read, bool require_write)
{
    Maps::Record record {};
    while (address < end && next(&record))
    {
        if (record.end <= address) continue;
        if (record.begin > address || (require_read && !record.readable) || (require_write && !record.writable)) break;
        address = record.end < end ? record.end : end;
    }
    return address == end;
}

void TestRangeCoverage()
{
    const std::string contents = KernelLine(0x1000, 0x2000, "rw-p") + KernelLine(0x2000, 0x3000, "r--p") +
        KernelLine(0x3000, 0x4000, "rw-p") + KernelLine(0x5000, 0x6000, "rw-p") +
        KernelLine(0x6000, 0x7000, "---p");
    struct Case { uint64_t begin, end; bool read, write, expected; };
    const Case cases[] = {
        {0x1000, 0x1008, true, false, true}, {0x1ffc, 0x2004, true, false, true},
        {0x1ffc, 0x2004, false, true, false}, {0x1000, 0x4000, true, false, true},
        {0x3000, 0x4000, true, true, true}, {0x3ffc, 0x5004, true, false, false},
        {0x4000, 0x4008, true, false, false}, {0xfff, 0x1001, true, false, false},
        {0x5ffc, 0x6004, true, false, false}, {0x7000, 0x7001, true, false, false},
    };
    for (const auto& test: cases)
    {
        File file = FixtureFile(contents);
        Check(file != nullptr, "range fixture file available");
        if (file == nullptr) continue;
        Maps::Reader reader(file.get());
        const bool actual = ScanRange([&](Maps::Record* record) { return reader.Next(record, test.begin); },
            test.begin, test.end, test.read, test.write);
        std::rewind(file.get());
        const bool previous = ScanRange([&](Maps::Record* record) { return LegacyNext(file.get(), record); },
            test.begin, test.end, test.read, test.write);
        Check(actual == test.expected && actual == previous, "cross-VMA rights and holes match reference");
#if defined(__unix__) || defined(__APPLE__)
        File direct_file = FixtureFile(contents);
        Check(direct_file != nullptr, "direct range fixture available");
        if (direct_file != nullptr)
        {
            Maps::Reader direct(::fileno(direct_file.get()));
            Check(ScanRange([&](Maps::Record* record) { return direct.Next(record, test.begin); },
                  test.begin, test.end, test.read, test.write) == actual,
                  "descriptor range coverage preserves permission and hole denial");
        }
#endif
    }
}

void TestKernelRangeQuery(bool direct_text = false)
{
#if defined(__linux__)
    const long host_page = ::sysconf(_SC_PAGESIZE);
    Check(host_page > 0, "kernel query host page size available");
    if (host_page <= 0) return;
    const size_t page = static_cast<size_t>(host_page);
    void* const mapping = ::mmap(nullptr, page * 4, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    Check(mapping != MAP_FAILED, "kernel query mapping available");
    if (mapping == MAP_FAILED) return;
    const uint64_t begin = reinterpret_cast<uintptr_t>(mapping);
#if defined(PROCMAP_QUERY)
    constexpr bool ioctl_compiled = true;
    bool native_available = !direct_text;
    int unavailable_error = direct_text ? EOPNOTSUPP : 0;
#else
    constexpr bool ioctl_compiled = false;
    bool native_available = false;
    int unavailable_error = EOPNOTSUPP;
#endif
    // Every query opens the live maps file, just like Core. Do not preserve
    // a mapping or permissions answer across mprotect or munmap.
    const auto query = [&](uint64_t address, uint64_t end, uint32_t rights,
                           uint32_t* observed = nullptr, int* error = nullptr) {
        File maps(std::fopen("/proc/self/maps", "r"), &std::fclose);
        Check(maps != nullptr, "kernel query fresh maps file available");
        uint32_t seen = 0;
        int failure = 0;
        auto result = Maps::RangeQueryResult::Denied;
        if (maps != nullptr)
        {
            if (native_available)
            {
#if defined(PROCMAP_QUERY)
                result = Maps::QueryRange(::fileno(maps.get()), address, end, rights, &seen, &failure);
#endif
            }
            else
            {
                // Keep querying fresh permissions after the ioctl capability
                // was disabled, including the revocations below.
                result = Maps::RangeQueryResult::Unsupported;
                failure = unavailable_error;
            }
        }
        if (result == Maps::RangeQueryResult::Unsupported)
        {
            native_available = false;
            unavailable_error = failure;
            failure = 0;
            Maps::Reader reader = direct_text ? Maps::Reader(::fileno(maps.get())) : Maps::Reader(maps.get());
            Maps::Record record;
            uint64_t cursor = address;
            while (cursor < end && reader.Next(&record, cursor))
            {
                if (record.end <= cursor) continue;
                seen = (record.readable ? 1U : 0U) | (record.writable ? 2U : 0U);
                if (record.begin > cursor || (seen & rights) != rights) break;
                cursor = std::min(end, record.end);
            }
            result = cursor == end ? Maps::RangeQueryResult::Allowed : Maps::RangeQueryResult::Denied;
        }
        if (observed != nullptr) *observed = seen;
        if (error != nullptr) *error = failure;
        std::printf("PROCMAP_CASE begin=%#" PRIx64 " end=%#" PRIx64
            " rights=%u result=%u observed=%u errno=%d\n", address, end,
            rights, static_cast<unsigned>(result), seen, failure);
        return result;
    };
    uint32_t observed = 0;
    int error = 0;
    const auto initial = query(begin, begin + 8, 3, &observed, &error);
    Check(initial == Maps::RangeQueryResult::Allowed && observed == 3 && error == 0,
          "kernel query observes initial read/write rights");
    Check(::mprotect(reinterpret_cast<void*>(begin + page), page, PROT_READ) == 0,
          "kernel query read-only VMA split established");
    Check(::mprotect(reinterpret_cast<void*>(begin + page * 3), page, PROT_NONE) == 0,
          "kernel query NoAccess VMA split established");
    Check(query(begin + page - 4, begin + page + 4, 1) == Maps::RangeQueryResult::Allowed,
          "kernel query reads across adjacent RW and R VMAs");
    Check(query(begin + page - 4, begin + page + 4, 2) == Maps::RangeQueryResult::Denied,
          "kernel query denies write across adjacent RW and R VMAs");
    Check(query(begin, begin + page * 3, 1) == Maps::RangeQueryResult::Allowed,
          "kernel query covers several readable VMAs");
    Check(query(begin + page * 3 - 4, begin + page * 3 + 4, 1) == Maps::RangeQueryResult::Denied,
          "kernel query denies read into NoAccess VMA");
    Check(query(begin + page, begin + page + 1, 2, &observed, &error) == Maps::RangeQueryResult::Denied &&
          observed == 1 && error == 0, "kernel query observes external write revocation");
    Check(::mprotect(reinterpret_cast<void*>(begin + page), page, PROT_READ | PROT_WRITE) == 0,
          "kernel query write rights restored");
    Check(query(begin + page, begin + page + 1, 2) == Maps::RangeQueryResult::Allowed,
          "kernel query observes restored write rights");
    Check(::munmap(reinterpret_cast<void*>(begin + page), page) == 0,
          "kernel query unmapped hole established");
    Check(query(begin + page - 4, begin + page * 2 + 4, 1) == Maps::RangeQueryResult::Denied,
          "kernel query denies a hole before the next readable VMA");
    Check(query(begin + page, begin + page + 1, 1, &observed, &error) == Maps::RangeQueryResult::Denied &&
          (!native_available || (observed == 0 && error == ENOENT)),
          "kernel query exact address does not skip an unmapped hole");
#if defined(PROCMAP_QUERY)
    Check(Maps::QueryRange(-1, begin, begin, 1, nullptr, nullptr) == Maps::RangeQueryResult::Denied &&
          Maps::QueryRange(-1, begin + 1, begin, 1, nullptr, nullptr) == Maps::RangeQueryResult::Denied &&
          Maps::QueryRange(-1, begin, begin + 1, 0, nullptr, nullptr) == Maps::RangeQueryResult::Denied &&
          Maps::QueryRange(-1, begin, begin + 1, 4, nullptr, nullptr) == Maps::RangeQueryResult::Denied,
          "kernel query invalid ranges and rights denied");
    Check(Maps::QueryRange(-1, begin, begin + 1, 1, &observed, &error) == Maps::RangeQueryResult::Denied &&
          error == EBADF, "kernel query invalid descriptor is denial rather than fallback");
#endif
    Check(::munmap(mapping, page * 4) == 0, "kernel query fixture released");
    std::printf("PROCMAP_QUERY direct_text=%u compiled=%u supported=%u unavailable_errno=%d native_runtime=%s"
                " fresh_text_fallback=%s contract=%s host_page=%zu\n",
                direct_text ? 1U : 0U, ioctl_compiled ? 1U : 0U, native_available ? 1U : 0U, unavailable_error,
                native_available ? (failures == 0 ? "PASS" : "FAIL") : "NOT_EXERCISED",
                native_available ? "NOT_EXERCISED" : (failures == 0 ? "PASS" : "FAIL"),
                failures == 0 ? "PASS" : "FAIL", page);
#else
    std::printf("PROCMAP_QUERY direct_text=%u compiled=0 supported=0 runtime=NOT_EXERCISED\n", direct_text ? 1U : 0U);
#endif
}

bool LoadMapsFile(const char* path, std::string* contents)
{
    File file(std::fopen(path, "r"), &std::fclose);
    if (file == nullptr) return false;
    char buffer[16 * 1024];
    for (size_t count = 0; (count = std::fread(buffer, 1, sizeof(buffer), file.get())) != 0;)
        contents->append(buffer, count);
    return std::ferror(file.get()) == 0;
}

void TestCapturedRecords(const std::string& contents)
{
    size_t offset = 0;
    size_t valid_count = 0;
    std::vector<Maps::Record> expected;
    while (offset < contents.size())
    {
        const size_t newline = contents.find('\n', offset);
        const size_t end = newline == std::string::npos ? contents.size() : newline;
        const std::string line = contents.substr(offset, end - offset);
        Maps::Record previous {}, current {};
        const bool old_valid = LegacyParse(line.c_str(), &previous);
        const bool new_valid = Maps::ParseRecord(line.data(), line.size(), &current);
        Check(old_valid && new_valid && Same(previous, current), "captured kernel line matches sscanf");
        if (new_valid) { expected.push_back(current); ++valid_count; }
        offset = end < contents.size() ? end + 1 : end;
    }
    Check(valid_count != 0, "captured map has records");
    File file = FixtureFile(contents);
    Check(file != nullptr, "captured map replay file available");
    if (file != nullptr)
    {
        const auto actual = ReadRecords(file.get());
        Check(actual.size() == expected.size() && std::equal(actual.begin(), actual.end(), expected.begin(), Same),
              "streaming capture replay matches full-line reference");
        if (!expected.empty()) for (const size_t index: {size_t {0}, expected.size() / 2, expected.size() - 1})
        {
            std::rewind(file.get());
            Maps::Reader reader(file.get());
            Maps::Record record;
            Check(reader.Next(&record, expected[index].begin) && Same(record, expected[index]),
                  "address-filtered captured map matches the full-line reference");
        }
    }
}

struct BenchmarkQuery { const char* label; uint64_t begin, end; };

bool FileQuery(const char* path, const BenchmarkQuery& query, bool candidate)
{
    File file(std::fopen(path, "r"), &std::fclose);
    if (file == nullptr) return false;
    if (candidate)
    {
#if defined(PROCMAP_QUERY)
        // Match Core's process transport fallback without caching any VMA
        // answer. Otherwise a blocked ioctl would dominate the benchmark
        // through repeated SELinux audit messages.
        static bool query_unavailable = false;
        if (!query_unavailable)
        {
            const auto query_result = Maps::QueryRange(::fileno(file.get()), query.begin, query.end,
                                                      1, nullptr, nullptr);
            if (query_result != Maps::RangeQueryResult::Unsupported)
                return query_result == Maps::RangeQueryResult::Allowed;
            query_unavailable = true;
        }
#endif
        Maps::Reader reader(file.get());
        return ScanRange([&](Maps::Record* record) { return reader.Next(record, query.begin); }, query.begin, query.end, true, false);
    }
    return ScanRange([&](Maps::Record* record) { return LegacyNext(file.get(), record); },
        query.begin, query.end, true, false);
}

bool ReplayQuery(std::FILE* file, const BenchmarkQuery& query, bool candidate)
{
    std::rewind(file);
    if (candidate)
    {
        Maps::Reader reader(file);
        return ScanRange([&](Maps::Record* record) { return reader.Next(record, query.begin); }, query.begin, query.end, true, false);
    }
    return ScanRange([&](Maps::Record* record) { return LegacyNext(file, record); },
        query.begin, query.end, true, false);
}

template<class Query>
double TimeQueries(Query&& query, uint64_t iterations, uint64_t* successes)
{
    uint64_t count = 0;
    const auto start = std::chrono::steady_clock::now();
    for (uint64_t index = 0; index < iterations; ++index) count += query() ? 1 : 0;
    const auto stop = std::chrono::steady_clock::now();
    benchmark_sink = benchmark_sink + count;
    *successes = count;
    return std::chrono::duration<double, std::nano>(stop - start).count() / static_cast<double>(iterations);
}

void Benchmark(const char* path, const std::string& contents, uint64_t iterations)
{
    File replay = FixtureFile(contents);
    if (replay == nullptr) { Check(false, "benchmark replay file available"); return; }
    const auto records = ReadRecords(replay.get());
    const auto low = std::find_if(records.begin(), records.end(), [](const auto& record) { return record.readable; });
    const auto high = std::find_if(records.rbegin(), records.rend(), [](const auto& record) { return record.readable; });
    if (low == records.end() || high == records.rend()) { Check(false, "benchmark has readable ranges"); return; }
    const BenchmarkQuery queries[] = {{"low", low->begin, low->begin + std::min<uint64_t>(8, low->end - low->begin)},
        {"high", high->begin, high->begin + std::min<uint64_t>(8, high->end - high->begin)}};
    for (const auto& query: queries)
    {
        // Keep correctness outside timing. With a live proc file each operation
        // still opens a fresh map; the fixture never reuses physical permissions.
        const auto invoke = [&](bool candidate) {
            return path != nullptr ? FileQuery(path, query, candidate) : ReplayQuery(replay.get(), query, candidate);
        };
        Check(invoke(false) && invoke(true), "benchmark query correct before timing");
        if (failures != 0) return;
        std::array<double, 3> previous {}, candidate {};
        for (size_t run = 0; run < previous.size(); ++run)
        {
            uint64_t old_count = 0, new_count = 0;
            if ((run & 1u) == 0)
            {
                previous[run] = TimeQueries([&] { return invoke(false); }, iterations, &old_count);
                candidate[run] = TimeQueries([&] { return invoke(true); }, iterations, &new_count);
            }
            else
            {
                candidate[run] = TimeQueries([&] { return invoke(true); }, iterations, &new_count);
                previous[run] = TimeQueries([&] { return invoke(false); }, iterations, &old_count);
            }
            Check(old_count == iterations && new_count == iterations, "benchmark query remains correct during timing");
            std::printf("BENCH_RUN range=%s run=%zu order=%s previous_ns=%.3f candidate_ns=%.3f previous_successes=%" PRIu64 " candidate_successes=%" PRIu64 "\n",
                query.label, run + 1, (run & 1u) == 0 ? "B-C" : "C-B", previous[run], candidate[run], old_count, new_count);
        }
        std::sort(previous.begin(), previous.end());
        std::sort(candidate.begin(), candidate.end());
        std::printf("BENCH range=%s mode=%s iterations=%" PRIu64 " runs=3 previous_median_ns=%.3f candidate_median_ns=%.3f ratio=%.4f reduction_percent=%.3f\n",
            query.label, path != nullptr ? "fresh-fopen" : "synthetic-rewind", iterations,
            previous[1], candidate[1], previous[1] / candidate[1], 100.0 * (1.0 - candidate[1] / previous[1]));
    }
}

} // namespace

int main(int argc, char** argv)
{
    const char* path = nullptr;
    bool benchmark = false;
    bool correctness_only = false;
#if defined(__ANDROID__)
    path = "/proc/self/maps";
#endif
    uint64_t iterations = 200;
    for (int index = 1; index < argc; ++index)
    {
        if (std::strcmp(argv[index], "--maps") == 0 && index + 1 < argc) path = argv[++index];
        else if (std::strcmp(argv[index], "--benchmark") == 0) benchmark = true;
        else if (std::strcmp(argv[index], "--correctness-only") == 0) correctness_only = true;
        else if (std::strcmp(argv[index], "--iterations") == 0 && index + 1 < argc)
        {
            const char* argument = argv[++index];
            char* end = nullptr;
            errno = 0;
            const unsigned long long parsed = std::strtoull(argument, &end, 10);
            if (argument[0] == '-' || end == argument || *end != '\0' || errno != 0 || parsed == 0 || parsed > 1000000)
            {
                std::fprintf(stderr, "iterations must be between 1 and 1000000\n");
                return 2;
            }
            iterations = static_cast<uint64_t>(parsed);
        }
        else
        {
            std::fprintf(stderr, "Usage: %s [--maps path] [--correctness-only | --benchmark] [--iterations 1..1000000]\n", argv[0]);
            return 2;
        }
    }
    if (benchmark && correctness_only)
    {
        std::fprintf(stderr, "--benchmark and --correctness-only are mutually exclusive\n");
        return 2;
    }
    TestRecords();
    TestDescriptorTransport();
    TestReaderBoundaries();
    TestRangeCoverage();
    TestKernelRangeQuery();
    TestKernelRangeQuery(true);
    std::string contents;
    if (path != nullptr)
    {
        if (!LoadMapsFile(path, &contents)) { std::perror("maps file"); return 2; }
    }
    else
    {
        for (uint64_t index = 0; index < 4000; ++index)
        {
            const uint64_t begin = 0x10000000 + index * 0x2000;
            contents += KernelLine(begin, begin + 0x1000, "rw-p", "[synthetic-owned-range]");
        }
    }
    TestCapturedRecords(contents);
    std::printf("CORRECTNESS %s checks=%u failures=%u map_bytes=%zu\n", failures == 0 ? "PASS" : "FAIL", checks, failures, contents.size());
    std::fflush(stdout);
    if (failures != 0) return 1;
    if (benchmark) Benchmark(path, contents, iterations);
    std::printf("RESULT %s checks=%u failures=%u\n", failures == 0 ? "PASS" : "FAIL", checks, failures);
    return failures == 0 ? 0 : 1;
}
