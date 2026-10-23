// Lumen, 2026-10-04. SPDX-License-Identifier: GPL-2.0-only
// Included by fexRuntimeTests.cpp after the native network fixture helpers.
namespace {
namespace SocketFlagsNet = Libs::Network::Net;
namespace SocketFlagsFs = Libs::LibKernel::FileSystem;

bool SocketFlagsNativeFile(uint64_t entry = 0, uint64_t stack_top = 0) {
    const auto fcntl = [entry, stack_top](int descriptor, int command, uint64_t argument) {
        if (entry == 0) return Libs::Posix::fcntl(descriptor, command, argument);
        const std::array<uint64_t, 3> args {static_cast<uint64_t>(descriptor),
            static_cast<uint64_t>(command), argument};
        uint64_t result {};
        return Invoke(entry, args.data(), args.size(), &result, stack_top)
            ? static_cast<int32_t>(result) : INT_MIN;
    };
    std::error_code error;
    const auto base = std::filesystem::temp_directory_path(error);
    if (error) return false;
    const auto directory = base / ("lumen-fcntl-" + std::to_string(SteadyNanoseconds()));
    if (!std::filesystem::create_directory(directory, error) || error) return false;
    const auto mount = "/" + directory.filename().string();
    struct Fixture {
        std::filesystem::path directory;
        std::string mount;
        int descriptor = -1;
        ~Fixture() {
            if (descriptor >= 0) SocketFlagsFs::KernelClose(descriptor);
            SocketFlagsFs::Umount(mount);
            std::error_code ignored;
            std::filesystem::remove_all(directory, ignored);
        }
    } fixture {directory, mount};
    const auto filename = directory / "flags.bin";
    {
        std::ofstream file(filename, std::ios::binary);
        file.write("abc", 3);
        if (!file) return false;
    }
    SocketFlagsFs::Mount(directory, mount);
    const auto guest_path = mount + "/flags.bin";
    constexpr uint32_t original_flags = 2u | 0x80u | 0x1000u;
    fixture.descriptor = SocketFlagsFs::KernelOpen(guest_path.c_str(), original_flags, 0);
    if (fixture.descriptor < 0) return false;
    const auto descriptor = fixture.descriptor;
    bool pass = fcntl(descriptor, 3, 0) == static_cast<int>(original_flags) &&
        fcntl(descriptor, 2, 1) == 0 && fcntl(descriptor, 1, 0) == 1;
    pass &= fcntl(descriptor, 4, 4u | 8u) == 0 &&
        fcntl(descriptor, 3, 0) == static_cast<int>(original_flags | 4u | 8u) &&
        SocketFlagsFs::KernelLseek(descriptor, 0, 0) == 0 &&
        SocketFlagsFs::KernelWrite(descriptor, "d", 1) == 1;
    pass &= fcntl(descriptor, 4, 0) == 0 &&
        fcntl(descriptor, 3, 0) == static_cast<int>(original_flags) &&
        SocketFlagsFs::KernelLseek(descriptor, 0, 0) == 0 &&
        SocketFlagsFs::KernelWrite(descriptor, "X", 1) == 1;
    for (const auto arguments: {std::pair<int, uint64_t> {2, 2}, {4, 0x10},
                               {4, UINT64_C(1) << 32}, {0, 0}}) {
        *Libs::Posix::GetErrorAddr() = 0;
        pass &= fcntl(descriptor, arguments.first, arguments.second) == -1 &&
            *Libs::Posix::GetErrorAddr() == Libs::Posix::POSIX_EINVAL &&
            fcntl(descriptor, 3, 0) == static_cast<int>(original_flags);
    }
    pass &= SocketFlagsFs::KernelClose(descriptor) == ::OK;
    fixture.descriptor = -1;
    *Libs::Posix::GetErrorAddr() = 0;
    pass &= fcntl(descriptor, 3, 0) == -1 &&
        *Libs::Posix::GetErrorAddr() == Libs::Posix::POSIX_EBADF;
    std::array<char, 5> contents {};
    std::ifstream file(filename, std::ios::binary);
    file.read(contents.data(), contents.size());
    pass &= file.gcount() == 4 && std::memcmp(contents.data(), "Xbcd", 4) == 0;
    return pass;
}

bool SocketFlagsP2pContract() {
    const int first = SocketFlagsNet::Socket(2, 6, 0);
    const int second = SocketFlagsNet::Socket(2, 6, 0);
    bool pass = first >= 0 && second >= 0;
    if (pass) {
        // Logical policies remain independent, including the SO_NBIO route.
        pass &= Libs::Posix::fcntl(first, 3, 0) == 2 &&
            Libs::Posix::fcntl(second, 3, 0) == 2 &&
            Libs::Posix::fcntl(first, 4, 4) == 0 &&
            Libs::Posix::fcntl(first, 3, 0) == 6 &&
            Libs::Posix::fcntl(second, 3, 0) == 2;
        int value = 0;
        uint32_t size = sizeof(value);
        pass &= SocketFlagsNet::Getsockopt(first, 0xffff, 0x1200, &value, &size) == 0 && value == 1;
        value = 0;
        pass &= SocketFlagsNet::Setsockopt(first, 0xffff, 0x1200, &value, sizeof(value)) == 0 &&
            Libs::Posix::fcntl(first, 3, 0) == 2;
        value = 1;
        pass &= SocketFlagsNet::Setsockopt(second, 0xffff, 0x1200, &value, sizeof(value)) == 0 &&
            Libs::Posix::fcntl(second, 3, 0) == 6 &&
            Libs::Posix::fcntl(first, 3, 0) == 2;
    }
    if (first >= 0) pass &= SocketFlagsNet::SocketClose(first) == ::OK;
    if (second >= 0) pass &= SocketFlagsNet::SocketClose(second) == ::OK;
    return pass;
}

// Rescue a blocking regression with a local datagram after one second. The
// expected empty-queue result must arrive without receiving that datagram.
template<typename Receive>
bool SocketFlagsEmptyQueue(int sender, const std::array<uint8_t, 16>& target,
                           Receive receive) {
    std::atomic<bool> finished {};
    std::atomic<bool> rescue_sent {};
    std::thread rescue([&] {
        const auto deadline = SteadyNanoseconds() + 1'000'000'000LL;
        while (!finished.load(std::memory_order_acquire) && SteadyNanoseconds() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (!finished.load(std::memory_order_acquire))
            rescue_sent.store(SocketFlagsNet::Sendto(sender, network_wait_payload.data(),
                network_wait_payload.size(), 0, target.data(), target.size()) == 8,
                std::memory_order_release);
    });
    const bool pass = receive();
    finished.store(true, std::memory_order_release);
    rescue.join();
    return pass && !rescue_sent.load(std::memory_order_acquire);
}
} // namespace

bool TestSocketFlags(uint64_t stack_top) {
    bool pass = true;
    CheckCase("fcntl-native-file-append-content-and-immutable-status", SocketFlagsNativeFile(), pass);
    CheckCase("fcntl-p2p-logical-nonblocking-and-so-nbio-coherence", SocketFlagsP2pContract(), pass);
    Loader::SymbolDatabase symbols;
    Libs::Posix::InitLibKernel_1_Posix(&symbols);
    const Loader::SymbolResolve import {"8nY19bKoiZk", "Posix", 1, "libkernel", 1, 1,
                                        Loader::SymbolType::Func};
    const auto* binding = symbols.Find(import);
    const bool typed_binding = binding != nullptr &&
        binding->vaddr == reinterpret_cast<uint64_t>(Libs::Posix::fcntl);
    CheckCase("fcntl-posix-authoritative-typed-binding", typed_binding, pass);
    if (!typed_binding || !RegisterHostFunction<SocketFlagsNet::Recv>("self-test-socket-flags-recv"))
        return false;
    const auto code_base = Memory::AllocateProgramMemory(0, 0x4000, Vm::Mode::ReadWrite,
                                                        "fex-socket-flags-code");
    const auto records = Memory::AllocateRuntimeMemory(0, 0x4000, Vm::Mode::ReadWrite,
                                                       "fex-socket-flags-records");
    if (code_base == 0 || records == 0) {
        if (code_base != 0) Memory::FreeGuestMemory(code_base, 0x4000);
        if (records != 0) Memory::FreeGuestMemory(records, 0x4000);
        return false;
    }
    std::array<uint8_t, 1024> code {};
    constexpr std::array<uint8_t, 21> relay {
        0x48,0x83,0xec,0x08,0x48,0xb8,0,0,0,0,0,0,0,0,
        0xff,0xd0,0x48,0x83,0xc4,0x08,0xc3};
    for (const auto offset: {0u, 32u}) std::copy(relay.begin(), relay.end(), code.begin() + offset);
    Put64(code, 6, binding->vaddr);
    Put64(code, 38, reinterpret_cast<uint64_t>(SocketFlagsNet::Recv));
    pass &= Memory::WriteGuestMemory(nullptr, code_base, code.data(), code.size());
    Memory::SetProgramMemoryProtection(code_base, 0x4000, Vm::Mode::ExecuteRead);
    CheckCase("fcntl-real-fex-file-positive-getters-append-content-and-errors",
              SocketFlagsNativeFile(code_base, stack_top), pass);
    const auto call = [&](int descriptor, int command, uint64_t argument, int expected,
                           int expected_errno = 0) {
        const std::array<uint64_t, 3> args {static_cast<uint64_t>(descriptor),
            static_cast<uint64_t>(command), argument};
        uint64_t result {};
        *Libs::Posix::GetErrorAddr() = 0;
        return Invoke(code_base, args.data(), args.size(), &result, stack_top) &&
            static_cast<int32_t>(result) == expected &&
            *Libs::Posix::GetErrorAddr() == expected_errno;
    };
    int receiver = -1;
    std::array<uint8_t, 16> target {};
    bool socket_pass = NetworkWaitBoundSocket(&receiver, &target);
    const int sender = SocketFlagsNet::Socket(2, 2, 0);
    socket_pass &= sender >= 0;
    if (socket_pass) {
        socket_pass &= call(receiver, 3, 0, 2) && call(receiver, 4, 4, 0) &&
            call(receiver, 3, 0, 6) && call(receiver, 2, 1, 0) && call(receiver, 1, 0, 1);
        NetworkWaitOutput output;
        const auto initial = output;
        socket_pass &= SocketFlagsEmptyQueue(sender, target, [&] {
            const auto result = SocketFlagsNet::Recv(receiver, output.bytes.data(), output.bytes.size(), 0);
            return result == -1 && *Libs::Posix::GetErrorAddr() == Libs::Posix::POSIX_EWOULDBLOCK &&
                std::memcmp(&output, &initial, sizeof(output)) == 0;
        });
        const auto typed_empty = [&] {
            if (!Memory::WriteGuestMemory(nullptr, records, &initial, sizeof(initial))) return false;
            const std::array<uint64_t, 4> args {static_cast<uint64_t>(receiver), records + 8, 8, 0};
            uint64_t result {};
            *Libs::Posix::GetErrorAddr() = 0;
            const bool invoked = Invoke(code_base + 32, args.data(), args.size(), &result, stack_top);
            NetworkWaitOutput actual;
            return invoked && static_cast<int64_t>(result) == -1 &&
                *Libs::Posix::GetErrorAddr() == Libs::Posix::POSIX_EWOULDBLOCK &&
                Memory::ReadGuestMemory(nullptr, records, &actual, sizeof(actual)) &&
                std::memcmp(&actual, &initial, sizeof(actual)) == 0;
        };
        socket_pass &= SocketFlagsEmptyQueue(sender, target, typed_empty);
        int nonblock = 0;
        socket_pass &= SocketFlagsNet::Setsockopt(receiver, 0xffff, 0x1200, &nonblock, sizeof(nonblock)) == 0 &&
            call(receiver, 3, 0, 2);
        nonblock = 1;
        socket_pass &= SocketFlagsNet::Setsockopt(receiver, 0xffff, 0x1200, &nonblock, sizeof(nonblock)) == 0 &&
            call(receiver, 3, 0, 6);
        for (const auto arguments: {std::pair<int, uint64_t> {2, 2}, {4, 8},
                                   {4, UINT64_C(1) << 32}, {0, 0}})
            socket_pass &= call(receiver, arguments.first, arguments.second, -1, Libs::Posix::POSIX_EINVAL) &&
                call(receiver, 3, 0, 6) && call(receiver, 1, 0, 1);
        socket_pass &= call(-1, 3, 0, -1, Libs::Posix::POSIX_EBADF);
    }
    if (receiver >= 0) {
        socket_pass &= SocketFlagsNet::SocketClose(receiver) == ::OK &&
            call(receiver, 3, 0, -1, Libs::Posix::POSIX_EBADF);
        const int replacement = SocketFlagsNet::Socket(2, 2, 0);
        socket_pass &= replacement == receiver && call(replacement, 3, 0, 2) && call(replacement, 1, 0, 0);
        if (replacement >= 0) socket_pass &= SocketFlagsNet::SocketClose(replacement) == ::OK;
    }
    if (sender >= 0) socket_pass &= SocketFlagsNet::SocketClose(sender) == ::OK;
    CheckCase("fcntl-real-fex-socket-nonblocking-errors-canaries-and-reuse", socket_pass, pass);
    pass &= Memory::FreeGuestMemory(records, 0x4000);
    pass &= Memory::FreeGuestMemory(code_base, 0x4000);
    return pass;
}
