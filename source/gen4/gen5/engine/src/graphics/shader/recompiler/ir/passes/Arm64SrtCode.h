// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <span>
#include <vector>

#if defined(__aarch64__) && (defined(__linux__) || defined(__APPLE__))
#include <sys/mman.h>
#include <unistd.h>
#ifdef __APPLE__
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#endif
#endif

namespace Libs::Graphics::ShaderRecompiler::IR {

// A resource plan owns its code for as long as its shaders can use the walker.
// Publication is W^X; inability to allocate executable memory leaves the portable
// evaluator available. No new signal handler or guest mapping is installed here.
class Arm64SrtCode final {
public:
    using Walker = bool (*)(const uint32_t*, uint32_t*, void*, uint64_t, size_t);

    static std::shared_ptr<const Arm64SrtCode> Create(std::span<const uint32_t> words) {
#if defined(__aarch64__) && (defined(__linux__) || defined(__APPLE__))
        if (words.empty() || words.size_bytes() > 256 * 1024) return {};
        const auto page = ::sysconf(_SC_PAGESIZE);
        if (page <= 0) return {};
        const auto size = (words.size_bytes() + page - 1) / page * page;
        int permissions = PROT_READ | PROT_WRITE;
        int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#ifdef __APPLE__
        permissions |= PROT_EXEC;
        flags |= MAP_JIT;
#endif
        auto* mapping = ::mmap(nullptr, size, permissions, flags, -1, 0);
        if (mapping == MAP_FAILED) return {};
#ifdef __APPLE__
        pthread_jit_write_protect_np(0);
        std::memcpy(mapping, words.data(), words.size_bytes());
        sys_icache_invalidate(mapping, words.size_bytes());
        pthread_jit_write_protect_np(1);
#else
        std::memcpy(mapping, words.data(), words.size_bytes());
        __builtin___clear_cache(static_cast<char*>(mapping),
                                static_cast<char*>(mapping) + words.size_bytes());
        if (::mprotect(mapping, size, PROT_READ | PROT_EXEC) != 0) {
            ::munmap(mapping, size);
            return {};
        }
#endif
        auto* owner = new (std::nothrow) Arm64SrtCode(mapping, size);
        if (owner == nullptr) {
            ::munmap(mapping, size);
            return {};
        }
        try {
            return std::shared_ptr<const Arm64SrtCode>(owner);
        } catch (const std::bad_alloc&) {
            // shared_ptr deletes owner if allocating its control block fails.
            return {};
        }
#else
        (void)words;
        return {};
#endif
    }

    ~Arm64SrtCode() {
#if defined(__aarch64__) && (defined(__linux__) || defined(__APPLE__))
        ::munmap(m_mapping, m_size);
#endif
    }

    Walker Function() const { return reinterpret_cast<Walker>(m_mapping); }

private:
    Arm64SrtCode(void* mapping, size_t size): m_mapping(mapping), m_size(size) {}
    void* m_mapping;
    size_t m_size;
};

// AAPCS64: x0=user data, x1=flat output, x2=memo, x3=generation,
// x4=user-data count. x5 is the current table; x6-x8 are scratch. Only
// caller-saved registers change, and every nested table keeps SP 16-byte aligned.
class Arm64SrtEmitter final {
public:
    void Begin(uint32_t user_data_count) {
        Emit(0xd503245f); // bti c (a hint on hosts without BTI)
        Emit(0xa9bf7bfd); // stp x29, x30, [sp, #-16]!
        Emit(0x910003fd); // mov x29, sp
        Emit(0xaa0003e5); // mov x5, x0
        Immediate(6, user_data_count);
        Compare(4, 6);
        FailIf(3); // lo
    }

    void EnterTable(int64_t pointer_offset, int32_t first, int32_t last) {
        Emit(0xf81f0fe5); // str x5, [sp, #-16]!
        Memory(0xf9400000, 5, 5, pointer_offset, 8); // ldr x5, [x5 + offset]
        Immediate(8, AddressMask & ~uint64_t{3});
        Emit(0x8a0800a5); // and x5, x5, x8
        if (first < 0) {
            Immediate(6, -static_cast<int64_t>(first));
            Compare(5, 6);
            FailIf(3); // lo: negative offset would underflow
        }
        if (last > 0) {
            Immediate(6, AddressMask - static_cast<uint32_t>(last));
            Compare(5, 6);
            FailIf(8); // hi: positive offset would overflow the guest address
        }
    }

    void Read(int32_t offset, uint32_t flat_offset, size_t memo_offset,
              size_t value_offset, size_t generation_offset) {
        Memory(0xb9400000, 7, 5, offset, 4); // ldr w7
        Memory(0xb9000000, 7, 1, uint64_t{flat_offset} * 4, 4); // str w7
        Memory(0xf9000000, 7, 2, memo_offset + value_offset, 8); // str x7
        Memory(0xf9000000, 3, 2, memo_offset + generation_offset, 8); // str x3
    }

    void LeaveTable() { Emit(0xf84107e5); } // ldr x5, [sp], #16

    std::shared_ptr<const Arm64SrtCode> Finish() {
        Immediate(0, 1);
        const auto success = m_words.size();
        Emit(0x14000000); // b epilogue
        const auto failure = m_words.size();
        Immediate(0, 0);
        const auto epilogue = m_words.size();
        Emit(0x910003bf); // mov sp, x29 (also unwinds nested tables on failure)
        Emit(0xa8c17bfd); // ldp x29, x30, [sp], #16
        Emit(0xd65f03c0); // ret
        if (m_overflow) return {};
        for (const auto site: m_failures) {
            m_words[site] |= static_cast<uint32_t>(failure - site) << 5;
        }
        m_words[success] |= static_cast<uint32_t>(epilogue - success);
        return Arm64SrtCode::Create(m_words);
    }

private:
    static constexpr uint64_t AddressMask = 0x0000ffffffffffffull;
    static constexpr size_t MaxWords = 65536;

    void Emit(uint32_t word) {
        if (m_words.size() == MaxWords) { m_overflow = true; return; }
        m_words.push_back(word);
    }
    void Immediate(uint32_t reg, uint64_t value) {
        Emit(0xd2800000 | (static_cast<uint32_t>(value & 0xffff) << 5) | reg);
        for (uint32_t half = 1; half < 4; ++half) {
            const auto bits = static_cast<uint32_t>((value >> (half * 16)) & 0xffff);
            if (bits != 0) Emit(0xf2800000 | (half << 21) | (bits << 5) | reg);
        }
    }
    void Compare(uint32_t left, uint32_t right) {
        Emit(0xeb00001f | (right << 16) | (left << 5));
    }
    void FailIf(uint32_t condition) {
        m_failures.push_back(m_words.size());
        Emit(0x54000000 | condition);
    }
    void Memory(uint32_t opcode, uint32_t reg, uint32_t base, int64_t offset,
                uint32_t scale) {
        if (offset >= 0 && offset % scale == 0 && offset / scale < 4096) {
            Emit(opcode | (static_cast<uint32_t>(offset / scale) << 10) | (base << 5) | reg);
        } else {
            Immediate(6, static_cast<uint64_t>(offset));
            Emit(0x8b060000 | (base << 5) | 6); // add x6, base, x6
            Emit(opcode | (6 << 5) | reg);
        }
    }

    std::vector<uint32_t> m_words;
    std::vector<size_t> m_failures;
    bool m_overflow = false;
};

} // namespace Libs::Graphics::ShaderRecompiler::IR
