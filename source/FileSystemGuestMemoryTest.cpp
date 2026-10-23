#include "Lumen/Core/Threads.h"
#include "Lumen/Core/File.h"
#include "Lumen/Core/VirtualMemory.h"
#include "Emulator/Config.h"
#include "Emulator/Kernel/FileSystem.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/HostCall.h"
#include "Emulator/Loader/HostCallScalar.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "Emulator/Log.h"
#include "CpuX64.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace {
namespace VM = Lumen::Core::VirtualMemory;
namespace FS = Lumen::Libs::LibKernel::FileSystem;
using Lumen::Libs::LibKernel::KERNEL_ERROR_EFAULT;

bool Check(bool value, const char* message)
{
	if (!value) std::fprintf(stderr, "FileSystemGuestMemoryTest: %s\n", message);
	return value;
}

bool CheckKernelFileBindings()
{
	Lumen::Loader::SymbolDatabase symbols;
	if (!Check(Lumen::Libs::Init(U"libkernel_1", &symbols), "libkernel registration")) return false;
	struct Binding
	{
		const char* nid;
		uint64_t target;
		uint8_t arity;
	};
	const std::array<Binding, 8> bindings {{
	    {"52NcYU9+lEo", reinterpret_cast<uint64_t>(&FS::KernelRename), 2},
	    {"naInUjYt3so", reinterpret_cast<uint64_t>(&FS::KernelRmdir), 1},
	    {"fTx66l5iWIA", reinterpret_cast<uint64_t>(&FS::KernelFsync), 1},
	    {"VW3TVZiM4-E", reinterpret_cast<uint64_t>(&FS::KernelFtruncate), 2},
	    {"UtszJWHrDcA", reinterpret_cast<uint64_t>(&FS::KernelFchmod), 2},
	    {"fgIsQ10xYVA", reinterpret_cast<uint64_t>(&FS::KernelChmod), 2},
	    {"0Cq8ipKr9n0", reinterpret_cast<uint64_t>(&FS::KernelUtimes), 2},
	    {"NLq2b1jOaN0", reinterpret_cast<uint64_t>(&FS::KernelFutimes), 2},
	}};
	for (const auto& binding: bindings)
	{
		Lumen::Loader::SymbolResolve query {};
		query.name = binding.nid;
		query.library = "libkernel";
		query.library_version = 1;
		query.module = "libkernel";
		query.module_version_major = 1;
		query.module_version_minor = 1;
		query.type = Lumen::Loader::SymbolType::Func;
		const auto* record = symbols.Find(query);
		const auto* scalar = record == nullptr ? nullptr :
		    symbols.HostCallScalarMetadataFor(record->vaddr);
		if (!Check(record != nullptr && record->vaddr == binding.target &&
		           scalar != nullptr && scalar->arity == binding.arity,
		           "exact libkernel filesystem binding or scalar ABI"))
			return false;
		query.library = "unrelated";
		if (!Check(symbols.Find(query) == nullptr, "libkernel filesystem binding escaped namespace"))
			return false;
	}
	return true;
}

class Mapping
{
public:
	explicit Mapping(uint64_t size): address(VM::Alloc(0, size, VM::Mode::ReadWrite)) {}
	~Mapping() { if (address != 0) VM::Free(address); }
	char* Bytes(uint64_t offset = 0) const { return reinterpret_cast<char*>(address + offset); }
	uint64_t address;
};

bool CheckRejectedSeekReleasesFileLock(int file)
{
	const bool rejected = FS::KernelLseek(file, -1, 0) == Lumen::Libs::LibKernel::KERNEL_ERROR_EINVAL &&
	                      FS::KernelLseek(file, -4, 1) == Lumen::Libs::LibKernel::KERNEL_ERROR_EINVAL &&
	                      FS::KernelLseek(file, -8, 2) == Lumen::Libs::LibKernel::KERNEL_ERROR_EINVAL;
	if (!Check(rejected, "negative seeks return EINVAL")) return false;

	// Core::Mutex is recursive on desktop, so a same-thread follow-up would not
	// detect a leaked lock. Exercise the same descriptor from another thread.
	std::atomic<bool> done {false};
	std::atomic<int64_t> position {-1};
	std::thread reader([&] {
		position.store(FS::KernelLseek(file, 0, 1));
		done.store(true);
	});
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (!done.load() && std::chrono::steady_clock::now() < deadline)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	if (!done.load())
	{
		std::fprintf(stderr, "FileSystemGuestMemoryTest: seek left the file locked for another thread\n");
		std::fflush(stderr);
		std::_Exit(1);
	}
	reader.join();
	return Check(position.load() == 3, "rejected seeks preserve the position and permit another thread");
}

