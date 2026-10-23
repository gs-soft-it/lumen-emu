// Lumen, 2026-10-04. SPDX-License-Identifier: GPL-2.0-only
// Included by fexRuntimeTests.cpp after its common fixture helpers.
namespace {
namespace NetworkWaitNet = Libs::Network::Net;
constexpr std::uint64_t network_wait_page = 0x4000;
constexpr std::uint64_t network_wait_guard = 0x6e65747761697431ULL;
constexpr std::array<std::uint8_t, 8> network_wait_payload {0x31,0x07,0x92,0xaa,0x4d,0xfe,0x18,0x63};
std::atomic<bool> network_wait_started {};
std::atomic<bool> network_wait_entered {};
std::atomic<bool> network_wait_finished {};
std::atomic<bool> network_wait_release {};
std::atomic<bool> network_wait_sent {};
std::atomic<std::int64_t> network_wait_deadline {};
std::atomic<std::int64_t> network_wait_result {};
std::atomic<int> network_wait_error {};
int network_wait_socket = -1;
std::uint64_t network_wait_buffer {};
std::uint64_t network_wait_size {};
std::uint64_t network_wait_address {};
std::uint64_t network_wait_length {};
int network_wait_flags {};

std::uint64_t NativeNetworkReceive(std::uint64_t) {
    if (!network_wait_started.load(std::memory_order_acquire)) {
        return SteadyNanoseconds() < network_wait_deadline.load(std::memory_order_acquire)
            ? 0 : UINT64_MAX;
    }
    *Libs::Posix::GetErrorAddr() = Libs::Posix::POSIX_EINVAL;
    // Notification and actual receive share one counted HLE callback. The
    // exclusive epoch cannot pass between them unless Recvfrom donates it.
    network_wait_entered.store(true, std::memory_order_release);
    const auto result = NetworkWaitNet::Recvfrom(network_wait_socket,
        reinterpret_cast<void*>(network_wait_buffer), network_wait_size, network_wait_flags,
        reinterpret_cast<void*>(network_wait_address),
        reinterpret_cast<std::uint32_t*>(network_wait_length));
    network_wait_result.store(result, std::memory_order_release);
    network_wait_error.store(*Libs::Posix::GetErrorAddr(), std::memory_order_release);
    network_wait_finished.store(true, std::memory_order_release);
    return 1;
}

std::uint64_t NativeNetworkNestedReceive(std::uint64_t target) {
    std::uint64_t result {};
    const bool invoked = Invoke(target, nullptr, 0, &result) && result == 1;
    // A new counted borrow after the nested refusal still belongs to this
    // outer HLE frame; the receive must never discard its reader ownership.
    const bool retained = LumenAndroidFexCoreRetainGuestDataLeaseV1() != 0;
    const bool released = retained && LumenAndroidFexCoreReleaseGuestDataLeaseV1() != 0;
    return invoked && retained && released ? 1 : UINT64_MAX;
}

bool NetworkWaitBoundSocket(int* socket, std::array<std::uint8_t, 16>* address) {
    *socket = NetworkWaitNet::Socket(2, 2, 0);
    *address = {16,2,0,0,127,0,0,1};
    if (*socket < 0 || NetworkWaitNet::Bind(*socket, address->data(), address->size()) != ::OK)
        return false;
    std::uint32_t size = address->size();
    return NetworkWaitNet::Getsockname(*socket, address->data(), &size) == ::OK && size == 16 &&
        (*address)[0] == 16 && (*address)[1] == 2 &&
        ((*address)[2] != 0 || (*address)[3] != 0) &&
        (*address)[4] == 127 && (*address)[5] == 0 && (*address)[6] == 0 && (*address)[7] == 1;
}

struct NetworkWaitOutput {
    std::uint64_t before {network_wait_guard};
    std::array<std::uint8_t, 8> bytes {0xa5,0xa5,0xa5,0xa5,0xa5,0xa5,0xa5,0xa5};
    std::uint64_t after {network_wait_guard};
};
struct NetworkWaitAddress {
    std::uint64_t before {network_wait_guard};
    std::array<std::uint8_t, 16> bytes {
        0xa5,0xa5,0xa5,0xa5,0xa5,0xa5,0xa5,0xa5,0xa5,0xa5,0xa5,0xa5,0xa5,0xa5,0xa5,0xa5};
    std::uint64_t middle {network_wait_guard};
    std::uint32_t size {16};
    std::uint32_t padding {0xa5a5a5a5};
    std::uint64_t after {network_wait_guard};
};
static_assert(sizeof(NetworkWaitOutput) == 24);
static_assert(offsetof(NetworkWaitAddress, size) == 32);

bool NetworkWaitNativeContract() {
    int receiver = -1, sender = -1;
    std::array<std::uint8_t, 16> target {};
    bool pass = NetworkWaitBoundSocket(&receiver, &target);
    sender = NetworkWaitNet::Socket(2, 2, 0);
    pass &= sender >= 0;
    const std::array<std::uint8_t, 12> datagram {
        0x31,0x07,0x92,0xaa,0x4d,0xfe,0x18,0x63,0x85,0x29,0x6c,0xd0};
    std::int64_t sent = -1;
    if (pass) {
        sent = NetworkWaitNet::Sendto(sender, datagram.data(), datagram.size(), 0,
            target.data(), target.size());
        pass &= sent == static_cast<std::int64_t>(datagram.size());
    }
    if (!pass) std::fprintf(stderr,
        "PS5_NETWORK_NATIVE_TEST step=setup receiver=%d sender=%d sent=%lld errno=%d\n",
        receiver, sender, static_cast<long long>(sent), *Libs::Posix::GetErrorAddr());
    NetworkWaitOutput output;
    NetworkWaitAddress address;
    if (pass) {
        // Local UDP enqueue need not have completed when Sendto returns.
        // Poll only the non-consuming peek, bounded by one second, and only
        // on the exact empty-queue result. Every payload/guard oracle remains.
        const auto deadline = SteadyNanoseconds() + 1'000'000'000LL;
        std::int64_t peek = -1;
        do {
            peek = NetworkWaitNet::Recvfrom(receiver, output.bytes.data(),
                output.bytes.size(), 0x82, address.bytes.data(), &address.size);
            if (peek >= 0 || *Libs::Posix::GetErrorAddr() != Libs::Posix::POSIX_EWOULDBLOCK ||
                SteadyNanoseconds() >= deadline) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (true);
        const bool peek_ok = peek == 8 && output.bytes == network_wait_payload &&
            output.before == network_wait_guard && output.after == network_wait_guard &&
            address.size == 16 && address.bytes[0] == 16 && address.bytes[1] == 2 &&
            address.bytes[4] == 127 && address.bytes[7] == 1;
        if (!peek_ok) std::fprintf(stderr,
            "PS5_NETWORK_NATIVE_TEST step=peek result=%lld errno=%d bytes=%d guards=%d addrlen=%u address=%u,%u,%u,%u,%u,%u destination_port=%u\n",
            static_cast<long long>(peek), *Libs::Posix::GetErrorAddr(), output.bytes == network_wait_payload,
            output.before == network_wait_guard && output.after == network_wait_guard, address.size,
            address.bytes[0], address.bytes[1], address.bytes[4], address.bytes[5], address.bytes[6], address.bytes[7],
            (static_cast<unsigned>(target[2]) << 8u) | target[3]);
        pass &= peek_ok;
        output = {};
        const auto consumed = NetworkWaitNet::Recv(receiver, output.bytes.data(), 4, 0x80);
        const bool consumed_ok = consumed == 4 && std::equal(datagram.begin(), datagram.begin() + 4,
            output.bytes.begin()) && std::all_of(output.bytes.begin() + 4, output.bytes.end(),
                [](auto byte) { return byte == 0xa5; }) &&
            output.before == network_wait_guard && output.after == network_wait_guard;
        if (!consumed_ok) std::fprintf(stderr,
            "PS5_NETWORK_NATIVE_TEST step=consume result=%lld errno=%d\n",
            static_cast<long long>(consumed), *Libs::Posix::GetErrorAddr());
        pass &= consumed_ok;
        output = {};
        const auto empty = NetworkWaitNet::Recv(receiver, output.bytes.data(), 8, 0x80);
        const bool empty_ok = empty == -1 && *Libs::Posix::GetErrorAddr() == Libs::Posix::POSIX_EWOULDBLOCK &&
            std::all_of(output.bytes.begin(), output.bytes.end(), [](auto byte) { return byte == 0xa5; });
        if (!empty_ok) std::fprintf(stderr,
            "PS5_NETWORK_NATIVE_TEST step=empty result=%lld errno=%d unchanged=%d\n",
            static_cast<long long>(empty), *Libs::Posix::GetErrorAddr(),
            std::all_of(output.bytes.begin(), output.bytes.end(), [](auto byte) { return byte == 0xa5; }));
        pass &= empty_ok;
    }
    if (sender >= 0) pass &= NetworkWaitNet::SocketClose(sender) == ::OK;
    if (receiver >= 0) pass &= NetworkWaitNet::SocketClose(receiver) == ::OK;
    return pass;
}
} // namespace

bool TestNetworkReceiveWait(std::uint64_t stack_top) {
    namespace P = Libs::LibKernel;
    bool pass = true;
    CheckCase("network-native-receive-peek-datagram-truncation-errno", NetworkWaitNativeContract(), pass);
    if (!RegisterHostFunction<NativeNetworkReceive>("self-test-native-network-receive") ||
        !RegisterHostFunction<NativeNetworkNestedReceive>("self-test-nested-network-receive")) return false;
    const auto code_base = Memory::AllocateProgramMemory(0, network_wait_page, Vm::Mode::ReadWrite,
                                                        "fex-network-wait-code");
    const auto records = Memory::AllocateRuntimeMemory(0, network_wait_page * 3, Vm::Mode::ReadWrite,
                                                       "fex-network-wait-records");
    if (code_base == 0 || records == 0) {
        if (records != 0) Memory::FreeGuestMemory(records, network_wait_page * 3);
        if (code_base != 0) Memory::FreeGuestMemory(code_base, network_wait_page);
        return false;
    }
    std::array<std::uint8_t, 128> code {
        0x53,0x48,0x89,0xfb,0x48,0x89,0xdf,0x48,0xb8,
        0,0,0,0,0,0,0,0,0xff,0xd0,0x48,0x85,0xc0,0x74,0xec,0x5b,0xc3};
    const auto target = reinterpret_cast<std::uint64_t>(NativeNetworkReceive);
    std::memcpy(code.data() + 9, &target, sizeof(target));
    constexpr std::array<std::uint8_t, 21> nested_relay {
        0x48,0x83,0xec,0x08,0x48,0xb8,0,0,0,0,0,0,0,0,
        0xff,0xd0,0x48,0x83,0xc4,0x08,0xc3};
    std::copy(nested_relay.begin(), nested_relay.end(), code.begin() + 64);
    const auto nested_target = reinterpret_cast<std::uint64_t>(NativeNetworkNestedReceive);
    std::memcpy(code.data() + 70, &nested_target, sizeof(nested_target));
    pass &= Memory::WriteGuestMemory(nullptr, code_base, code.data(), code.size());
    Memory::SetProgramMemoryProtection(code_base, network_wait_page, Vm::Mode::ExecuteRead);
    const auto output_base = records;
    const auto address_base = records + network_wait_page;
    const auto scratch = records + network_wait_page * 2;

    for (unsigned mode = 0; mode != 4; ++mode) {
        NetworkWaitOutput initial_output, actual_output;
        NetworkWaitAddress initial_address, actual_address;
        std::array<std::uint8_t, 16> destination {};
        int receiver = -1, sender = -1, replacement = -1;
        bool round = NetworkWaitBoundSocket(&receiver, &destination);
        sender = NetworkWaitNet::Socket(2, 2, 0);
        round &= sender >= 0 &&
            Memory::WriteGuestMemory(nullptr, output_base, &initial_output, sizeof(initial_output)) &&
            Memory::WriteGuestMemory(nullptr, address_base, &initial_address, sizeof(initial_address)) &&
            Memory::WriteGuestMemory(nullptr, scratch, &network_wait_guard, sizeof(network_wait_guard));
        network_wait_socket = receiver;
        network_wait_buffer = output_base + offsetof(NetworkWaitOutput, bytes);
        network_wait_size = network_wait_payload.size();
        network_wait_address = address_base + offsetof(NetworkWaitAddress, bytes);
        network_wait_length = address_base + offsetof(NetworkWaitAddress, size);
        network_wait_flags = 0;
        network_wait_started.store(false, std::memory_order_release);
        network_wait_entered.store(false, std::memory_order_release);
        network_wait_finished.store(false, std::memory_order_release);
        network_wait_release.store(false, std::memory_order_release);
        network_wait_sent.store(false, std::memory_order_release);
        network_wait_result.store(INT64_MIN, std::memory_order_release);
        network_wait_error.store(0, std::memory_order_release);
        network_wait_deadline.store(SteadyNanoseconds() + 8'000'000'000LL, std::memory_order_release);
        P::Pthread child {};
        const bool created = round && P::PthreadCreate(&child, nullptr,
            reinterpret_cast<P::pthread_entry_func_t>(code_base), nullptr,
            "fex-network-wait-peer") == ::OK;
        round &= created;
        // Native owned signaler provides bounded cleanup on the old receive
        // path too: a denied epoch becomes a test failure, never a stuck join.
        std::thread signaler([sender, destination, created] {
            if (!created) return;
            const auto release_deadline = SteadyNanoseconds() + 4'000'000'000LL;
            while (!network_wait_release.load(std::memory_order_acquire) &&
                   SteadyNanoseconds() < release_deadline) std::this_thread::yield();
            network_wait_sent.store(NetworkWaitNet::Sendto(sender, network_wait_payload.data(),
                network_wait_payload.size(), 0, destination.data(), destination.size()) ==
                    static_cast<std::int64_t>(network_wait_payload.size()), std::memory_order_release);
        });
        network_wait_started.store(true, std::memory_order_release);
        while (created && !network_wait_entered.load(std::memory_order_acquire) &&
               !network_wait_finished.load(std::memory_order_acquire) &&
               SteadyNanoseconds() < network_wait_deadline.load(std::memory_order_acquire))
            std::this_thread::yield();
        const bool entered = created && network_wait_entered.load(std::memory_order_acquire);
        const auto token = entered ? LumenAndroidFexCoreBeginGuestMemoryMutationV1() : 0;
        bool epoch = token != 0 && !network_wait_finished.load(std::memory_order_acquire);
        if (token != 0) {
            epoch &= Memory::ProtectGuestMemory(scratch, network_wait_page, Vm::Mode::Read);
            if (mode == 1) epoch &= Memory::ProtectGuestMemory(output_base, network_wait_page, Vm::Mode::Read);
            if (mode == 2) epoch &= Memory::ProtectGuestMemory(address_base, network_wait_page, Vm::Mode::Read);
            if (mode == 3) {
                epoch &= NetworkWaitNet::SocketClose(receiver) == ::OK;
                replacement = NetworkWaitNet::Socket(2, 2, 0);
                // The guest descriptor is deliberately reused while the old
                // native receive remains blocked on its pinned transport.
                epoch &= replacement == receiver;
                receiver = -1;
                std::array<std::uint8_t, 16> replacement_address {16,2,0,0,127,0,0,1};
                epoch &= replacement >= 0 && NetworkWaitNet::Bind(replacement,
                    replacement_address.data(), replacement_address.size()) == ::OK;
            }
            // Wake recv while admission is closed. It must await readmission
            // before inspecting or touching any output guest mapping.
            network_wait_release.store(true, std::memory_order_release);
            LumenAndroidFexCoreEndGuestMemoryMutationV1(token, 0, 0, 0);
        } else network_wait_release.store(true, std::memory_order_release);
        signaler.join();
        void* child_result {};
        const bool joined = created && P::PthreadJoinHost(child, &child_result) == ::OK;
        round &= entered && epoch && joined && reinterpret_cast<std::uint64_t>(child_result) == 1 &&
            network_wait_sent.load(std::memory_order_acquire);
        round &= Memory::ReadGuestMemory(nullptr, output_base, &actual_output, sizeof(actual_output)) &&
            Memory::ReadGuestMemory(nullptr, address_base, &actual_address, sizeof(actual_address));
        const bool guards = actual_output.before == network_wait_guard &&
            actual_output.after == network_wait_guard && actual_address.before == network_wait_guard &&
            actual_address.middle == network_wait_guard && actual_address.after == network_wait_guard &&
            actual_address.padding == initial_address.padding;
        if (mode == 1 || mode == 2) {
            round &= network_wait_result.load(std::memory_order_acquire) == -1 &&
                network_wait_error.load(std::memory_order_acquire) == Libs::Posix::POSIX_EFAULT &&
                actual_output.bytes == initial_output.bytes &&
                actual_address.bytes == initial_address.bytes && actual_address.size == initial_address.size;
        } else {
            round &= network_wait_result.load(std::memory_order_acquire) == 8 &&
                network_wait_error.load(std::memory_order_acquire) == Libs::Posix::POSIX_EINVAL &&
                actual_output.bytes == network_wait_payload && actual_address.size == 16 &&
                actual_address.bytes[0] == 16 && actual_address.bytes[1] == 2 &&
                actual_address.bytes[4] == 127 && actual_address.bytes[7] == 1;
        }
        if (replacement >= 0) {
            std::array<std::uint8_t, 8> untouched {0xa5,0xa5,0xa5,0xa5,0xa5,0xa5,0xa5,0xa5};
            round &= NetworkWaitNet::Recv(replacement, untouched.data(), untouched.size(), 0x80) == -1 &&
                *Libs::Posix::GetErrorAddr() == Libs::Posix::POSIX_EWOULDBLOCK &&
                std::all_of(untouched.begin(), untouched.end(), [](auto byte) { return byte == 0xa5; });
        }
        constexpr std::array<const char*, 4> names {
            "network-recvfrom-blocked-reader-allows-vm-and-owned-copyout",
            "network-recvfrom-revoked-payload-fault-keeps-all-outputs",
            "network-recvfrom-revoked-address-fault-keeps-all-outputs",
            "network-recvfrom-pins-transport-across-close-and-descriptor-reuse"};
        CheckCase(names[mode], round && guards, pass);
        pass &= Memory::ProtectGuestMemory(output_base, network_wait_page * 3, Vm::Mode::ReadWrite);
        if (receiver >= 0) pass &= NetworkWaitNet::SocketClose(receiver) == ::OK;
        if (sender >= 0) pass &= NetworkWaitNet::SocketClose(sender) == ::OK;
        if (replacement >= 0) pass &= NetworkWaitNet::SocketClose(replacement) == ::OK;
    }
    // All these real FEX calls must fail before consuming the empty socket or
    // modifying payload/address/length. Nonblocking errors capture native errno
    // before the FEX reader is readmitted, which may itself call host APIs.
    int receiver = -1;
    std::array<std::uint8_t, 16> destination {};
    bool errors = NetworkWaitBoundSocket(&receiver, &destination);
    network_wait_socket = receiver;
    network_wait_flags = 0x80;
    network_wait_started.store(true, std::memory_order_release);
    for (unsigned mode = 0; mode != 6; ++mode) {
        NetworkWaitOutput initial_output, actual_output;
        NetworkWaitAddress initial_address, actual_address;
        if (mode == 5) initial_address.size = 15;
        bool round = errors && Memory::WriteGuestMemory(nullptr, output_base,
            &initial_output, sizeof(initial_output)) && Memory::WriteGuestMemory(nullptr,
                address_base, &initial_address, sizeof(initial_address));
        network_wait_buffer = output_base + offsetof(NetworkWaitOutput, bytes);
        network_wait_size = mode == 4 ? UINT64_MAX : 8;
        network_wait_address = address_base + offsetof(NetworkWaitAddress, bytes);
        network_wait_length = address_base + offsetof(NetworkWaitAddress, size);
        if (mode == 1) network_wait_buffer = 0;
        if (mode == 2) network_wait_buffer = UINT64_MAX - 3;
        if (mode == 3) network_wait_address = network_wait_buffer;
        std::uint64_t result {};
        round &= Invoke(code_base, nullptr, 0, &result, stack_top) && result == 1 &&
            network_wait_result.load(std::memory_order_acquire) == -1;
        const auto error = network_wait_error.load(std::memory_order_acquire);
        round &= error == (mode == 0 ? Libs::Posix::POSIX_EWOULDBLOCK :
            mode == 1 ? Libs::Posix::POSIX_EFAULT : Libs::Posix::POSIX_EINVAL);
        round &= Memory::ReadGuestMemory(nullptr, output_base, &actual_output, sizeof(actual_output)) &&
            Memory::ReadGuestMemory(nullptr, address_base, &actual_address, sizeof(actual_address)) &&
            std::memcmp(&actual_output, &initial_output, sizeof(actual_output)) == 0 &&
            std::memcmp(&actual_address, &initial_address, sizeof(actual_address)) == 0;
        constexpr std::array<const char*, 6> names {
            "network-recvfrom-empty-nonblocking-preserves-errno-and-outputs",
            "network-recvfrom-null-output-rejected-without-effects",
            "network-recvfrom-overflow-output-rejected-without-effects",
            "network-recvfrom-overlapping-outputs-rejected-without-effects",
            "network-recvfrom-unrepresentable-size-rejected-without-effects",
            "network-recvfrom-short-address-rejected-before-consumption"};
        CheckCase(names[mode], round, pass);
    }
    // Queue a datagram before invoking nested receive. A refused callback
    // must preserve both the caller's output and the packet for a later
    // admitted top-level receiver.
    int sender = NetworkWaitNet::Socket(2, 2, 0);
    NetworkWaitOutput initial_output, actual_output;
    NetworkWaitAddress initial_address, actual_address;
    bool nested = errors && sender >= 0 &&
        Memory::WriteGuestMemory(nullptr, output_base, &initial_output, sizeof(initial_output)) &&
        Memory::WriteGuestMemory(nullptr, address_base, &initial_address, sizeof(initial_address)) &&
        NetworkWaitNet::Sendto(sender, network_wait_payload.data(), network_wait_payload.size(), 0,
            destination.data(), destination.size()) == static_cast<std::int64_t>(network_wait_payload.size());
    network_wait_buffer = output_base + offsetof(NetworkWaitOutput, bytes);
    network_wait_size = network_wait_payload.size();
    network_wait_address = address_base + offsetof(NetworkWaitAddress, bytes);
    network_wait_length = address_base + offsetof(NetworkWaitAddress, size);
    std::uint64_t result {};
    nested &= Invoke(code_base + 64, &code_base, 1, &result, stack_top) && result == 1 &&
        network_wait_result.load(std::memory_order_acquire) == -1 &&
        network_wait_error.load(std::memory_order_acquire) == Libs::Posix::POSIX_EOPNOTSUPP &&
        Memory::ReadGuestMemory(nullptr, output_base, &actual_output, sizeof(actual_output)) &&
        Memory::ReadGuestMemory(nullptr, address_base, &actual_address, sizeof(actual_address)) &&
        std::memcmp(&actual_output, &initial_output, sizeof(actual_output)) == 0 &&
        std::memcmp(&actual_address, &initial_address, sizeof(actual_address)) == 0;
    CheckCase("network-recvfrom-nested-reader-refused-without-consuming-or-copyout", nested, pass);
    // The full packet remains queued: exercise FEX PEEK, datagram prefix
    // truncation and ordinary output copy under its real runtime adapter.
    network_wait_flags = 0x82;
    bool peek = Invoke(code_base, nullptr, 0, &result, stack_top) && result == 1 &&
        network_wait_result.load(std::memory_order_acquire) == 8 &&
        Memory::ReadGuestMemory(nullptr, output_base, &actual_output, sizeof(actual_output)) &&
        actual_output.bytes == network_wait_payload && actual_output.before == network_wait_guard &&
        actual_output.after == network_wait_guard;
    CheckCase("network-recvfrom-fex-peek-keeps-datagram-after-nested-refusal", nested && peek, pass);
    network_wait_flags = 0x80;
    network_wait_size = 4;
    bool truncated = Memory::WriteGuestMemory(nullptr, output_base, &initial_output, sizeof(initial_output)) &&
        Invoke(code_base, nullptr, 0, &result, stack_top) && result == 1 &&
        network_wait_result.load(std::memory_order_acquire) == 4 &&
        Memory::ReadGuestMemory(nullptr, output_base, &actual_output, sizeof(actual_output)) &&
        std::equal(network_wait_payload.begin(), network_wait_payload.begin() + 4, actual_output.bytes.begin()) &&
        std::all_of(actual_output.bytes.begin() + 4, actual_output.bytes.end(), [](auto byte) { return byte == 0xa5; }) &&
        actual_output.before == network_wait_guard && actual_output.after == network_wait_guard;
    CheckCase("network-recvfrom-fex-datagram-truncation-preserves-padding", peek && truncated, pass);
    bool consumed = Invoke(code_base, nullptr, 0, &result, stack_top) && result == 1 &&
        network_wait_result.load(std::memory_order_acquire) == -1 &&
        network_wait_error.load(std::memory_order_acquire) == Libs::Posix::POSIX_EWOULDBLOCK;
    CheckCase("network-recvfrom-fex-truncation-consumes-the-whole-datagram", truncated && consumed, pass);
    if (sender >= 0) pass &= NetworkWaitNet::SocketClose(sender) == ::OK;
    if (receiver >= 0) pass &= NetworkWaitNet::SocketClose(receiver) == ::OK;
    pass &= Memory::FreeGuestMemory(records, network_wait_page * 3);
    pass &= Memory::FreeGuestMemory(code_base, network_wait_page);
    return pass;
}
