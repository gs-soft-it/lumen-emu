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
    using MemoryReader = bool (*)(void*, uint64_t, uint32_t*, size_t);
    using CheckedWalker = bool (*)(const uint32_t*, uint32_t*, void*, uint64_t,
                                  size_t, MemoryReader, void*);

    static std::shared_ptr<const Arm64SrtCode> Create(std::span<const uint32_t> words,
                                                    std::span<const uint32_t> checked = {}) {
#if defined(__aarch64__) && (defined(__linux__) || defined(__APPLE__))
        if (words.empty() || words.size_bytes() > 256 * 1024 ||
            checked.size_bytes() > 256 * 1024) return {};
        const auto checked_offset = checked.empty() ? 0 : (words.size_bytes() + 15) & ~size_t{15};
        const auto code_size = checked.empty() ? words.size_bytes() : checked_offset + checked.size_bytes();
        const auto page = ::sysconf(_SC_PAGESIZE);
        if (page <= 0) return {};
        const auto size = (code_size + page - 1) / page * page;
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
        if (!checked.empty()) std::memcpy(static_cast<char*>(mapping) + checked_offset,
                                         checked.data(), checked.size_bytes());
        sys_icache_invalidate(mapping, code_size);
        pthread_jit_write_protect_np(1);
#else
        std::memcpy(mapping, words.data(), words.size_bytes());
        if (!checked.empty()) std::memcpy(static_cast<char*>(mapping) + checked_offset,
                                         checked.data(), checked.size_bytes());
        __builtin___clear_cache(static_cast<char*>(mapping),
                                static_cast<char*>(mapping) + code_size);
        if (::mprotect(mapping, size, PROT_READ | PROT_EXEC) != 0) {
            ::munmap(mapping, size);
            return {};
        }
#endif
        auto* owner = new (std::nothrow) Arm64SrtCode(mapping, size, checked_offset);
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
        (void)checked;
        return {};
#endif
    }

    ~Arm64SrtCode() {
#if defined(__aarch64__) && (defined(__linux__) || defined(__APPLE__))
        ::munmap(m_mapping, m_size);
#endif
    }

    Walker Function() const { return reinterpret_cast<Walker>(m_mapping); }
    CheckedWalker CheckedFunction() const {
        return m_checked_offset == 0 ? nullptr
            : reinterpret_cast<CheckedWalker>(static_cast<char*>(m_mapping) + m_checked_offset);
    }

private:
    Arm64SrtCode(void* mapping, size_t size, size_t checked_offset):
        m_mapping(mapping), m_size(size), m_checked_offset(checked_offset) {}
    void* m_mapping;
    size_t m_size;
    size_t m_checked_offset;
};

// AAPCS64: x0=user data, x1=flat output, x2=memo, x3=generation,
// x4=user-data count. x5 is the current table; x6-x8 are scratch. Only
// caller-saved registers change, and every nested table keeps SP 16-byte aligned.
// The checked entry additionally takes x5=reader and x6=context, and keeps flat,
// memo, generation, reader, context and table in saved x19-x24 across callbacks.
class Arm64SrtEmitter final {
public:
    explicit Arm64SrtEmitter(bool checked = false): m_checked(checked) {}

    void Begin(uint32_t user_data_count) {
        Emit(0xd503245f); // bti c (a hint on hosts without BTI)
        Emit(0xa9bf7bfd); // stp x29, x30, [sp, #-16]!
        Emit(0x910003fd); // mov x29, sp
        if (m_checked) {
            Emit(0xa9bf53f3); // stp x19, x20, [sp, #-16]!
            Emit(0xa9bf5bf5); // stp x21, x22, [sp, #-16]!
            Emit(0xa9bf63f7); // stp x23, x24, [sp, #-16]!
            Move(19, 1);
            Move(20, 2);
            Move(21, 3);
            Move(22, 5);
            Move(23, 6);
            Move(24, 0);
        } else {
            Emit(0xaa0003e5); // mov x5, x0
        }
        Immediate(6, user_data_count);
        Compare(4, 6);
        FailIf(3); // lo
        if (m_checked) {
            Compare(22, 31);
            FailIf(0); // eq: checked entry requires a reader
        }
    }

