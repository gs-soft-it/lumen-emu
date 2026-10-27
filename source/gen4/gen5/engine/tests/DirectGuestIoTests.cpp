// SPDX-License-Identifier: GPL-2.0-only
// Real file and loopback I/O across the production nonidentity guest boundary.
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/singleton.h"
#include "common/threads.h"
#include "kernel/fileSystem.h"
#include "kernel/guestIoBuffer.h"
#include "kernel/module.h"
#include "kernel/pthread.h"
#include "libs/errno.h"
#include "libs/network.h"
#include "loader/elf.h"
#include "loader/guestExecution.h"
#include "loader/runtimeLinker.h"
#include "loader/timer.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <unistd.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>

namespace Libs::LibKernel {
int64_t KYTY_SYSV_ABI read(int descriptor, void* buffer, uint64_t size);
}

namespace {
namespace Kernel = Libs::LibKernel;
namespace Memory = Kernel::Memory;
namespace Fs = Kernel::FileSystem;
namespace Net = Libs::Network::Net;
constexpr uint64_t Page = 0x4000;
constexpr uint64_t Base = 0xfd0000000;
void Check(bool value, const char* message) {
    std::printf("DIRECT_GUEST_IO_TEST %s %s\n", value ? "PASS" : "FAIL", message);
    std::fflush(stdout);
    if (!value) std::abort();
}
void* Guest(uint64_t offset) { return reinterpret_cast<void*>(Base + offset); }
void Store(uint64_t offset, const void* data, size_t size) {
    Check(Memory::WriteGuestMemory(nullptr, Base + offset, data, size), "guest fixture copyin");
}
template<size_t Size> std::array<uint8_t, Size> Load(uint64_t offset) {
    std::array<uint8_t, Size> bytes {};
    Check(Memory::ReadGuestMemory(nullptr, Base + offset, bytes.data(), bytes.size()), "guest fixture copyout");
    return bytes;
}
void CheckResult(uint64_t offset, const uint8_t* expected, size_t size) {
    const auto bytes = Load<34>(offset - 1);
    Check(bytes.front() == 0xa5 && std::equal(expected, expected + size, bytes.begin() + 1) &&
          std::all_of(bytes.begin() + 1 + size, bytes.end(), [](uint8_t b) { return b == 0xa5; }),
          "actual result bytes only; destination guards and unread suffix preserved");
}
void FillDestination(uint64_t offset) {
    std::array<uint8_t, 34> bytes;
    bytes.fill(0xa5);
    Store(offset - 1, bytes.data(), bytes.size());
}
std::array<uint8_t, 16> LoopbackAddress() { return {16, 2, 0, 0, 127, 0, 0, 1}; }
std::array<int, 2> ConnectedSockets() {
    auto address = LoopbackAddress();
    const int listener = Net::Socket(2, 1, 0);
    Check(listener >= 0 && Net::Bind(listener, address.data(), address.size()) == 0 &&
          Net::Listen(listener, 1) == 0, "loopback listener");
    uint32_t length = address.size();
    Check(Net::Getsockname(listener, address.data(), &length) == 0, "ephemeral loopback endpoint");
    const int sender = Net::Socket(2, 1, 0);
    Check(sender >= 0 && Net::Connect(sender, address.data(), length) == 0, "loopback connection");
    const int receiver = Net::Accept(listener, nullptr, nullptr);
    Check(receiver >= 0 && Net::SocketClose(listener) == 0, "loopback accept");
    return {sender, receiver};
}
#include "DirectLoaderSentinelTests.inc"
#include "DirectLoaderDependencyTests.inc"
}