struct HostMetadata
{
	mode_t   mode;
	timespec access;
	timespec write;
};

bool ReadHostMetadata(const char* filename, HostMetadata* output)
{
	struct stat state {};
	if (::stat(filename, &state) != 0) return false;
	output->mode = state.st_mode;
#if defined(__APPLE__)
	output->access = state.st_atimespec;
	output->write  = state.st_mtimespec;
#else
	output->access = state.st_atim;
	output->write  = state.st_mtim;
#endif
	return true;
}

bool SameHostMetadata(const HostMetadata& left, const HostMetadata& right)
{
	return left.mode == right.mode && left.access.tv_sec == right.access.tv_sec &&
	       left.access.tv_nsec == right.access.tv_nsec && left.write.tv_sec == right.write.tv_sec &&
	       left.write.tv_nsec == right.write.tv_nsec;
}

bool SameGuestTime(const Lumen::Libs::LibKernel::KernelTimespec& left,
	               const Lumen::Libs::LibKernel::KernelTimespec& right)
{
	return left.tv_sec == right.tv_sec && left.tv_nsec == right.tv_nsec;
}

bool CheckMetadataOverlay(const char* directory, const char* filename, int file, int directory_descriptor,
	                      Mapping& memory, uint64_t page)
{
	using Lumen::Libs::LibKernel::KERNEL_ERROR_EBADF;
	using Lumen::Libs::LibKernel::KERNEL_ERROR_EINVAL;

	HostMetadata host_before {};
	if (!Check(ReadHostMetadata(filename, &host_before), "read host metadata baseline")) return false;
	FS::FileStat guest_before {};
	if (!Check(FS::KernelFstat(file, &guest_before) == OK, "guest metadata baseline")) return false;

	bool ok = true;
	ok = Check(FS::KernelFchmod(file, 0601) == OK, "fchmod records guest mode") && ok;
	FS::FileStat by_descriptor {};
	FS::FileStat by_path {};
	ok = Check(FS::KernelFstat(file, &by_descriptor) == OK &&
	           FS::KernelStat(filename, &by_path) == OK &&
	           (by_descriptor.st_mode & 0170000u) == 0100000u &&
	           (by_descriptor.st_mode & 0007777u) == 0601u &&
	           by_descriptor.st_mode == by_path.st_mode &&
	           SameGuestTime(by_descriptor.st_birthtim, guest_before.st_birthtim),
	           "fchmod is visible through stat/fstat without changing type or birth time") && ok;
	ok = Check(FS::KernelChmod(filename, 017640) == OK &&
	           FS::KernelFstat(file, &by_descriptor) == OK &&
	           (by_descriptor.st_mode & 0007777u) == 07640u &&
	           (by_descriptor.st_mode & 0170000u) == 0100000u,
	           "chmod replaces only guest permission/special bits") && ok;

	auto* first_times = reinterpret_cast<Lumen::Libs::LibKernel::KernelTimeval*>(memory.Bytes(2048));
	first_times[0] = {123456789, 123456};
	first_times[1] = {223456789, 654321};
	ok = Check(FS::KernelFutimes(file, first_times) == OK &&
	           FS::KernelStat(filename, &by_path) == OK &&
	           by_path.st_atim.tv_sec == 123456789 && by_path.st_atim.tv_nsec == 123456000 &&
	           by_path.st_mtim.tv_sec == 223456789 && by_path.st_mtim.tv_nsec == 654321000,
	           "futimes publishes exact microsecond metadata") && ok;

	auto* second_times = reinterpret_cast<Lumen::Libs::LibKernel::KernelTimeval*>(memory.Bytes(2112));
	second_times[0] = {323456789, 1};
	second_times[1] = {423456789, 999999};
	ok = Check(FS::KernelUtimes(filename, second_times) == OK &&
	           FS::KernelFstat(file, &by_descriptor) == OK &&
	           by_descriptor.st_atim.tv_sec == 323456789 && by_descriptor.st_atim.tv_nsec == 1000 &&
	           by_descriptor.st_mtim.tv_sec == 423456789 && by_descriptor.st_mtim.tv_nsec == 999999000,
	           "utimes and fstat share one overlay") && ok;

	const FS::FileStat before_rejection = by_descriptor;
	first_times[0].tv_usec = 1000000;
	ok = Check(FS::KernelFutimes(file, first_times) == KERNEL_ERROR_EINVAL &&
	           FS::KernelFstat(file, &by_descriptor) == OK &&
	           SameGuestTime(by_descriptor.st_atim, before_rejection.st_atim) &&
	           SameGuestTime(by_descriptor.st_mtim, before_rejection.st_mtim),
	           "invalid microseconds leave metadata unchanged") && ok;
	first_times[0].tv_usec = 123456;
	for (const auto* invalid: {
	         reinterpret_cast<const Lumen::Libs::LibKernel::KernelTimeval*>(memory.Bytes(page * 2)),
	         reinterpret_cast<const Lumen::Libs::LibKernel::KernelTimeval*>(memory.Bytes(page * 2 - 8)),
	         reinterpret_cast<const Lumen::Libs::LibKernel::KernelTimeval*>(UINTPTR_MAX - 3)})
	{
		ok = Check(FS::KernelFutimes(file, invalid) == KERNEL_ERROR_EFAULT,
		           "futimes rejects guard, cross-page and overflow pointers") && ok;
	}
	ok = Check(FS::KernelFchmod(-1, 0600) == KERNEL_ERROR_EBADF &&
	           FS::KernelFutimes(INT_MAX, nullptr) == KERNEL_ERROR_EBADF &&
	           FS::KernelChmod(nullptr, 0600) == KERNEL_ERROR_EFAULT &&
	           FS::KernelUtimes(memory.Bytes(page * 2), nullptr) == KERNEL_ERROR_EFAULT,
	           "metadata APIs reject invalid descriptors and paths") && ok;

	// SetMode and SetTimes update one entry under one lock. Concurrent readers
	// must therefore see a complete mode and one coherent timestamp pair.
	std::atomic<bool> start {false};
	std::thread mode_writer([&] {
		while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
		for (int index = 0; index < 200; ++index)
			(void)FS::KernelFchmod(file, (index & 1) != 0 ? 0600 : 0644);
	});
	std::thread time_writer([&] {
		while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
		for (int index = 0; index < 200; ++index)
			(void)FS::KernelFutimes(file, (index & 1) != 0 ? first_times : second_times);
	});
	start.store(true, std::memory_order_release);
	mode_writer.join();
	time_writer.join();
	ok = Check(FS::KernelStat(filename, &by_path) == OK &&
	           ((by_path.st_mode & 0007777u) == 0600u || (by_path.st_mode & 0007777u) == 0644u) &&
	           ((by_path.st_atim.tv_sec == first_times[0].tv_sec &&
	             by_path.st_mtim.tv_sec == first_times[1].tv_sec) ||
	            (by_path.st_atim.tv_sec == second_times[0].tv_sec &&
	             by_path.st_mtim.tv_sec == second_times[1].tv_sec)),
	           "concurrent metadata updates remain coherent") && ok;

	HostMetadata host_after {};
	ok = Check(ReadHostMetadata(filename, &host_after) && SameHostMetadata(host_before, host_after),
	           "guest chmod/utimes never mutate host metadata") && ok;

	const std::string truncate_path = std::string(directory) + "/metadata-truncate.bin";
	auto* truncate_file = std::fopen(truncate_path.c_str(), "wb");
	if (!Check(truncate_file != nullptr, "truncate fixture")) return false;
	const bool truncate_written = std::fwrite("truncate", 1, 8, truncate_file) == 8;
	std::fclose(truncate_file);
	const int truncate_descriptor = FS::KernelOpen(truncate_path.c_str(), 2, 0);
	FS::FileStat truncated {};
	ok = Check(truncate_written && truncate_descriptor >= 3 &&
	           FS::KernelFtruncate(truncate_descriptor, 3) == OK &&
	           FS::KernelFstat(truncate_descriptor, &truncated) == OK && truncated.st_size == 3 &&
	           FS::KernelFtruncate(truncate_descriptor, -1) == KERNEL_ERROR_EINVAL &&
	           FS::KernelFstat(truncate_descriptor, &truncated) == OK && truncated.st_size == 3 &&
	           FS::KernelFtruncate(directory_descriptor, 0) == Lumen::Libs::LibKernel::KERNEL_ERROR_EISDIR &&
	           FS::KernelFtruncate(file, 0) == KERNEL_ERROR_EBADF &&
	           FS::KernelFtruncate(-1, 0) == KERNEL_ERROR_EBADF,
	           "ftruncate resizes writable files and rejects invalid inputs atomically") && ok;
	ok = Check(FS::KernelClose(truncate_descriptor) == OK &&
	           FS::KernelUnlink(truncate_path.c_str()) == OK,
	           "truncate fixture cleanup") && ok;

	const std::string transient = std::string(directory) + "/metadata-recreate.bin";
	auto* transient_file = std::fopen(transient.c_str(), "wb");
	if (!Check(transient_file != nullptr, "metadata recreate fixture")) return false;
	std::fclose(transient_file);
	ok = Check(FS::KernelChmod(transient.c_str(), 0123) == OK &&
	           FS::KernelUnlink(transient.c_str()) == OK,
	           "unlink removes file carrying overlay metadata") && ok;
	transient_file = std::fopen(transient.c_str(), "wb");
	if (!Check(transient_file != nullptr, "recreate unlinked fixture")) return false;
	std::fclose(transient_file);
	FS::FileStat recreated {};
	ok = Check(FS::KernelStat(transient.c_str(), &recreated) == OK &&
	           (recreated.st_mode & 0007777u) == 0777u,
	           "recreated path does not inherit unlinked overlay") && ok;
	::unlink(transient.c_str());

	const std::string rename_source = std::string(directory) + "/metadata-rename-source.bin";
	const std::string rename_destination = std::string(directory) + "/metadata-rename-destination.bin";
	transient_file = std::fopen(rename_source.c_str(), "wb");
	if (!Check(transient_file != nullptr, "rename fixture")) return false;
	std::fclose(transient_file);
	const int rename_descriptor = FS::KernelOpen(rename_source.c_str(), 0, 0);
	ok = Check(rename_descriptor >= 3 && FS::KernelFchmod(rename_descriptor, 0345) == OK &&
	           FS::KernelRename(rename_source.c_str(), rename_destination.c_str()) == OK &&
	           FS::KernelFstat(rename_descriptor, &recreated) == OK &&
	           (recreated.st_mode & 0007777u) == 0345u &&
	           FS::KernelStat(rename_source.c_str(), &recreated) == Lumen::Libs::LibKernel::KERNEL_ERROR_ENOENT &&
	           FS::KernelStat(rename_destination.c_str(), &recreated) == OK &&
	           (recreated.st_mode & 0007777u) == 0345u,
	           "rename migrates descriptor and overlay identity") && ok;
	ok = Check(FS::KernelClose(rename_descriptor) == OK &&
	           FS::KernelUnlink(rename_destination.c_str()) == OK,
	           "renamed fixture cleanup") && ok;

	const std::string empty_directory = std::string(directory) + "/metadata-directory";
	ok = Check(FS::KernelMkdir(empty_directory.c_str(), 0700) == OK &&
	           FS::KernelChmod(empty_directory.c_str(), 0712) == OK &&
	           FS::KernelRmdir(empty_directory.c_str()) == OK &&
	           FS::KernelMkdir(empty_directory.c_str(), 0700) == OK &&
	           FS::KernelStat(empty_directory.c_str(), &recreated) == OK &&
	           (recreated.st_mode & 0007777u) == 0777u &&
	           FS::KernelRmdir(empty_directory.c_str()) == OK,
	           "rmdir and mkdir clear directory overlay metadata") && ok;
	ok = Check(FS::KernelRename(nullptr, rename_destination.c_str()) == KERNEL_ERROR_EFAULT &&
	           FS::KernelRmdir(memory.Bytes(page * 2)) == KERNEL_ERROR_EFAULT,
	           "rename/rmdir validate guest path pointers") && ok;

	return ok;
}