    void EnterTable(int64_t pointer_offset, int32_t first, int32_t last,
                    size_t low_memo_offset = SIZE_MAX, size_t high_memo_offset = SIZE_MAX) {
        const auto table = TableRegister();
        Emit(0xf81f0fe0 | table); // str table, [sp, #-16]!
        if (m_checked && low_memo_offset != SIZE_MAX) {
            // Both pointer DWORDs were already checked and memoized in the
            // parent table. Reuse that snapshot instead of rereading guest RAM.
            Memory(0xb9400000, 7, 20, low_memo_offset, 4);
            Memory(0xb9400000, 8, 20, high_memo_offset, 4);
            Emit(0xaa0880f8); // orr x24, x7, x8, lsl #32
        } else {
            // The checked entry uses this only for bounded host user data.
            Memory(0xf9400000, table, table, pointer_offset, 8);
        }
        Immediate(8, AddressMask & ~uint64_t{3});
        Emit(0x8a080000 | (table << 5) | table); // and table, table, x8
        if (first < 0) {
            Immediate(6, -static_cast<int64_t>(first));
            Compare(table, 6);
            FailIf(3); // lo: negative offset would underflow
        }
        if (last > 0) {
            Immediate(6, AddressMask - static_cast<uint32_t>(last));
            Compare(table, 6);
            FailIf(8); // hi: positive offset would overflow the guest address
        }
    }

    void Read(int32_t offset, uint32_t flat_offset, size_t memo_offset,
              size_t value_offset, size_t generation_offset) {
        if (m_checked) {
            Emit(0xd10043ff); // sub sp, sp, #16 (callback output)
            Move(0, 23);
            Immediate(6, static_cast<uint64_t>(offset));
            Emit(0x8b060301); // add x1, x24, x6
            Emit(0x910003e2); // mov x2, sp
            Immediate(3, 1);
            Emit(0xd63f02c0); // blr x22
            Emit(0x7200001f); // tst w0, #1 (AAPCS64 bool leaves upper bits unspecified)
            FailIf(0); // eq: no publication on rejection
            Emit(0xb94003e7); // ldr w7, [sp]
            Emit(0x910043ff); // add sp, sp, #16
        } else {
            Memory(0xb9400000, 7, 5, offset, 4); // ldr w7
        }
        Memory(0xb9000000, 7, m_checked ? 19 : 1, uint64_t{flat_offset} * 4, 4);
        Memory(0xf9000000, 7, m_checked ? 20 : 2, memo_offset + value_offset, 8);
        Memory(0xf9000000, m_checked ? 21 : 3, m_checked ? 20 : 2,
               memo_offset + generation_offset, 8);
    }

    void LeaveTable() { Emit(0xf84107e0 | TableRegister()); } // ldr table, [sp], #16

    std::shared_ptr<const Arm64SrtCode> Finish(Arm64SrtEmitter* checked = nullptr) {
        if (!Finalize()) return {};
        std::span<const uint32_t> checked_words;
        try {
            if (checked != nullptr && checked->Finalize()) checked_words = checked->m_words;
        } catch (const std::bad_alloc&) {
            // Preserve the finished raw variant if the checked epilogue cannot grow.
        }
        auto result = Arm64SrtCode::Create(m_words, checked_words);
        if (result == nullptr && !checked_words.empty()) result = Arm64SrtCode::Create(m_words);
        return result;
    }

private:
    static constexpr uint64_t AddressMask = 0x0000ffffffffffffull;
    static constexpr size_t MaxWords = 65536;

    uint32_t TableRegister() const { return m_checked ? 24 : 5; }

    bool Finalize() {
        Immediate(0, 1);
        const auto success = m_words.size();
        Emit(0x14000000); // b epilogue
        const auto failure = m_words.size();
        Immediate(0, 0);
        const auto epilogue = m_words.size();
        if (m_checked) {
            Emit(0xd100c3bf); // sub sp, x29, #48 (unwind callback and nested frames)
            Emit(0xa8c163f7); // ldp x23, x24, [sp], #16
            Emit(0xa8c15bf5); // ldp x21, x22, [sp], #16
            Emit(0xa8c153f3); // ldp x19, x20, [sp], #16
        } else {
            Emit(0x910003bf); // mov sp, x29 (also unwinds nested tables on failure)
        }
        Emit(0xa8c17bfd); // ldp x29, x30, [sp], #16
        Emit(0xd65f03c0); // ret
        if (m_overflow) return false;
        for (const auto site: m_failures) {
            m_words[site] |= static_cast<uint32_t>(failure - site) << 5;
        }
        m_words[success] |= static_cast<uint32_t>(epilogue - success);
        return true;
    }

    void Move(uint32_t to, uint32_t from) { Emit(0xaa0003e0 | (from << 16) | to); }
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
    bool m_checked;
    bool m_overflow = false;
};

} // namespace Libs::Graphics::ShaderRecompiler::IR
