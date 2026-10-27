// SPDX-License-Identifier: GPL-2.0-only
#include "common/arm64MemoryAccess.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

namespace {
using namespace Common::Arm64MemoryAccess;
constexpr uint64_t Guest = 0x0fe0040000ull;
unsigned checks = 0;
void Require(bool value, const char* label) {
    ++checks;
    if (!value) { std::fprintf(stderr, "ARM64 memory contract failed: %s\n", label); std::exit(1); }
}
uint64_t Mask(size_t size) { return size == 8 ? UINT64_MAX : (uint64_t{1} << (size * 8)) - 1; }
uint64_t Word(const std::byte* bytes, size_t size) {
    uint64_t value = 0; std::memcpy(&value, bytes, size); return value;
}
void Put(std::byte* bytes, uint64_t value, size_t size) { std::memcpy(bytes, &value, size); }
bool Same(const State& a, const State& b) { return a.x == b.x && a.v == b.v; }

struct Memory {
    alignas(64) std::array<std::byte, 256> bytes{};
    size_t limit = bytes.size();
    bool read = true, write = true;
    unsigned calls = 0;
    uint64_t address = 0;
    size_t width = 0;
    Access access = Access::Read;
    Memory() {
        for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = std::byte((i * 17 + 0x83) & 255);
    }
    static bool Resolve(void* opaque, uint64_t address, size_t size, Access access,
                        void** host) noexcept {
        auto& m = *static_cast<Memory*>(opaque);
        ++m.calls; m.address = address; m.width = size; m.access = access;
        if (address < Guest || address - Guest >= m.limit || size > m.limit - (address - Guest) ||
            (access != Access::Write && !m.read) || (access != Access::Read && !m.write)) return false;
        *host = m.bytes.data() + (address - Guest); return true;
    }
};
State Registers() {
    State s;
    for (size_t r = 0; r < s.x.size(); ++r) {
        s.x[r] = 0xfedcba9876543200ull + r;
        for (size_t lane = 0; lane < 16; ++lane) s.v[r][lane] = std::byte((r * 19 + lane * 3) & 255);
    }
    return s;
}
void Success(uint32_t opcode, uint64_t fault, State& state, Memory& memory,
             State expected, std::array<std::byte, 256> bytes,
             uint64_t address, size_t width, Access access, const char* label) {
    Require(Execute(opcode, fault, state, Memory::Resolve, &memory), label);
    Require(Same(state, expected) && memory.bytes == bytes, label);
    Require(memory.calls == 1 && memory.address == address && memory.width == width &&
                memory.access == access, "one complete operand resolved before access");
}
void Rejected(uint32_t opcode, uint64_t fault, State state, Memory memory, const char* label) {
    const auto old_state = state;
    const auto old_bytes = memory.bytes;
    Require(!Execute(opcode, fault, state, Memory::Resolve, &memory), label);
    Require(Same(state, old_state) && memory.bytes == old_bytes, "rejection preserves registers and memory");
}

// Constants below were assembled and disassembled with the platform A64 assembler.
// Each oracle describes the architectural operation, independently of decoder masks.
void Scalar() {
    struct LoadCase { uint32_t opcode; size_t size, offset; bool sign, word; };
    constexpr LoadCase loads[] {
        {0x39400c20,1,3,false,true}, {0x79400c20,2,6,false,true},
        {0xb9400c20,4,12,false,true}, {0xf9400c20,8,24,false,false},
        {0x39c00c20,1,3,true,true}, {0x39800c20,1,3,true,false},
        {0x79c00c20,2,6,true,true}, {0x79800c20,2,6,true,false},
        {0xb9800c20,4,12,true,false}};
    for (const auto c : loads) {
        Memory m; auto s = Registers(); s.x[1] = Guest + 64;
        Put(m.bytes.data()+64+c.offset, UINT64_C(0x8123456789abcdef), c.size);
        auto expected = s;
        uint64_t value = Word(m.bytes.data()+64+c.offset, c.size);
        if (c.sign && (value & (uint64_t{1} << (c.size*8-1)))) value |= ~Mask(c.size);
        expected.x[0] = c.word ? static_cast<uint32_t>(value) : value;
        Success(c.opcode, Guest+64+c.offset+c.size-1, s, m, expected, m.bytes,
                Guest+64+c.offset, c.size, Access::Read, "scalar width/sign and W zero extension");
    }
    constexpr uint32_t stores[]{0x39000c20,0x79000c20,0xb9000c20,0xf9000c20};
    for (unsigned width = 0; width < 4; ++width) {
        const size_t size = size_t{1} << width;
        Memory m; auto s = Registers(); s.x[1] = Guest+64;
        auto expected = m.bytes; Put(expected.data()+64+3*size, s.x[0], size);
        Success(stores[width], Guest+64+3*size, s, m, s, expected,
                Guest+64+3*size, size, Access::Write, "scalar store width and canaries");
    }
    for (const bool load : {false,true}) {
        Memory m; auto s=Registers(); s.x[31]=Guest+64; auto expected=m.bytes;
        if (!load) Put(expected.data()+72,0,8);
        Success(load?0xf94007ff:0xf90007ff, Guest+72,s,m,s,expected,Guest+72,8,
                load?Access::Read:Access::Write,"ZR data register does not alias SP");
    }
    struct AddressCase { uint32_t opcode; int offset; bool store, writeback; uint64_t index; };
    constexpr AddressCase addressing[]{
        {0xf85f8020,-8,false,false,0}, // LDUR
        {0xf85f8c20,-8,false,true,0},  // LDR pre-index
        {0xf8008420,0,true,true,0},    // STR post-index #8
        {0xf8627820,16,false,false,2}, // LSL
        {0xf8625820,16,false,false,0xffffffff00000002ull}, // UXTW
        {0xf862d820,-8,false,false,0x12345678ffffffffull}, // SXTW
        {0xf862f820,-8,false,false,UINT64_MAX}}; // SXTX
    for (const auto c : addressing) {
        Memory m; auto s=Registers(); s.x[1]=Guest+64; s.x[2]=c.index;
        auto expected=s; auto bytes=m.bytes;
        if (c.store) Put(bytes.data()+64+c.offset,s.x[0],8);
        else expected.x[0]=Word(bytes.data()+64+c.offset,8);
        if (c.writeback) expected.x[1]=Guest+64+(c.store?8:c.offset);
        Success(c.opcode,Guest+64+c.offset,s,m,expected,bytes,Guest+64+c.offset,8,
                c.store?Access::Write:Access::Read,"scalar addressing and writeback");
    }
    constexpr uint32_t simd_load[]{0x3d400020,0x7d400020,0xbd400020,0xfd400020,0x3dc00020};
    constexpr uint32_t simd_store[]{0x3d000020,0x7d000020,0xbd000020,0xfd000020,0x3d800020};
    for (unsigned width=0;width<5;++width) for (const bool load:{false,true}) {
        const size_t size=size_t{1}<<width;
        Memory m; auto s=Registers(); s.x[1]=Guest+64; auto expected=s; auto bytes=m.bytes;
        if (load) { expected.v[0].fill(std::byte{}); std::copy_n(bytes.begin()+64,size,expected.v[0].begin()); }
        else std::copy_n(s.v[0].begin(),size,bytes.begin()+64);
        Success(load?simd_load[width]:simd_store[width],Guest+64,s,m,expected,bytes,Guest+64,size,
                load?Access::Read:Access::Write,"SIMD scalar width and cleared upper lanes");
    }
}

void Pairs() {
    struct Case { uint32_t opcode; size_t size, offset; bool simd, load, sign, post, sp; };
    constexpr Case cases[]{
        {0x29410440,4,8,false,true,false,false,false},
        {0xa9410440,8,16,false,true,false,false,false},
        {0x69410440,4,8,false,true,true,false,false},
        {0x2d410440,4,8,true,true,false,false,false},
        {0x6d410440,8,16,true,true,false,false,false},
        {0xacc10440,16,0,true,true,false,true,false},
        {0x29bf07e0,4,0,false,false,false,false,true},
        {0xa9bf07e0,8,0,false,false,false,false,true},
        {0x2d010440,4,8,true,false,false,false,false},
        {0x6d010440,8,16,true,false,false,false,false},
        {0xac810440,16,0,true,false,false,true,false},
        {0xa8410440,8,16,false,true,false,false,false},
        {0xa8010440,8,16,false,false,false,false,false}};
    for (const auto c:cases) {
        Memory m; auto s=Registers(); s.x[c.sp?31:2]=Guest+64+(c.sp?2*c.size:0);
        auto expected=s; auto bytes=m.bytes;
        if (c.sign) { Put(bytes.data()+64+c.offset,0xffffff81u,4); Put(bytes.data()+68+c.offset,0x7fffffff,4); m.bytes=bytes; }
        for (unsigned r=0;r<2;++r) {
            auto* p=bytes.data()+64+c.offset+r*c.size;
            if (c.simd) {
                if (c.load) { expected.v[r].fill(std::byte{}); std::copy_n(p,c.size,expected.v[r].begin()); }
                else std::copy_n(s.v[r].begin(),c.size,p);
            } else if (c.load) {
                const auto value=Word(p,c.size);
                expected.x[r]=c.sign?static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(value))):value;
            } else Put(p,s.x[r],c.size);
        }
        if (c.post) expected.x[2]+=2*c.size;
        if (c.sp) expected.x[31]=Guest+64;
        Success(c.opcode,Guest+64+c.offset+2*c.size-1,s,m,expected,bytes,Guest+64+c.offset,2*c.size,
                c.load?Access::Read:Access::Write,"pair general/SIMD/non-temporal/LDPSW/writeback");
    }
}