bool CheckExclusiveCreation(const char* directory)
{
	using namespace Lumen::Libs::LibKernel;
	constexpr int create_exclusive_rw = 0x0a02;
	Mapping memory(8192);
	if (!Check(memory.address != 0, "exclusive-create guest buffers")) return false;
	const std::string prefix = std::string(directory) + "/exclusive-";
	const std::array<std::string, 7> names {prefix + "existing", prefix + "next", prefix + "read",
	    prefix + "write", prefix + "readonly-trunc", prefix + "race", prefix + "parent-file"};
	struct Cleanup
	{
		const std::array<std::string, 7>& names;
		~Cleanup() { for (const auto& name: names) unlink(name.c_str()); }
	} cleanup {names};
	struct Descriptor
	{
		int value = -1;
		~Descriptor() { if (value >= 3) (void)FS::KernelClose(value); }
		bool Close() { const int old = value; value = -1; return FS::KernelClose(old) == OK; }
	};
	const auto path = [&](const std::string& name) {
		std::strcpy(memory.Bytes(), name.c_str());
		return memory.Bytes();
	};
	const std::array<uint8_t, 8> payload {0, 1, 17, 33, 127, 128, 254, 255};
	constexpr int64_t payload_bytes = 8;
	std::memcpy(memory.Bytes(4096), payload.data(), payload.size());
	Descriptor original {FS::KernelOpen(path(names[0]), create_exclusive_rw, 0600)};
	if (!Check(original.value >= 3 && FS::KernelFcntl(original.value, 3, 0) == 2,
	           "exclusive-create returns RW descriptor without transient flags")) return false;
	const int reusable_descriptor = original.value;
	if (!Check(FS::KernelWrite(original.value, memory.Bytes(4096), payload.size()) == payload_bytes &&
	           FS::KernelPread(original.value, memory.Bytes(4112), payload.size(), 0) == payload_bytes &&
	           std::memcmp(memory.Bytes(4112), payload.data(), payload.size()) == 0 &&
	           FS::KernelLseek(original.value, 0, 1) == payload_bytes,
	           "exclusive Write->Pread immediate bytes without flush; Pread preserves cursor")) return false;
	if (!Check(FS::KernelPwrite(original.value, memory.Bytes(4096), 1, 3) == 1 &&
	           FS::KernelLseek(original.value, 0, 1) == payload_bytes &&
	           FS::KernelChmod(path(names[0]), 0751) == OK, "exclusive Pwrite cursor and metadata fixture")) return false;
	FS::FileStat before {};
	if (!Check(FS::KernelFstat(original.value, &before) == OK && original.Close(), "close exclusive initial descriptor")) return false;
	for (unsigned i = 0; i < 16; ++i)
	{
		if (!Check(FS::KernelOpen(path(names[0]), create_exclusive_rw | (i % 2 == 0 ? 0x0400 : 0), 0600) == KERNEL_ERROR_EEXIST,
		           "existing EXCL file returns EEXIST, including TRUNC")) return false;
	}
	FS::FileStat after {};
	if (!Check(FS::KernelStat(path(names[0]), &after) == OK && after.st_size == before.st_size &&
	           after.st_mode == before.st_mode && after.st_ctim.tv_sec == before.st_ctim.tv_sec &&
	           after.st_ctim.tv_nsec == before.st_ctim.tv_nsec,
	           "failed exclusive creation preserves bytes length and metadata overlay")) return false;
	Descriptor reopened {FS::KernelOpen(path(names[0]), 2, 0)};
	std::array<uint8_t, 8> changed = payload;
	changed[3] = payload[0];
	if (!Check(reopened.value == reusable_descriptor &&
	           FS::KernelPread(reopened.value, memory.Bytes(4112), payload.size(), 0) == payload_bytes &&
	           std::memcmp(memory.Bytes(4112), changed.data(), changed.size()) == 0 && reopened.Close(),
	           "failed EXCL calls leak no descriptor and leave exact existing bytes")) return false;
	if (!Check(FS::KernelOpen(path(names[4]), 0x0e00, 0600) == KERNEL_ERROR_EACCES &&
	           access(names[4].c_str(), F_OK) != 0 &&
	           FS::KernelOpen(path(std::string(directory) + "/missing-parent/file"), create_exclusive_rw, 0600) == KERNEL_ERROR_ENOENT &&
	           FS::KernelOpen(path(std::string(directory)), create_exclusive_rw, 0600) == KERNEL_ERROR_EEXIST &&
	           FS::KernelOpen(path(names[0] + "/child"), create_exclusive_rw, 0600) == KERNEL_ERROR_ENOTDIR,
	           "readonly/trunc, absent parent, directory, nondirectory errors precede effects")) return false;
	Descriptor next {FS::KernelOpen(path(names[1]), create_exclusive_rw, 0600)};
	if (!Check(next.value == reusable_descriptor && next.Close(), "all error paths preserve descriptor slot reuse")) return false;
	Descriptor read {FS::KernelOpen(path(names[2]), 0x0a00, 0600)};
	const auto* invalid_source = reinterpret_cast<const void*>(UINTPTR_MAX - 3);
	if (!Check(read.value >= 3 && FS::KernelFcntl(read.value, 3, 0) == 0 &&
	           FS::KernelWrite(read.value, invalid_source, 8) == KERNEL_ERROR_EBADF &&
	           FS::KernelPwrite(read.value, invalid_source, 8, 0) == KERNEL_ERROR_EBADF &&
	           FS::KernelFtruncate(read.value, 8) == KERNEL_ERROR_EBADF && read.Close(),
	           "read-only native/guest access denies writes before guest borrow")) return false;
	Descriptor write {FS::KernelOpen(path(names[3]), 0x0a01, 0600)};
	std::memset(memory.Bytes(4112), 0x5a, payload.size());
	uint64_t mapped_bytes = 99;
	if (!Check(write.value >= 3 && FS::KernelFcntl(write.value, 3, 0) == 1 &&
	           FS::KernelWrite(write.value, memory.Bytes(4096), payload.size()) == payload_bytes &&
	           FS::KernelRead(write.value, memory.Bytes(4112), payload.size()) == KERNEL_ERROR_EBADF &&
	           FS::KernelPread(write.value, memory.Bytes(4112), payload.size(), 0) == KERNEL_ERROR_EBADF &&
	           FS::KernelReadMappedRange(write.value, memory.Bytes(4112), payload.size(), 0, &mapped_bytes) == KERNEL_ERROR_EBADF &&
	           mapped_bytes == 0 && std::all_of(memory.Bytes(4112), memory.Bytes(4112) + payload.size(), [](char b) { return b == 0x5a; }) &&
	           write.Close(), "write-only reads reject before altering guest destination")) return false;

	// No wrapper mutex: every worker calls the real guest KernelOpen on the
	// same immutable managed pathname. Native O_EXCL determines one winner.
	std::strcpy(memory.Bytes(256), names[5].c_str());
	constexpr unsigned workers = 8;
	std::atomic<unsigned> ready {0};
	std::atomic<bool> start {false};
	std::array<int, workers> results {};
	std::array<bool, workers> published {};
	std::array<std::thread, workers> threads;
	for (unsigned i = 0; i < workers; ++i)
	{
		threads[i] = std::thread([&, i] {
			++ready;
			while (!start.load()) std::this_thread::yield();
			Descriptor file {FS::KernelOpen(memory.Bytes(256), create_exclusive_rw, 0600)};
			results[i] = file.value;
			if (file.value >= 3) published[i] = FS::KernelWrite(file.value, memory.Bytes(4096), payload.size()) == payload_bytes && file.Close();
		});
	}
	while (ready.load() != workers) std::this_thread::yield();
	start.store(true);
	for (auto& thread: threads) thread.join();
	unsigned winners = 0;
	for (unsigned i = 0; i < workers; ++i)
	{
		if (results[i] >= 3) { ++winners; if (!Check(published[i], "race winner uses and closes creation handle")) return false; }
		else if (!Check(results[i] == KERNEL_ERROR_EEXIST, "race loser returns EEXIST")) return false;
	}
	Descriptor winner {FS::KernelOpen(path(names[5]), 0, 0)};
	return Check(winners == 1 && winner.value >= 3 &&
	             FS::KernelPread(winner.value, memory.Bytes(4112), payload.size(), 0) == payload_bytes &&
	             std::memcmp(memory.Bytes(4112), payload.data(), payload.size()) == 0 && winner.Close(),
	             "one real KernelOpen race winner; losers preserve bytes");
}

