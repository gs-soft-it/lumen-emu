// SPDX-License-Identifier: GPL-2.0-only
#include "common/arm64MemoryAccess.h"
#include <atomic>
#include <cstring>
#include <type_traits>

#if defined(__aarch64__) || defined(__arm64__)
namespace Common::Arm64MemoryAccess {
namespace {
uint64_t Reg(const State& s, unsigned r) { return r == 31 ? 0 : s.x[r]; }
void Set(State& s, unsigned r, uint64_t value) { if (r != 31) s.x[r] = value; }
int64_t Signed(uint64_t value, unsigned bits) {
    const uint64_t sign = uint64_t{1} << (bits - 1);
    return static_cast<int64_t>((value ^ sign) - sign);
}
uint64_t Load(const void* p, size_t n) {
    // A naturally aligned scalar access is single-copy atomic. A dynamic-sized
    // memcpy is allowed to split it into smaller transfers, even around DMBs.
    // Keep unaligned accesses on the established byte-copy path.
    if ((reinterpret_cast<uintptr_t>(p) & (n - 1)) == 0) {
        switch (n) {
        case 1: return __atomic_load_n(static_cast<const uint8_t*>(p), __ATOMIC_RELAXED);
        case 2: return __atomic_load_n(static_cast<const uint16_t*>(p), __ATOMIC_RELAXED);
        case 4: return __atomic_load_n(static_cast<const uint32_t*>(p), __ATOMIC_RELAXED);
        case 8: return __atomic_load_n(static_cast<const uint64_t*>(p), __ATOMIC_RELAXED);
        }
    }
    uint64_t value = 0; std::memcpy(&value, p, n); return value;
}
void Store(void* p, uint64_t value, size_t n) {
    if ((reinterpret_cast<uintptr_t>(p) & (n - 1)) == 0) {
        switch (n) {
        case 1: __atomic_store_n(static_cast<uint8_t*>(p), static_cast<uint8_t>(value), __ATOMIC_RELAXED); return;
        case 2: __atomic_store_n(static_cast<uint16_t*>(p), static_cast<uint16_t>(value), __ATOMIC_RELAXED); return;
        case 4: __atomic_store_n(static_cast<uint32_t*>(p), static_cast<uint32_t>(value), __ATOMIC_RELAXED); return;
        case 8: __atomic_store_n(static_cast<uint64_t*>(p), value, __ATOMIC_RELAXED); return;
        }
    }
    std::memcpy(p, &value, n);
}
void CopyScalar(void* destination, const void* source, size_t n) {
    if (n <= sizeof(uint64_t)) Store(destination, Load(source, n), n);
    else std::memcpy(destination, source, n);
}
bool Operand(Resolve resolve, void* opaque, uint64_t fault, uint64_t address,
             size_t size, Access access, void** host) {
    return size && size <= UINT64_MAX - address && fault >= address &&
           fault - address < size && resolve(opaque, address, size, access, host);
}

template<typename T>
void AtomicOp(unsigned operation, void* host, uint64_t source, uint64_t expected,
              uint64_t* old, bool* exchanged) {
    auto* target = static_cast<T*>(host);
    const T src = static_cast<T>(source);
    T prior = static_cast<T>(expected);
    if (operation == 9) { // Compare and swap; failed comparison still returns the observed value.
        *exchanged = __atomic_compare_exchange_n(target, &prior, src, false,
                                                 __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    } else if (operation == 10) {
        prior = __atomic_load_n(target, __ATOMIC_SEQ_CST);
    } else if (operation == 11) {
        __atomic_store_n(target, src, __ATOMIC_SEQ_CST); prior = 0;
    } else {
        prior = __atomic_load_n(target, __ATOMIC_SEQ_CST);
        for (;;) {
            T next = src;
            switch (operation) {
            case 0: next = static_cast<T>(prior + src); break;
            case 1: next = prior & ~src; break;
            case 2: next = prior ^ src; break;
            case 3: next = prior | src; break;
            case 4: next = static_cast<std::make_signed_t<T>>(prior) >
                           static_cast<std::make_signed_t<T>>(src) ? prior : src; break;
            case 5: next = static_cast<std::make_signed_t<T>>(prior) <
                           static_cast<std::make_signed_t<T>>(src) ? prior : src; break;
            case 6: next = prior > src ? prior : src; break;
            case 7: next = prior < src ? prior : src; break;
            case 8: break; // SWP
            }
            T observed = prior;
            if (__atomic_compare_exchange_n(target, &observed, next, false,
                                             __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) break;
            prior = observed;
        }
    }
    *old = prior;
}
bool Atomic(size_t size, unsigned op, void* host, uint64_t source, uint64_t expected,
            uint64_t* old, bool* exchanged) {
    // Do not turn an unaligned operand into a non-atomic memcpy. Such an opcode
    // remains a genuine fault until a split-lock implementation is available.
    if (reinterpret_cast<uintptr_t>(host) % size) return false;
    switch (size) {
    case 1: AtomicOp<uint8_t>(op, host, source, expected, old, exchanged); return true;
    case 2: AtomicOp<uint16_t>(op, host, source, expected, old, exchanged); return true;
    case 4: AtomicOp<uint32_t>(op, host, source, expected, old, exchanged); return true;
    case 8: AtomicOp<uint64_t>(op, host, source, expected, old, exchanged); return true;
    default: return false;
    }
}
#if defined(__aarch64__) || defined(__arm64__)
using Wide = unsigned __int128;
static_assert(__atomic_always_lock_free(sizeof(Wide), nullptr));
bool ComparePair(void* host, size_t size, const void* expected, const void* desired,
                 void* observed) {
    if (reinterpret_cast<uintptr_t>(host) % size) return false;
    if (size == 16) {
        Wide before, after;
        std::memcpy(&before, expected, 16); std::memcpy(&after, desired, 16);
        const bool success = __atomic_compare_exchange_n(static_cast<Wide*>(host), &before,
            after, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
        std::memcpy(observed, &before, 16); return success;
    }
    uint64_t before, after;
    std::memcpy(&before, expected, 8); std::memcpy(&after, desired, 8);
    const bool success = __atomic_compare_exchange_n(static_cast<uint64_t*>(host), &before,
        after, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    std::memcpy(observed, &before, 8); return success;
}
#endif
}

bool Execute(uint32_t i, uint64_t fault, State& s, Resolve resolve, void* opaque) noexcept {
    if (!resolve) return false;
    const unsigned rn = (i >> 5) & 31, rt = i & 31, rs = (i >> 16) & 31;
    const unsigned size_code = i >> 30, opc = (i >> 22) & 3;
    const uint64_t base = s.x[rn];
    void* host = nullptr;
#if defined(__aarch64__) || defined(__arm64__)
    if ((i & 0xffffffe0u) == 0xd50b7420u) { // DC ZVA, used by native memset.
        uint64_t dczid = 0;
        asm volatile("mrs %0, dczid_el0" : "=r"(dczid));
        if (dczid & 16) return false;
        const size_t size = size_t{4} << (dczid & 15);
        const uint64_t address = Reg(s, rt) & ~(uint64_t{size} - 1);
        if (!Operand(resolve, opaque, fault, address, size, Access::Write, &host)) return false;
        std::atomic_thread_fence(std::memory_order_seq_cst);
        std::memset(host, 0, size);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        return true;
    }
#endif
    // CAS and the LSE read-modify-write family.
    const bool cas = (i & 0x3fa07c00u) == 0x08a07c00u;
    const bool ldar = (i & 0x3ffffc00u) == 0x08dffc00u ||
                      (i & 0x3ffffc00u) == 0x38bfc000u;
    const bool stlr = (i & 0x3ffffc00u) == 0x089ffc00u;
    const bool lse = (i & 0x3f200c00u) == 0x38200000u;
    if (cas || ldar || stlr || lse) {
        const size_t size = size_t{1} << size_code;
        const unsigned operation = cas ? 9 : ldar ? 10 : stlr ? 11 : (i >> 12) & 15;
        if (operation > 11 || (lse && !ldar && operation > 8)) return false;
        const auto access = ldar ? Access::Read : stlr ? Access::Write : Access::ReadWrite;
        if (!Operand(resolve, opaque, fault, base, size, access, &host) ||
            reinterpret_cast<uintptr_t>(host) % size) return false;
        uint64_t old = 0; bool exchanged = false;
        const uint64_t source = cas || stlr ? Reg(s, rt) : Reg(s, rs);
        const uint64_t expected = Reg(s, rs);
        if (!Atomic(size, operation, host, source, expected, &old, &exchanged)) return false;
        if (cas) Set(s, rs, old);
        else if (!stlr) Set(s, rt, old);
        return true;
    }
#if defined(__aarch64__) || defined(__arm64__)
    if ((i & 0xbfa07c00u) == 0x08207c00u) { // CASP
        if ((rs | rt) & 1) return false;
        const size_t element = size_t{4} << ((i >> 30) & 1), size = element * 2;
        if (!Operand(resolve, opaque, fault, base, size, Access::ReadWrite, &host) ||
            reinterpret_cast<uintptr_t>(host) % size) return false;
        alignas(16) std::array<std::byte, 16> expected {}, desired {}, observed {};
        uint64_t lo = Reg(s, rt), hi = Reg(s, rt + 1);
        std::memcpy(desired.data(), &lo, element); std::memcpy(desired.data()+element, &hi, element);
        lo = Reg(s, rs); hi = Reg(s, rs + 1);
        std::memcpy(expected.data(), &lo, element); std::memcpy(expected.data()+element, &hi, element);
        ComparePair(host, size, expected.data(), desired.data(), observed.data());
        Set(s, rs, Load(observed.data(), element)); Set(s, rs+1, Load(observed.data()+element, element));
        return true;
    }
#endif
    // LDP/STP, LDNP/STNP and LDPSW. Offset/pre/post-index modes, scalar or SIMD.
    if ((i & 0x3a000000u) == 0x28000000u) {
        const bool simd = (i & (1u << 26)) != 0, load = (i & (1u << 22)) != 0;
        const unsigned mode = (i >> 23) & 3, rt2 = (i >> 10) & 31;
        const bool sign = !simd && size_code == 1;
        if (size_code == 3 || (sign && (!load || mode == 0))) return false;
        const size_t element = simd ? size_t{4} << size_code : size_code == 2 ? 8 : 4;
        const int64_t offset = Signed((i >> 15) & 127, 7) * static_cast<int64_t>(element);
        const bool writeback = mode == 1 || mode == 3;
        if (!simd && writeback && rn != 31 && (rn == rt || rn == rt2)) return false;
        if (load && rt == rt2) return false;
        const uint64_t address = mode == 1 ? base : base + offset;
        if (!Operand(resolve, opaque, fault, address, 2*element,
                     load ? Access::Read : Access::Write, &host)) return false;
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (simd) {
            if (load) {
                s.v[rt].fill(std::byte{}); s.v[rt2].fill(std::byte{});
                CopyScalar(s.v[rt].data(), host, element);
                CopyScalar(s.v[rt2].data(), static_cast<std::byte*>(host)+element, element);
            } else {
                CopyScalar(host, s.v[rt].data(), element);
                CopyScalar(static_cast<std::byte*>(host)+element, s.v[rt2].data(), element);
            }
        } else if (load) {
            uint64_t lo = Load(host, element), hi = Load(static_cast<std::byte*>(host)+element, element);
            if (sign) { lo = Signed(lo, 32); hi = Signed(hi, 32); }
            Set(s, rt, lo); Set(s, rt2, hi);
        } else {
            const uint64_t lo = Reg(s, rt), hi = Reg(s, rt2);
            Store(host, lo, element); Store(static_cast<std::byte*>(host)+element, hi, element);
        }
        if (writeback) s.x[rn] = base + offset;
        std::atomic_thread_fence(std::memory_order_seq_cst); return true;
    }
    const bool unsigned_offset = (i & 0x3b000000u) == 0x39000000u;
    const bool signed_offset = (i & 0x3b200000u) == 0x38000000u;
    const bool register_offset = (i & 0x3b200c00u) == 0x38200800u;
    const bool rcpc = (i & 0x3b200c00u) == 0x19000000u;
    if (unsigned_offset || signed_offset || register_offset || rcpc) {
        const bool simd = (i & (1u << 26)) != 0;
        const unsigned scale = simd ? size_code + ((opc >> 1) * 4) : size_code;
        const bool load = simd ? (opc & 1) : opc != 0;
        const bool sign = !simd && opc >= 2;
        if (scale > 4 || (!simd && ((size_code == 3 && sign) || (size_code == 2 && opc == 3))) ||
            (simd && rcpc)) return false;
        const size_t size = size_t{1} << scale;
        int64_t offset = 0; bool writeback = false, post = false;
        if (unsigned_offset) offset = static_cast<int64_t>((i >> 10) & 4095) << scale;
        else if (register_offset) {
            const unsigned option = (i >> 13) & 7;
            uint64_t index = Reg(s, rs);
            if (option == 2) index = static_cast<uint32_t>(index);
            else if (option == 6) index = Signed(static_cast<uint32_t>(index), 32);
            else if (option != 3 && option != 7) return false;
            offset = static_cast<int64_t>(index << (((i >> 12) & 1) ? scale : 0));
        } else {
            offset = Signed((i >> 12) & 511, 9);
            const unsigned mode = (i >> 10) & 3;
            if (rcpc && mode != 0) return false;
            writeback = !rcpc && (mode == 1 || mode == 3); post = mode == 1;
            if (!simd && writeback && rn == rt && rn != 31) return false;
        }
        const uint64_t address = post ? base : base + offset;
        if (!Operand(resolve, opaque, fault, address, size, load ? Access::Read : Access::Write, &host)) return false;
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (simd) {
            if (load) { s.v[rt].fill(std::byte{}); CopyScalar(s.v[rt].data(), host, size); }
            else CopyScalar(host, s.v[rt].data(), size);
        } else if (load) {
            uint64_t value = Load(host, size);
            if (sign) value = Signed(value, size*8);
            if (sign && opc == 3) value = static_cast<uint32_t>(value);
            Set(s, rt, value);
        } else {
            Store(host, Reg(s, rt), size);
        }
        if (writeback) s.x[rn] = base + offset;
        std::atomic_thread_fence(std::memory_order_seq_cst); return true;
    }
    // Advanced SIMD multiple structures, including the LD1/ST1 sequences used
    // by native memcpy/memset. Deinterleave only the requested element lanes.
    if ((i & 0xbf200000u) == 0x0c000000u) {
        const bool load = (i & (1u << 22)) != 0, post = (i & (1u << 23)) != 0;
        const unsigned opcode = (i >> 12) & 15, scale = (i >> 10) & 3;
        unsigned repeats = 1, structures = 1;
        switch (opcode) {
        case 7: break; case 10: repeats=2; break; case 6: repeats=3; break; case 2: repeats=4; break;
        case 8: structures=2; break; case 4: structures=3; break; case 0: structures=4; break;
        default: return false;
        }
        const size_t vector_size = (i & (1u<<30)) ? 16 : 8, element = size_t{1} << scale;
        if (element == 8 && vector_size == 8 && structures > 1) return false;
        const size_t size = vector_size * repeats * structures;
        if (!Operand(resolve, opaque, fault, base, size, load ? Access::Read : Access::Write, &host)) return false;
        std::array<std::byte,64> data{};
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (load) std::memcpy(data.data(), host, size);
        size_t cursor=0;
        for (unsigned r=0;r<repeats;++r) {
            if (load) for (unsigned j=0;j<structures;++j) s.v[(rt+r*structures+j)&31].fill(std::byte{});
            for (size_t lane=0;lane<vector_size;lane+=element) for (unsigned j=0;j<structures;++j) {
                auto* vector=s.v[(rt+r*structures+j)&31].data()+lane;
                if (load) std::memcpy(vector,data.data()+cursor,element);
                else std::memcpy(data.data()+cursor,vector,element);
                cursor+=element;
            }
        }
        if (!load) std::memcpy(host,data.data(),size);
        if (post) s.x[rn]=base+(rs==31?size:Reg(s,rs));
        std::atomic_thread_fence(std::memory_order_seq_cst); return true;
    }
    return false;
}
}

#else
namespace Common::Arm64MemoryAccess {
bool Execute(uint32_t, uint64_t, State&, Resolve, void*) noexcept { return false; }
}
#endif