void NeonMultiple() {
    struct Case { uint32_t opcode; unsigned registers, element, vector_size, first; bool interleaved,load,post,register_post; };
    constexpr Case cases[]{
        {0x4c407020,1,1,16,0,false,true,false,false},
        {0x4cdfa020,2,1,16,0,false,true,true,false},
        {0x4cdf6020,3,1,16,0,false,true,true,false},
        {0x4cdf2020,4,1,16,0,false,true,true,false},
        {0x4c822020,4,1,16,0,false,false,true,true},
        {0x4c408420,2,2,16,0,true,true,false,false},
        {0x4c008420,2,2,16,0,true,false,false,false},
        {0x4c404820,3,4,16,0,true,true,false,false},
        {0x4c004820,3,4,16,0,true,false,false,false},
        {0x4c400020,4,1,16,0,true,true,false,false},
        {0x4c000020,4,1,16,0,true,false,false,false},
        {0x0c40a03f,2,1,8,31,false,true,false,false}};
    for(const auto c:cases) {
        Memory m; auto s=Registers(); s.x[1]=Guest+64; s.x[2]=96;
        auto expected=s; auto bytes=m.bytes;
        for(unsigned r=0;r<c.registers;++r) {
            const unsigned target=(c.first+r)%32;
            if(c.load) expected.v[target].fill(std::byte{});
            for(unsigned lane=0;lane<c.vector_size/c.element;++lane) {
                const auto offset=64+(c.interleaved?(lane*c.registers+r)*c.element:r*c.vector_size+lane*c.element);
                if(c.load) std::copy_n(bytes.begin()+offset,c.element,expected.v[target].begin()+lane*c.element);
                else std::copy_n(s.v[target].begin()+lane*c.element,c.element,bytes.begin()+offset);
            }
        }
        const auto size=c.registers*c.vector_size;
        if(c.post) expected.x[1]+=c.register_post?96:size;
        Success(c.opcode,Guest+64+size-1,s,m,expected,bytes,Guest+64,size,
                c.load?Access::Read:Access::Write,"NEON structures, deinterleave, wrapping registers and post-index");
    }
}