int main() {
    Common::InitializeThreads();
    Config::Initialize();
    Config::ConfigOptions options;
    options.printf_direction = Config::LogDirection::Silent;
    Config::Load(options);
    Log::Initialize();
    Loader::Timer::Start();
    Kernel::Initialize();
    Memory::Initialize();
    Kernel::PthreadInitSelfForMainThread();
    Fs::Initialize();
    Libs::Network::Initialize();
    Check(Net::NetInit() == 0, "network initialization");

    int64_t physical = -1;
    Check(Memory::KernelAllocateDirectMemory(0, Memory::KernelGetDirectMemorySize(), 2 * Page,
          Page, 12, &physical) == 0, "allocate shared direct backing");
    void* mapping = Guest(0);
    Check(Memory::KernelMapNamedDirectMemory(&mapping, 2 * Page, 0x33, 0x10, physical, Page,
          "GuestIoFixture") == 0 && mapping == Guest(0), "map fixed inaccessible guest arena");
    uint64_t host {};
    Check(Memory::IsGuestNonIdentity(Base, 2 * Page) &&
          Memory::TranslateGuestMemory(Base, 2 * Page, &host) && host != Base,
          "fixture requires an actual nonidentity alias");

    std::array<uint8_t, 16> payload;
    for (size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<uint8_t>(0x31 + i);
    Store(128, payload.data(), payload.size());
    const char* tmp = std::getenv("TMPDIR");
    std::string directory = std::string(tmp && *tmp ? tmp : "/tmp") + "/lumen-guest-io-XXXXXX";
    Check(mkdtemp(directory.data()) != nullptr, "isolated temporary filesystem");
    CheckLoaderSentinel(directory);
    CheckLoaderDependencies(directory);
    Fs::Mount(directory, "/guest-io");
    const int file = Fs::KernelOpen("/guest-io/data.bin", 0x602, 0600);
    Check(file >= 3, "open production guest file descriptor");
    Check(Fs::KernelWrite(file, Guest(128), payload.size()) == 16, "file write from guest alias");
    Check(Fs::KernelPwrite(file, Guest(128), 4, 8) == 4 && Fs::KernelLseek(file, 0, 1) == 16,
          "pwrite input staging preserves descriptor position");
    auto expected = payload;
    std::copy_n(payload.begin(), 4, expected.begin() + 8);
    FillDestination(512);
    Check(Fs::KernelPread(file, Guest(512), 32, 0) == 16 && Fs::KernelLseek(file, 0, 1) == 16,
          "pread commits short result and preserves descriptor position");
    CheckResult(512, expected.data(), expected.size());
    FillDestination(512);
    Check(Fs::KernelLseek(file, 0, 0) == 0 && Fs::KernelRead(file, Guest(512), 32) == 16 &&
          Fs::KernelLseek(file, 0, 1) == 16, "read stages output and advances descriptor position");
    CheckResult(512, expected.data(), expected.size());

    const std::array<Fs::KernelIovec, 2> iov {{{Guest(1024), 5}, {Guest(1088), 11}}};
    Store(4096, iov.data(), sizeof(iov));
    FillDestination(1024);
    FillDestination(1088);
    const auto* guest_iov = static_cast<const Fs::KernelIovec*>(Guest(4096));
    Check(Fs::KernelPreadv(file, guest_iov, iov.size(), 0) == 16, "preadv marshals nested guest pointers");
    CheckResult(1024, expected.data(), 5);
    CheckResult(1088, expected.data() + 5, 11);
    const auto saved_iov = Load<sizeof(iov)>(4096);
    Check(std::memcmp(saved_iov.data(), iov.data(), sizeof(iov)) == 0,
          "iovec descriptors retain guest addresses");
    Check(Fs::KernelPwritev(file, guest_iov, iov.size(), 16) == 16 &&
          Fs::KernelLseek(file, 0, 1) == 16, "pwritev stages all inputs and restores position");
    Check(Fs::KernelWritev(file, guest_iov, iov.size()) == 16 && Fs::KernelLseek(file, 0, 1) == 32,
          "writev stages nested inputs and advances position");
    FillDestination(512);
    Check(Fs::KernelPread(file, Guest(512), 16, 16) == 16, "read back vector-written file bytes");
    CheckResult(512, expected.data(), expected.size());

    const auto sockets = ConnectedSockets();
    FillDestination(512);
    Check(Net::Send(sockets[0], Guest(128), 16, 0) == 16 &&
          Net::Recv(sockets[1], Guest(512), 16, 0x40) == 16, "TCP send/recv across guest alias");
    CheckResult(512, payload.data(), payload.size());
    const int send_buffer_size = 16384;
    Store(1024, &send_buffer_size, sizeof(send_buffer_size));
    Check(Net::Setsockopt(sockets[0], 0xffff, 0x1001, Guest(1024), sizeof(int)) == 0,
          "setsockopt stages guest option input");
    FillDestination(512);
    uint32_t option_length = sizeof(int);
    Check(Net::Getsockopt(sockets[0], 0xffff, 0x1007, Guest(512), &option_length) == 0 &&
          option_length == sizeof(int), "getsockopt stages guest option output");
    const std::array<uint8_t, sizeof(int)> no_error {};
    CheckResult(512, no_error.data(), no_error.size());

    const int datagram_receiver = Net::Socket(2, 2, 0);
    const int datagram_sender = Net::Socket(2, 2, 0);
    auto address = LoopbackAddress();
    uint32_t address_size = address.size();
    Check(datagram_receiver >= 0 && datagram_sender >= 0 &&
          Net::Bind(datagram_receiver, address.data(), address.size()) == 0 &&
          Net::Getsockname(datagram_receiver, address.data(), &address_size) == 0, "UDP loopback endpoint");
    FillDestination(512);
    Check(Net::Sendto(datagram_sender, Guest(128), 16, 0, address.data(), address_size) == 16 &&
          Net::Recvfrom(datagram_receiver, Guest(512), 32, 0, nullptr, nullptr) == 16,
          "UDP sendto/recvfrom commit actual datagram length");
    CheckResult(512, payload.data(), payload.size());

    Store(Page + 128, payload.data(), payload.size());
    Check(Memory::KernelMprotect(Guest(Page), Page, 1) == 0, "read-only guest page");
    int input_pipe[2];
    const int original_stdin = dup(STDIN_FILENO);
    Check(original_stdin >= 0 && pipe(input_pipe) == 0 &&
          dup2(input_pipe[0], STDIN_FILENO) == STDIN_FILENO, "isolated process-local stdin pipe");
    close(input_pipe[0]);
    std::clearerr(stdin);
    const std::array<uint8_t, 5> input_line {'A', 0, 'B', '\n', 'Z'};
    Check(::write(input_pipe[1], input_line.data(), input_line.size()) == input_line.size() &&
          close(input_pipe[1]) == 0, "finite stdin fixture including embedded NUL");
    Check(Kernel::read(0, Guest(Page + 128), 16) == -1 &&
          *Libs::Posix::GetErrorAddr() == Libs::Posix::POSIX_EFAULT,
          "stdin rejects read-only output before consuming pipe bytes");
    FillDestination(512);
    Check(Kernel::read(0, Guest(512), 16) == 1, "stdin retains fgets strlen return convention");
    const std::array<uint8_t, 5> expected_line {'A', 0, 'B', '\n', 0};
    CheckResult(512, expected_line.data(), expected_line.size());
    FillDestination(512);
    Check(Kernel::read(0, Guest(512), 2) == 1, "stdin respects count minus one limit");
    const std::array<uint8_t, 2> final_line {'Z', 0};
    CheckResult(512, final_line.data(), final_line.size());
    FillDestination(512);
    Check(Kernel::read(0, Guest(512), 16) == 0, "stdin EOF returns zero");
    CheckResult(512, input_line.data(), 0);
    Check(dup2(original_stdin, STDIN_FILENO) == STDIN_FILENO && close(original_stdin) == 0,
          "restore process stdin");
    std::clearerr(stdin);
    Check(Fs::KernelPread(file, Guest(Page + 128), 16, 0) == Kernel::KERNEL_ERROR_EFAULT &&
          Fs::KernelLseek(file, 0, 1) == 32, "read-only output rejected before descriptor mutation");
    Check(Fs::KernelPwrite(file, Guest(Page + 128), 16, 0) == 16,
          "read-only source remains valid for file write");
    Check(Net::Send(sockets[0], Guest(Page + 128), 16, 0) == 16 &&
          Net::Recv(sockets[1], Guest(Page + 128), 16, 0) == -1 &&
          *Libs::Posix::GetErrorAddr() == Libs::Posix::POSIX_EFAULT,
          "socket output permissions rejected before consuming queued bytes");
    Check(Net::Recv(sockets[1], Guest(512), 16, 0x40) == 16 && Load<16>(512) == payload,
          "failed socket copyout leaves queued data available");
    Check(Memory::KernelMprotect(Guest(Page), Page, 0) == 0, "no-access guest page");
    Check(Fs::KernelPwrite(file, Guest(Page + 128), 16, 0) == Kernel::KERNEL_ERROR_EFAULT &&
          Net::Send(sockets[0], Guest(Page + 128), 16, 0) == -1 &&
          *Libs::Posix::GetErrorAddr() == Libs::Posix::POSIX_EFAULT, "no-access input rejected");
    Check(Memory::GuestIoBuffer::Validate(reinterpret_cast<void*>(UINT64_MAX - 3), 8, false) ==
          Memory::GuestIoError::InvalidAddress &&
          Memory::GuestIoBuffer::Validate(Guest(Page - 4), 8, false) == Memory::GuestIoError::InvalidAddress,
          "overflow and cross-protection ranges rejected");
    Check(Net::Recv(sockets[1], Guest(512), 1, 0x80) == -1 &&
          *Libs::Posix::GetErrorAddr() == Libs::Posix::POSIX_EAGAIN,
          "rejected send queues no data");

    for (int socket : {sockets[0], sockets[1], datagram_receiver, datagram_sender})
        Check(Net::SocketClose(socket) == 0, "close loopback socket");
    Check(Fs::KernelClose(file) == 0, "close guest file");
    Fs::Umount("/guest-io");
    Check(std::filesystem::remove_all(directory) == 2, "remove fixture files");
    Check(Memory::KernelCheckedReleaseDirectMemory(physical, 2 * Page) == 0, "release guest backing and alias");
    Libs::Network::Shutdown();
    Fs::Shutdown();
    Libs::Posix::ReleaseCurrentThreadError();
    Memory::Shutdown();
    std::puts("DIRECT_GUEST_IO_TEST PASS complete");
    return 0;
}
