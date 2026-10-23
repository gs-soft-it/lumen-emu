// SPDX-License-Identifier: GPL-2.0-or-later
#include "common/signal_context.h"
#include "core/signals.h"
#include "video_core/host_page_watch.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line=%d: %s\n", __LINE__, #x); std::abort(); } } while (0)

namespace {
std::uintptr_t watched_address{};
std::size_t watched_size{};
volatile sig_atomic_t read_faults{};
volatile sig_atomic_t write_faults{};
VideoCore::HostPageWatch* host_watch{};
static_assert(std::atomic<std::uint8_t>::is_always_lock_free);
std::array<std::atomic<std::uint8_t>, 4> watches{};
std::array<std::uint8_t, 4> permissions{};
volatile unsigned char* backing_alias{};
volatile sig_atomic_t readbacks{};
volatile sig_atomic_t invalidations{};
volatile sig_atomic_t protection_calls{};

std::size_t PageIndex(std::uintptr_t address) {
    return (address - watched_address) / VideoCore::HostPageWatch::GuestPageSize;
}

bool ApplyHostWatch() {
    using Watch = VideoCore::HostPageWatch;
    const auto allowed = host_watch->Permissions(watched_address, [](auto page) {
        const auto index = PageIndex(page);
        if (permissions[index] == Watch::Unavailable) return Watch::Unavailable;
        return static_cast<std::uint8_t>(permissions[index] & ~watches[index].load());
    });
    if (allowed == Watch::Unavailable) return false;
    protection_calls = protection_calls + 1;
    return mprotect(reinterpret_cast<void*>(watched_address), watched_size,
                    ((allowed & Watch::Read) ? PROT_READ : 0) |
                    ((allowed & Watch::Write) ? PROT_WRITE : 0)) == 0;
}

bool WatchAccess(void* context, void* fault_address) {
    const auto address = reinterpret_cast<std::uintptr_t>(fault_address);
    if (address < watched_address || address - watched_address >= watched_size) return false;
    const bool write = Common::IsWriteError(context);
    if (write) write_faults = write_faults + 1;
    else read_faults = read_faults + 1;
    if (host_watch) {
        using Watch = VideoCore::HostPageWatch;
        const auto blocked = host_watch->FaultPages(address, write, [](auto page) {
            return watches[PageIndex(page)].load();
        });
        if (blocked.count == 0) return false;
        for (std::size_t i = 0; i < blocked.count; ++i) {
            const auto index = PageIndex(blocked.pages[i]);
            if (watches[index].load() & Watch::Read) {
                // The renderer publishes to a separate backing alias, never
                // through the still-protected guest view.
                backing_alias[index * Watch::GuestPageSize] = 0x90 + index;
                readbacks = readbacks + 1;
            }
            if (write) {
                watches[index] = 0;
                invalidations = invalidations + 1;
            } else {
                watches[index].fetch_and(static_cast<std::uint8_t>(~Watch::Read));
            }
            if (!ApplyHostWatch()) _exit(92);
        }
        return true;
    }
    if (mprotect(reinterpret_cast<void*>(watched_address), watched_size,
                 write ? PROT_READ | PROT_WRITE : PROT_READ) != 0) _exit(91);
    return true;
}
} // namespace