void OrderedAndAtomic() {
    constexpr uint32_t ldar[]{0x08dffc20,0x48dffc20,0x88dffc20,0xc8dffc20};
    constexpr uint32_t stlr[]{0x089ffc20,0x489ffc20,0x889ffc20,0xc89ffc20};
    constexpr uint32_t ldapr[]{0x38bfc020,0x78bfc020,0xb8bfc020,0xf8bfc020};
    constexpr uint32_t ldapur[]{0x195ff020,0x595fe020,0x995fc020,0xd95f8020};
    constexpr uint32_t stlur[]{0x191ff020,0x591fe020,0x991fc020,0xd91f8020};
    for(unsigned width=0;width<4;++width) for(unsigned mode=0;mode<5;++mode) {
        const size_t size=size_t{1}<<width; const bool load=mode!=1&&mode!=4;
        Memory m; auto s=Registers(); s.x[1]=Guest+64+(mode>=3?size:0);
        auto expected=s; auto bytes=m.bytes;
        if(load) expected.x[0]=Word(bytes.data()+64,size); else Put(bytes.data()+64,s.x[0],size);
        const uint32_t opcodes[]{ldar[width],stlr[width],ldapr[width],ldapur[width],stlur[width]};
        Success(opcodes[mode],Guest+64,s,m,expected,bytes,Guest+64,size,
                load?Access::Read:Access::Write,"ordered scalar and RCPC widths");
    }
    for(const auto c:{std::pair{0x59dfe020u,size_t{2}},std::pair{0x199ff020u,size_t{1}}}) {
        Memory m; auto s=Registers(); s.x[1]=Guest+64+c.second; auto expected=s;
        Put(m.bytes.data()+64,0xff81,c.second);
        expected.x[0]=c.second==2?0xffffff81u:UINT64_C(0xffffffffffffff81);
        Success(c.first,Guest+64,s,m,expected,m.bytes,Guest+64,c.second,Access::Read,"RCPC sign extension");
    }
    constexpr uint32_t cas[]{0x08a07c41,0x48a07c41,0x88a07c41,0xc8a07c41};
    constexpr uint32_t add[]{0x38200041,0x78200041,0xb8200041,0xf8200041};
    for(unsigned width=0;width<4;++width) {
        const size_t size=size_t{1}<<width;
        for(unsigned ordering=0;ordering<4;++ordering) for(const bool match:{false,true}) {
            Memory m; auto s=Registers(); s.x[2]=Guest+64; s.x[0]=match?0x91:0x92; s.x[1]=0x35;
            Put(m.bytes.data()+64,0x91,size); auto expected=s; auto bytes=m.bytes;
            expected.x[0]=0x91; if(match) Put(bytes.data()+64,0x35,size);
            const uint32_t order=((ordering&1)?1u<<22:0)|((ordering&2)?1u<<15:0);
            Success(cas[width]|order,Guest+64,s,m,expected,bytes,Guest+64,size,Access::ReadWrite,"CAS widths/order/success/failure");
        }
        for(unsigned operation=0;operation<9;++operation) for(unsigned ordering=0;ordering<4;++ordering) {
            Memory m; auto s=Registers(); s.x[2]=Guest+64; s.x[0]=0x37;
            const uint64_t before=Mask(size)-5, source=s.x[0];
            Put(m.bytes.data()+64,before,size); auto expected=s; auto bytes=m.bytes; uint64_t after=0;
            // Includes a negative old signed value and a positive source, so signed
            // min/max deliberately differ from their unsigned counterparts.
            switch(operation) {
                case 0: after=(before+source)&Mask(size); break;
                case 1: after=before&~source; break;
                case 2: after=before^source; break;
                case 3: after=before|source; break;
                case 4: after=source; break;
                case 5: after=before; break;
                case 6: after=before; break;
                case 7: after=source; break;
                case 8: after=source; break;
            }
            expected.x[1]=before; Put(bytes.data()+64,after,size);
            const uint32_t order=((ordering&1)?1u<<23:0)|((ordering&2)?1u<<22:0);
            Success(add[width]|(operation<<12)|order,Guest+64,s,m,expected,bytes,Guest+64,size,
                    Access::ReadWrite,"LSE operations/all widths/orderings and old-value return");
        }
    }
#if defined(__aarch64__) || defined(__arm64__)
    for(const size_t element:{size_t{4},size_t{8}}) for(unsigned ordering=0;ordering<4;++ordering)
        for(const bool match:{false,true}) {
            Memory m; auto s=Registers(); s.x[4]=Guest+64;
            s.x[0]=match?0x1234:0x1235; s.x[1]=0x5678; s.x[2]=0xabcd; s.x[3]=0xef01;
            Put(m.bytes.data()+64,0x1234,element); Put(m.bytes.data()+64+element,0x5678,element);
            auto expected=s; expected.x[0]=0x1234; auto bytes=m.bytes;
            if(match) { Put(bytes.data()+64,s.x[2],element); Put(bytes.data()+64+element,s.x[3],element); }
            const uint32_t order=((ordering&1)?1u<<22:0)|((ordering&2)?1u<<15:0);
            Success((element==4?0x08207c82u:0x48207c82u)|order,Guest+64,s,m,expected,bytes,
                    Guest+64,2*element,Access::ReadWrite,"CASP pair widths/order/success/failure");
        }
#endif
}