bool Run(const char* directory, const char* filename)
{
	const long page_size = sysconf(_SC_PAGESIZE);
	if (!Check(page_size > 0, "host page size")) return false;
	const uint64_t page = static_cast<uint64_t>(page_size);
	Mapping memory(page * 3);
	Mapping tiny(1);
	if (!Check(memory.address != 0 && tiny.address != 0, "managed allocations")) return false;
	std::memset(memory.Bytes(), 0x5a, static_cast<size_t>(page * 3));
	std::strcpy(memory.Bytes(page + 32), filename);
	if (!Check(VM::Protect(memory.address + page, page, VM::Mode::Read) &&
	           VM::Protect(memory.address + page * 2, page, VM::Mode::NoAccess), "R and guard mappings")) return false;
	if (!CheckExclusiveCreation(directory)) return false;

	const int file = FS::KernelOpen(filename, 0, 0);
	const int expected_directory = FS::KernelOpen(directory, 0x00020000, 0);
	const int checked_directory = FS::KernelOpen(directory, 0x00020000, 0);
	if (!Check(file >= 3 && expected_directory >= 3 && checked_directory >= 3, "fixture descriptors")) return false;
	bool ok = true;
	ok = Check(FS::KernelFcntl(file, 1, 0) == 0 &&
	           FS::KernelFcntl(file, 2, 1) == OK && FS::KernelFcntl(file, 1, 0) == 1 &&
	           FS::KernelFcntl(file, 3, 0) == 0 &&
	           FS::KernelFcntl(file, 4, 4) == OK && FS::KernelFcntl(file, 3, 0) == 4,
	           "fcntl tracks descriptor and mutable status flags") && ok;
	ok = Check(FS::KernelFcntl(file, 2, 2) == Lumen::Libs::LibKernel::KERNEL_ERROR_EINVAL &&
	           FS::KernelFcntl(file, 4, 8) == Lumen::Libs::LibKernel::KERNEL_ERROR_EINVAL &&
	           FS::KernelFcntl(file, INT_MAX, 0) == Lumen::Libs::LibKernel::KERNEL_ERROR_EINVAL &&
	           FS::KernelFcntl(-1, 1, 0) == Lumen::Libs::LibKernel::KERNEL_ERROR_EBADF,
	           "fcntl rejects unsupported flags, commands and descriptors") && ok;
	ok = Check(FS::KernelFsync(file) == OK &&
	           FS::KernelFsync(expected_directory) == Lumen::Libs::LibKernel::KERNEL_ERROR_EISDIR &&
	           FS::KernelFsync(-1) == Lumen::Libs::LibKernel::KERNEL_ERROR_EBADF,
	           "fsync flushes files and rejects directories/descriptors") && ok;
	auto* output = reinterpret_cast<FS::FileStat*>(memory.Bytes(128));
	ok = Check(FS::KernelStat(memory.Bytes(page + 32), output) == OK && output->st_size == 7,
	           "stat reads a borrowed R path and publishes size") && ok;
	FS::FileStat native {};
	ok = Check(FS::KernelFstat(file, &native) == OK && native.st_size == 7,
	           "native caller output") && ok;
	ok = Check(FS::KernelLseek(file, 3, 0) == 3, "file position setup") && ok;
	ok = CheckRejectedSeekReleasesFileLock(file) && ok;
	ok = CheckMetadataOverlay(directory, filename, file, expected_directory, memory, page) && ok;

	std::array<char, 128> prefix {};
	std::memcpy(prefix.data(), memory.Bytes(page - prefix.size()), prefix.size());
	const std::array<char*, 5> invalid_outputs {
	    memory.Bytes(page), memory.Bytes(page * 2), memory.Bytes(page - 32),
	    reinterpret_cast<char*>(UINTPTR_MAX - 3), tiny.Bytes()};
	for (auto* pointer: invalid_outputs)
	{
		auto* stat = reinterpret_cast<FS::FileStat*>(pointer);
		ok = Check(FS::KernelStat(filename, stat) == KERNEL_ERROR_EFAULT &&
		           FS::KernelFstat(file, stat) == KERNEL_ERROR_EFAULT,
		           "stat/fstat reject R, guard, cross-page, overflow and padding") && ok;
		ok = Check(std::memcmp(prefix.data(), memory.Bytes(page - prefix.size()), prefix.size()) == 0 &&
		           FS::KernelLseek(file, 0, 1) == 3, "denied stat preserves bytes and file position") && ok;
	}
	std::array<char, sizeof(FS::FileStat)> before_stat {};
	std::memcpy(before_stat.data(), output, before_stat.size());
	for (const char* path: {memory.Bytes(page * 2), memory.Bytes(page * 2 - 2),
	                        reinterpret_cast<char*>(UINTPTR_MAX - 3)})
	{
		ok = Check(FS::KernelStat(path, output) == KERNEL_ERROR_EFAULT &&
		           std::memcmp(before_stat.data(), output, before_stat.size()) == 0,
		           "denied path has no partial stat result") && ok;
	}
	Lumen::Cpu::X64State state {};
	state.gpr[7] = memory.address + page * 2;
	state.gpr[6] = reinterpret_cast<uint64_t>(output);
	Lumen::Loader::SymbolDatabase symbols;
	const auto stat_target = reinterpret_cast<uint64_t>(&FS::KernelStat);
	const bool registered = symbols.RegisterHostCallTarget(stat_target) &&
	    symbols.SetHostCallScalarMetadata(stat_target, Lumen::Loader::MakeHostCallScalarMetadata<&FS::KernelStat>());
	const bool handled = registered && Lumen::Loader::InvokeHostCall(&symbols, stat_target, &state);
	const bool returned_error = handled && state.memory_fault_size == 0 &&
	                            static_cast<int32_t>(state.gpr[0]) == KERNEL_ERROR_EFAULT &&
	                            std::memcmp(before_stat.data(), output, before_stat.size()) == 0;
	if (!returned_error)
		std::fprintf(stderr, "FileSystemGuestMemoryTest: bridge handled=%d result=%016llx fault=%016llx/%u\n",
		             handled, static_cast<unsigned long long>(state.gpr[0]),
		             static_cast<unsigned long long>(state.memory_fault_address), state.memory_fault_size);
	ok = Check(returned_error,
	           "invalid stat path returns EFAULT through host-call bridge without aborting guest") && ok;

	std::array<char, 512> expected {};
	expected.fill(0x5a);
	int64_t expected_position = -1;
	ok = Check(FS::KernelGetdirentries(expected_directory, expected.data(), expected.size(), &expected_position) == 512 &&
	           expected_position == 0, "directory fixture first record") && ok;
	int64_t position = -1;
	for (char* pointer: invalid_outputs)
	{
		ok = Check(FS::KernelGetdirentries(checked_directory, pointer, 512, &position) == KERNEL_ERROR_EFAULT &&
		           position == -1 && std::memcmp(prefix.data(), memory.Bytes(page - prefix.size()), prefix.size()) == 0,
		           "invalid directory output preserves basep, cursor and prefix") && ok;
	}
	auto* record = memory.Bytes(1024);
	std::array<char, 512> before_record {};
	std::memcpy(before_record.data(), record, before_record.size());
	for (char* pointer: {memory.Bytes(page), memory.Bytes(page * 2), memory.Bytes(page - 4),
	                    reinterpret_cast<char*>(UINTPTR_MAX - 3), tiny.Bytes()})
	{
		ok = Check(FS::KernelGetdirentries(checked_directory, record, 512, reinterpret_cast<int64_t*>(pointer)) ==
		               KERNEL_ERROR_EFAULT &&
		           std::memcmp(before_record.data(), record, before_record.size()) == 0,
		           "invalid basep preserves directory output and cursor") && ok;
	}
	ok = Check(FS::KernelGetdirentries(checked_directory, record, 512, &position) == 512 && position == 0 &&
	           std::memcmp(record, expected.data(), expected.size()) == 0,
	           "first successful directory record survives all rejected calls") && ok;
	// Compare getdents and getdirentries through EOF. Both must consume the
	// same entries; no directory bytes are touched when no record remains.
	for (unsigned count = 0; count != 16; ++count)
	{
		expected.fill(0x5a);
		std::memset(record, 0x5a, 512);
		const int baseline = FS::KernelGetdents(expected_directory, expected.data(), 512);
		const int actual = FS::KernelGetdents(checked_directory, record, 512);
		ok = Check(actual == baseline && std::memcmp(record, expected.data(), expected.size()) == 0,
		           "getdents records and EOF agree") && ok;
		if (actual == 0)
		{
			ok = Check(FS::KernelGetdents(checked_directory, memory.Bytes(page * 2), 512) == 0,
			           "EOF does not borrow or access a directory record") && ok;
			break;
		}
		ok = Check(count != 15, "directory reaches EOF") && ok;
	}
	ok = Check(FS::KernelChmod(filename, 0111) == OK, "install metadata before reset") && ok;
	ok = Check(FS::KernelClose(file) == OK &&
	           FS::KernelClose(expected_directory) == OK &&
	           FS::KernelClose(checked_directory) == OK, "close fixture descriptors") && ok;
	ok = Check(FS::ResetForNextSession(), "first session reset") && ok;
	FS::FileStat after_reset {};
	ok = Check(FS::KernelStat(filename, &after_reset) == OK &&
	           (after_reset.st_mode & 0007777u) == 0777u,
	           "session reset clears metadata overlay") && ok;
	ok = Check(FS::ResetForNextSession(), "second session reset is idempotent") && ok;
	return ok;
}
} // namespace

int main()
{
	Lumen::Core::ThreadsSubsystem::Instance()->Init(nullptr);
	Lumen::Core::core_file_init();
	Lumen::Config::ConfigSubsystem::Instance()->Init(nullptr);
	Lumen::Log::LogSubsystem::Instance()->Init(nullptr);
	Lumen::Log::SetDirection(Lumen::Log::Direction::Silent);
	FS::FileSystemSubsystem::Instance()->Init(nullptr);
	if (!CheckKernelFileBindings()) return 1;
	char directory[] = "/tmp/lumen-fs-memory-XXXXXX";
	if (!Check(mkdtemp(directory) != nullptr, "temporary directory")) return 1;
	const std::string filename = std::string(directory) + "/fixture.bin";
	auto* file = std::fopen(filename.c_str(), "wb");
	if (!Check(file != nullptr, "temporary file")) { rmdir(directory); return 1; }
	const bool written = std::fwrite("fixture", 1, 7, file) == 7;
	std::fclose(file);
	const bool ok = written && Run(directory, filename.c_str());
	(void)FS::ResetForNextSession();
	unlink(filename.c_str());
	rmdir(directory);
	if (ok) std::puts("FileSystemGuestMemoryTest: PASS");
	return ok ? 0 : 1;
}