int main() {
    alarm(10);
    const long host_page_size = sysconf(_SC_PAGESIZE);
    CHECK(host_page_size > 0);
    watched_size = static_cast<std::size_t>(host_page_size);
    void* mapping = mmap(nullptr, watched_size, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(mapping != MAP_FAILED);
    watched_address = reinterpret_cast<std::uintptr_t>(mapping);
    auto* bytes = static_cast<volatile unsigned char*>(mapping);
    bytes[0] = 0x31;
    bytes[watched_size - 1] = 0x72;

    // Use the production core dispatcher and the same access classification
    // and priority as GPU page tracking. No GPU or guest CPU is initialized.
    Core::Signals::Instance()->RegisterAccessViolationHandler(WatchAccess, 0);
    CHECK(mprotect(mapping, watched_size, PROT_READ) == 0);
    bytes[0] = 0x42;
    CHECK(write_faults == 1 && read_faults == 0);
    CHECK(bytes[0] == 0x42);

    CHECK(mprotect(mapping, watched_size, PROT_NONE) == 0);
    const auto first = bytes[0];
    CHECK(first == 0x42 && read_faults == 1 && write_faults == 1);
    // A read fault only restored reads. A following write must fault again.
    bytes[watched_size - 1] = 0x73;
    CHECK(write_faults == 2 && read_faults == 1);

    CHECK(mprotect(mapping, watched_size, PROT_NONE) == 0);
    const auto last = bytes[watched_size - 1];
    CHECK(last == 0x73 && read_faults == 2 && write_faults == 2);
    CHECK(!WatchAccess(nullptr, reinterpret_cast<void*>(watched_address + watched_size)));
    CHECK(munmap(mapping, watched_size) == 0);
    watched_address = 0;

    // Exercise two independent guest subpages sharing a real Darwin host
    // page, including faults in an otherwise untracked neighboring subpage.
    using Watch = VideoCore::HostPageWatch;
    Watch geometry{watched_size};
    if (watched_size == 4 * Watch::GuestPageSize) {
        char name[] = "/tmp/lumen-host-page-watch-XXXXXX";
        const int fd = mkstemp(name);
        CHECK(fd >= 0);
        CHECK(unlink(name) == 0);
        CHECK(ftruncate(fd, watched_size) == 0);
        mapping = mmap(nullptr, watched_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        void* alias = mmap(nullptr, watched_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        CHECK(mapping != MAP_FAILED && alias != MAP_FAILED);
        CHECK(close(fd) == 0);
        watched_address = reinterpret_cast<std::uintptr_t>(mapping);
        bytes = static_cast<volatile unsigned char*>(mapping);
        backing_alias = static_cast<volatile unsigned char*>(alias);
        permissions.fill(Watch::Read | Watch::Write);
        host_watch = &geometry;
        watches[0] = Watch::Read | Watch::Write;
        watches[2] = Watch::Read | Watch::Write;
        CHECK(ApplyHostWatch());
        const auto before_reads = read_faults;
        const auto neighbor = bytes[3 * Watch::GuestPageSize];
        CHECK(neighbor == 0 && read_faults == before_reads + 1 && readbacks == 2);
        CHECK(bytes[0] == 0x90 && bytes[2 * Watch::GuestPageSize] == 0x92);
        CHECK(watches[0] == Watch::Write && watches[2] == Watch::Write);
        const auto before_writes = write_faults;
        bytes[Watch::GuestPageSize] = 0x63;
        CHECK(write_faults == before_writes + 1 && invalidations == 2);
        CHECK(watches[0] == 0 && watches[2] == 0);

        // VM rights remain authoritative when a watch is removed. An absent
        // neighboring subpage rejects protection changes for the whole host
        // granule rather than restoring access to an unmapped address.
        permissions[2] = Watch::Read;
        CHECK(geometry.Permissions(watched_address, [](auto page) {
            return permissions[PageIndex(page)];
        }) == Watch::Read);
        CHECK(ApplyHostWatch());
        permissions[3] = Watch::Unavailable;
        const auto before_protect = protection_calls;
        CHECK(!ApplyHostWatch() && protection_calls == before_protect);
        CHECK(munmap(mapping, watched_size) == 0);
        CHECK(munmap(alias, watched_size) == 0);
        watched_address = 0;
        host_watch = nullptr;
    }
    alarm(0);
    std::printf("PageFaultWatchTest: PASS host_page_size=%zu read_faults=%d write_faults=%d "
                "subpage_readbacks=%d subpage_invalidations=%d "
                "scope=native-core-dispatch-and-host-page-policy gpu-cache=not-tested fex=not-tested\n",
                watched_size, static_cast<int>(read_faults), static_cast<int>(write_faults),
                static_cast<int>(readbacks), static_cast<int>(invalidations));
}