void Negative() {
    Memory m; auto s=Registers(); s.x[1]=Guest+64; s.x[2]=Guest+64; s.x[4]=Guest+64;
    for(const uint32_t opcode:{0xffffffffu,0xd503201fu,0x0d400c20u,0x4d40c020u,
            0xc85ffc20u,0xc85f7c20u,0xc802fc20u,0xc8027c20u,
            0xc87f8440u,0xc87f0440u,0xc8238440u,0xc8230440u})
        Rejected(opcode,Guest+64,s,m,"unknown, unsupported lane and LL/SC instructions rejected");
    Rejected(0xf9400020,Guest+63,s,m,"fault precedes operand");
    Rejected(0xf9400020,Guest+72,s,m,"fault follows operand");
    auto denied=m; denied.read=false;
    Rejected(0xf9400020,Guest+64,s,denied,"read permission denied");
    denied=m; denied.write=false;
    Rejected(0xf9000020,Guest+64,s,denied,"write permission denied");
    Rejected(0xc8a07c41,Guest+64,s,denied,"atomic requires write permission");
    denied=m; denied.read=false;
    Rejected(0xc8a07c41,Guest+64,s,denied,"atomic requires read permission");
    denied=m; denied.limit=95;
    Rejected(0xac810440,Guest+64,s,denied,"pair full-span guard preflight before either store");
    denied=m; denied.limit=127;
    Rejected(0x4c822020,Guest+64,s,denied,"64-byte NEON full-span preflight before any store");
    auto unaligned=s; unaligned.x[2]=Guest+65;
    Rejected(0xc8a07c41,Guest+65,unaligned,m,"unaligned scalar atomic rejected");
    unaligned=s; unaligned.x[4]=Guest+72;
    Rejected(0x48207c82,Guest+72,unaligned,m,"unaligned pair atomic rejected");
    Rejected(0x48217c82,Guest+64,s,m,"odd CASP pair register rejected");
    auto overlap=s; overlap.x[0]=Guest+64;
    Rejected(0xf85f8c00,Guest+56,overlap,m,"writeback base and destination overlap rejected");
    auto wrap=s; wrap.x[1]=UINT64_MAX-3;
    Rejected(0xf9400020,UINT64_MAX-3,wrap,m,"full operand overflow rejected");
    const auto old=s;
    Require(!Execute(0xf9400020,Guest+64,s,nullptr,nullptr)&&Same(old,s),"null resolver rejected");
}

void ZeroCacheBlock() {
#if defined(__aarch64__) || defined(__arm64__)
    uint64_t dczid = 0;
    asm volatile("mrs %0, dczid_el0" : "=r"(dczid));
    const size_t block = size_t{4} << (dczid & 15);
    struct ZeroMemory {
        std::vector<std::byte> bytes;
        size_t limit;
        bool writable = true;
        unsigned calls = 0;
        uint64_t address = 0;
        size_t width = 0;
        Access access = Access::Read;
        explicit ZeroMemory(size_t size): bytes(size, std::byte{0xa5}), limit(size) {}
        static bool Resolve(void* opaque, uint64_t address, size_t size, Access access,
                            void** host) noexcept {
            auto& m = *static_cast<ZeroMemory*>(opaque);
            ++m.calls; m.address = address; m.width = size; m.access = access;
            // This mapping grants write-only access: zeroing must not request reads.
            if (!m.writable || access != Access::Write || address < Guest ||
                address - Guest >= m.limit || size > m.limit - (address - Guest)) return false;
            *host = m.bytes.data() + address - Guest;
            return true;
        }
    };
    if (dczid & 16) {
        ZeroMemory memory(3*block); auto state = Registers(); state.x[0] = Guest+block;
        const auto before = state; const auto bytes = memory.bytes;
        Require(!Execute(0xd50b7420, Guest+block, state, ZeroMemory::Resolve, &memory) &&
                    Same(state,before) && bytes==memory.bytes && memory.calls==0,
                "DC ZVA honors the native DZP prohibition");
        return;
    }
    Require(block<=2048 && Guest%block==0, "native DCZID block size and guest arena alignment");
    for (const size_t offset : {size_t{0},size_t{1},block-1})
        for (const size_t fault_offset : {size_t{0},block-1}) {
            ZeroMemory memory(3*block); auto state=Registers(); state.x[0]=Guest+block+offset;
            const auto before=state; auto expected=memory.bytes;
            std::fill(expected.begin()+block,expected.begin()+2*block,std::byte{});
            Require(Execute(0xd50b7420,Guest+block+fault_offset,state,ZeroMemory::Resolve,&memory),
                    "DC ZVA admits any address inside the aligned block");
            Require(Same(state,before)&&memory.bytes==expected&&memory.calls==1&&
                        memory.address==Guest+block&&memory.width==block&&memory.access==Access::Write,
                    "DC ZVA zeroes only the complete aligned block and preserves canaries/registers");
        }
    for (unsigned denial=0;denial<4;++denial) {
        ZeroMemory memory(3*block); auto state=Registers(); state.x[0]=Guest+block+1;
        uint64_t fault=Guest+block;
        if (denial==0) memory.writable=false;
        if (denial==1) memory.limit=2*block-1;
        if (denial==2) fault=Guest+block-1;
        if (denial==3) fault=Guest+2*block;
        const auto before=state; const auto bytes=memory.bytes;
        Require(!Execute(0xd50b7420,fault,state,ZeroMemory::Resolve,&memory)&&
                    Same(state,before)&&memory.bytes==bytes,
                "DC ZVA rejects rights, partial blocks and unrelated faults without partial zeroing");
    }
    std::printf("DC ZVA native block: %zu bytes\n",block);
#endif
}

struct Shared {
    alignas(64) uint64_t value=0;
    static bool Resolve(void* opaque,uint64_t address,size_t size,Access access,void** host) noexcept {
        if(address!=Guest||size!=8||access!=Access::ReadWrite) return false;
        *host=&static_cast<Shared*>(opaque)->value; return true;
    }
};
void ConcurrentAtomics() {
    constexpr size_t workers=6, iterations=3000, total=workers*iterations;
    for(const bool cas:{false,true}) {
        Shared shared; std::atomic<bool> failed{false}; std::atomic<unsigned> ready{0};
        std::atomic<bool> start{false}; std::array<std::vector<uint64_t>,workers> observed;
        std::vector<std::thread> threads;
        for(size_t worker=0;worker<workers;++worker) threads.emplace_back([&,worker] {
            auto& values=observed[worker]; values.reserve(iterations);
            ready.fetch_add(1); while(!start.load(std::memory_order_acquire)) std::this_thread::yield();
            for(size_t count=0;count<iterations;++count) {
                // Alternate emulated instructions with native atomics on the SAME alias.
                if(worker&1) { values.push_back(__atomic_fetch_add(&shared.value,uint64_t{1},__ATOMIC_SEQ_CST)); continue; }
                State state; state.x[2]=Guest;
                if(!cas) {
                    state.x[0]=1;
                    if(!Execute(0xf8e00041,Guest,state,Shared::Resolve,&shared)) { failed=true; return; }
                    values.push_back(state.x[1]);
                } else {
                    uint64_t expected=__atomic_load_n(&shared.value,__ATOMIC_SEQ_CST);
                    for(;;) {
                        state.x[0]=expected; state.x[1]=expected+1;
                        if(!Execute(0xc8e0fc41,Guest,state,Shared::Resolve,&shared)) { failed=true; return; }
                        if(state.x[0]==expected) { values.push_back(expected); break; }
                        expected=state.x[0];
                    }
                }
            }
        });
        while(ready.load()!=workers) std::this_thread::yield(); start.store(true,std::memory_order_release);
        for(auto& thread:threads) thread.join();
        std::vector<uint64_t> all; for(auto& values:observed) all.insert(all.end(),values.begin(),values.end());
        std::sort(all.begin(),all.end());
        bool sequence=all.size()==total;
        for(size_t i=0;sequence&&i<all.size();++i) sequence=all[i]==i;
        Require(!failed&&shared.value==total&&sequence,cas?"concurrent CAS/native atomic linearizability":"concurrent LSE/native atomic linearizability");
    }
}

struct ScalarShared {
    alignas(64) uint64_t values[2]{};
    static bool Resolve(void* opaque,uint64_t address,size_t size,Access access,void** host) noexcept {
        if(address!=Guest||(size!=8&&size!=16)||access==Access::ReadWrite) return false;
        *host=static_cast<ScalarShared*>(opaque)->values; return true;
    }
};
void ConcurrentScalarTransfers() {
    // Native atomic accesses and decoded ordinary LDR/STR share the same alias.
    // Every byte differs between the two patterns, so a split transfer can yield
    // a third value. Pair elements are checked individually, not as a 128-bit unit.
    constexpr uint64_t patterns[]{0x0123456789abcdefull,0xfedcba9876543210ull};
    constexpr size_t iterations=30000;
    struct Case { uint32_t load,store; bool pair,simd; const char* label; };
    constexpr Case cases[]{
        {0xf9400020,0xf9000020,false,false,"aligned scalar/native atomic transfers do not tear"},
        {0xfd400020,0xfd000020,false,true,"aligned SIMD 8-byte/native atomic transfers do not tear"},
        {0xa9400440,0xa9000440,true,false,"aligned pair elements/native atomic transfers do not tear"}};
    for(const auto c:cases) for(const bool load:{false,true}) {
        ScalarShared shared; shared.values[0]=shared.values[1]=patterns[0];
        std::atomic<bool> ready{false},start{false},done{false},failed{false};
        std::atomic<size_t> native_iterations{0};
        const auto valid=[&](uint64_t value) {return value==patterns[0]||value==patterns[1];};
        std::thread native([&] {
            ready.store(true,std::memory_order_release);
            while(!start.load(std::memory_order_acquire)) std::this_thread::yield();
            size_t count=0;
            do {
                for(size_t word=0;word<(c.pair?2u:1u);++word) {
                    if(load) __atomic_store_n(&shared.values[word],patterns[count&1],__ATOMIC_RELAXED);
                    else if(!valid(__atomic_load_n(&shared.values[word],__ATOMIC_RELAXED))) failed=true;
                }
                ++count;
            } while(!done.load(std::memory_order_acquire));
            native_iterations.store(count,std::memory_order_release);
        });
        while(!ready.load(std::memory_order_acquire)) std::this_thread::yield();
        State state; state.x[c.pair?2:1]=Guest;
        start.store(true,std::memory_order_release);
        for(size_t iteration=0;iteration<iterations;++iteration) {
            if(!load) {
                if(c.simd) Put(state.v[0].data(),patterns[iteration&1],8);
                else {state.x[0]=patterns[iteration&1]; if(c.pair) state.x[1]=patterns[iteration&1];}
            }
            if(!Execute(load?c.load:c.store,Guest,state,ScalarShared::Resolve,&shared)) {failed=true;break;}
            if(load&&(!valid(c.simd?Word(state.v[0].data(),8):state.x[0])||
                      (c.pair&&!valid(state.x[1])))) {failed=true;break;}
        }
        done.store(true,std::memory_order_release); native.join();
        Require(!failed&&native_iterations.load()>0,c.label);
    }
}
void UnalignedScalarTransfers() {
    struct Case { uint32_t load,store; bool pair,simd; };
    constexpr Case cases[]{
        {0xf9400020,0xf9000020,false,false},
        {0xfd400020,0xfd000020,false,true},
        {0xa9400440,0xa9000440,true,false}};
    for(const auto c:cases) for(const bool load:{false,true}) {
        Memory memory; auto state=Registers(); state.x[c.pair?2:1]=Guest+65;
        auto expected=state; auto bytes=memory.bytes;
        for(size_t word=0;word<(c.pair?2u:1u);++word) {
            if(load) {
                if(c.simd) {
                    expected.v[0].fill(std::byte{});
                    std::copy_n(bytes.begin()+65,8,expected.v[0].begin());
                } else expected.x[word]=Word(bytes.data()+65+word*8,8);
            } else {
                Put(bytes.data()+65+word*8,c.simd?Word(state.v[0].data(),8):state.x[word],8);
            }
        }
        Success(load?c.load:c.store,Guest+65,state,memory,expected,bytes,Guest+65,c.pair?16:8,
                load?Access::Read:Access::Write,"unaligned scalar/SIMD/pair transfers preserve bytes and canaries");
    }
}
} // namespace
int main() {
    Scalar(); Pairs(); NeonMultiple(); OrderedAndAtomic(); Negative(); ZeroCacheBlock(); ConcurrentAtomics();
    ConcurrentScalarTransfers(); UnalignedScalarTransfers();
    std::printf("Arm64MemoryAccessTests: %u checks passed\n",checks);
}
