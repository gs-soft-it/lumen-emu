// SPDX-License-Identifier: GPL-2.0-only
// The production PS5 service and typed ABI run inside the unchanged direct FEX
// engine. This is a control-flow correctness fixture, not a throughput test.
#include "DirectFexRuntime.h"
#include "common/assert.h"
#include "common/virtualMemory.h"
#include "kernel/memory.h"
#include "libs/errno.h"
#include "libs/libs.h"
#include "loader/hostCall.h"

#include <array>
#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

namespace Libs {
#include "libs/fiber.inc"
}

namespace {
namespace Direct = Lumen::Ps5::DirectFex;
namespace Fiber = Libs::Fiber;
uint64_t Page = 4096;
constexpr uint64_t CodeSize = 16384;
struct Region { uint64_t base, size; bool writable; };
std::vector<Region> regions;
uint64_t code;
unsigned host_calls;

bool Accessible(uint64_t address, uint64_t size, bool write) {
	for (const auto& r : regions)
		if ((!write || r.writable) && address >= r.base && address - r.base <= r.size &&
		    size <= r.size - (address - r.base)) return true;
	return false;
}

void Check(bool value, const char* label) {
	if (!value) {
		std::fprintf(stderr, "DIRECT_FIBER_TEST FAIL %s\n", label);
		std::abort();
	}
	std::printf("DIRECT_FIBER_TEST PASS %s\n", label);
}

void* Allocate(uint64_t size, bool writable = true) {
	auto* p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	Check(p != MAP_FAILED, "fixture mapping");
	regions.push_back({reinterpret_cast<uint64_t>(p), size, writable});
	return p;
}

bool Read(void*, uint64_t address, void* output, std::size_t size) noexcept {
	if (!Accessible(address, size, false)) return false;
	std::memcpy(output, reinterpret_cast<const void*>(address), size);
	return true;
}

uint32_t Query(void*, uint64_t address, uint64_t* base, uint64_t* size, uint32_t* writable) noexcept {
	if (address < code || address - code >= CodeSize) return 0;
	*base = code; *size = CodeSize; *writable = 0;
	return 1;
}

uint32_t Host(void*, uint64_t target, uint64_t* registers,
	          LumenAndroidFexCoreSimdViewV2* simd, uint64_t*) noexcept {
	++host_calls;
	return Loader::DispatchHostCall(target, registers, simd, Read, nullptr)
	    ? LUMEN_ANDROID_FEXCORE_HOST_CALL_RETURN_GUEST_V1
	    : LUMEN_ANDROID_FEXCORE_HOST_CALL_FAILED_V1;
}

struct Code {
	std::vector<uint8_t> bytes;
	void Emit(std::initializer_list<uint8_t> values) { bytes.insert(bytes.end(), values); }
	void Imm(unsigned reg, uint64_t value) {
		Emit({static_cast<uint8_t>(0x48 | (reg >> 3)), static_cast<uint8_t>(0xb8 | (reg & 7))});
		const auto* p = reinterpret_cast<const uint8_t*>(&value);
		bytes.insert(bytes.end(), p, p + 8);
	}
	void Store(void* address, unsigned reg) {
		Imm(0, reinterpret_cast<uint64_t>(address));
		Emit({static_cast<uint8_t>(0x48 | ((reg >> 3) << 2)), 0x89, static_cast<uint8_t>((reg & 7) << 3)});
	}
	void Result(void* address) {
		Emit({0x48,0x89,0xc1}); // mov rcx,rax; Store uses RAX for the address
		Store(address,1);
	}
	void Call(uint64_t target, uint64_t arg0, uint64_t arg1 = 0, uint64_t arg2 = 0) {
		Imm(7, arg0); Imm(6, arg1); Imm(2, arg2);
		Imm(11, Direct::ResolveHostTarget(target));
		Emit({0x41, 0xff, 0xd3});
	}
	void Prolog() { Emit({0x48,0x83,0xec,8}); }
	void Ret() { Emit({0x48,0x83,0xc4,8,0xc3}); }
	uint64_t Place(uint64_t offset) {
		Check(offset + bytes.size() < CodeSize, "guest code range");
		std::memcpy(reinterpret_cast<void*>(code + offset), bytes.data(), bytes.size());
		return code + offset;
	}
};

struct Data {
	alignas(16) Fiber::FiberObject a {}, b {}, invalid {}, stackless {};
	uint64_t init_a {}, run_a {}, init_b {}, run_b {};
	uint64_t initial_xmm {}, initial_mmx {};
	uint64_t resume_a {}, switch_a {}, switch_b {};
	uint64_t rbx {}, rbp {}, r12 {}, r13 {}, r14 {}, r15 {}, xmm {}, rsp {};
	uint64_t self_a {}, self_thread {}, returned[4] {}, result[4] {}, parent_register[4] {};
	uint64_t nested_run {}, self_switch {}, outside_return {}, terminated_run {};
	uint64_t failure_result {}, failure_output {0xfeed};
	uint64_t stackless_count {}, stackless_return[2] {}, stackless_result[2] {};
	uint32_t mxcsr_initial {0x5f80}, mxcsr_restored {};
	uint16_t fcw_initial {0x77f}, fcw_restored {};
};

template <auto Function> uint64_t Target(const char* name) {
	Check(Loader::RegisterHostFunction<Function>(name), name);
	return reinterpret_cast<uint64_t>(Function);
}

bool Run(uint64_t entry, uint64_t stack) {
	LumenAndroidFexCoreGuestStateV1 state {sizeof(state), 1, entry};
	state.gprs[4] = stack + Page - 24;
	const auto sentinel = Direct::RootReturn();
	std::memcpy(reinterpret_cast<void*>(state.gprs[4]), &sentinel, 8);
	const auto result = Direct::RunGuestV2(&state, Host, Query, nullptr);
	if (result != 0 || state.rip != sentinel)
		std::fprintf(stderr, "DIRECT_FIBER_EXEC result=%u rip=%" PRIx64 " expected=%" PRIx64 "\n",
		             result, state.rip, sentinel);
	return result == 0 && state.rip == sentinel;
}
} // namespace

namespace Libs::LibKernel::Memory {
// The fixture owns every admitted range. Production resolves these rights from
// the original PS5 VM map; the fiber implementation above is shared verbatim.
bool QueryGuestAccess(uint64_t address, uint64_t size, Common::VirtualMemory::Mode mode) noexcept {
	return Accessible(address, size, (static_cast<uint32_t>(mode) & 2) != 0);
}
}

int main() {
	const auto host_page = sysconf(_SC_PAGESIZE);
	Check((host_page == 4096 || host_page == 16384) && Direct::Probe() == 0, "ARM64 direct FEX bootstrap");
	Page = static_cast<uint64_t>(host_page);
	code = reinterpret_cast<uint64_t>(Allocate(CodeSize, false));
	auto& d = *static_cast<Data*>(Allocate(Page));
	new (&d) Data;
	auto* stack_a = Allocate(Page);
	auto* stack_b = Allocate(Page);
	const auto stack_parent = reinterpret_cast<uint64_t>(Allocate(Page));
	const auto stack_worker = reinterpret_cast<uint64_t>(Allocate(Page));
	const auto run = Target<Fiber::FiberRun>("FiberRun");
	const auto change = Target<Fiber::FiberSwitch>("FiberSwitch");
	const auto yield = Target<Fiber::FiberReturnToThread>("FiberReturnToThread");
	const auto self = Target<Fiber::FiberGetSelf>("FiberGetSelf");
	const auto ptr = [](const void* p) { return reinterpret_cast<uint64_t>(p); };
	Code a;
	a.Prolog(); a.Store(&d.init_a,7); a.Store(&d.run_a,6);
	a.Imm(0,ptr(&d.initial_xmm)); a.Emit({0x66,0x0f,0xd6,0x00});
	a.Emit({0x48,0x0f,0x7e,0xc1}); a.Store(&d.initial_mmx,1); // movq rcx,mm0
	a.Call(self, ptr(&d.self_a));
	a.Call(run,ptr(&d.a)); a.Result(&d.nested_run);
	a.Call(change,ptr(&d.a)); a.Result(&d.self_switch);
	for (const unsigned reg : {3,5,12,13,14,15}) a.Imm(reg, 0x12340000 + reg);
	a.Imm(0,0x1122334455667788ULL); a.Emit({0x66,0x48,0x0f,0x6e,0xc0}); // movq xmm0,rax
	a.Imm(0,ptr(&d.mxcsr_initial)); a.Emit({0x0f,0xae,0x10}); // ldmxcsr [rax]
	a.Imm(0,ptr(&d.fcw_initial)); a.Emit({0xd9,0x28}); // fldcw [rax]
	a.Call(yield,100,ptr(&d.resume_a));
	a.Store(&d.rbx,3); a.Store(&d.rbp,5); a.Store(&d.r12,12);
	a.Store(&d.r13,13); a.Store(&d.r14,14); a.Store(&d.r15,15); a.Store(&d.rsp,4);
	a.Imm(0,ptr(&d.xmm)); a.Emit({0x66,0x0f,0xd6,0x00}); // movq [rax],xmm0
	a.Imm(0,ptr(&d.mxcsr_restored)); a.Emit({0x0f,0xae,0x18}); // stmxcsr [rax]
	a.Imm(0,ptr(&d.fcw_restored)); a.Emit({0xd9,0x38}); // fnstcw [rax]
	a.Call(change,ptr(&d.b),222,ptr(&d.switch_a));
	a.Call(yield,300); a.Ret();
	const auto entry_a = a.Place(0);
	Code b;
	b.Prolog(); b.Store(&d.init_b,7); b.Store(&d.run_b,6);
	b.Call(change,ptr(&d.a),444,ptr(&d.switch_b)); b.Ret();
	const auto entry_b = b.Place(2048);
	Check(Fiber::FiberInitialize(&d.a,"A",reinterpret_cast<Fiber::FiberEntry>(entry_a),
	                            11,stack_a,Page,nullptr,0x3500000) == OK &&
	      Fiber::FiberInitialize(&d.b,"B",reinterpret_cast<Fiber::FiberEntry>(entry_b),
	                            22,stack_b,Page,nullptr,0x3500000) == OK,
	      "original PS5 fiber initialize");
	std::array<uint64_t,4> drivers {};
	for (unsigned i = 0; i < drivers.size(); ++i) {
		Code driver;
		driver.Prolog(); driver.Imm(12,0xabcdef00+i);
		driver.Imm(0,0x1122334455667788ULL);
		driver.Emit({0x48,0x0f,0x6e,0xc0,0x66,0x48,0x0f,0x6e,0xc0}); // seed MM0 and XMM0
		driver.Call(run,ptr(i==2 ? &d.b : &d.a),std::array<uint64_t,4>{111,333,555,666}[i],ptr(&d.returned[i]));
		// RAX is the returned code; use RCX so Store can materialize its address.
		driver.Result(&d.result[i]);
		driver.Store(&d.parent_register[i],12);
		driver.Call(self,ptr(&d.self_thread)); driver.Ret();
		drivers[i]=driver.Place(3072+i*1024);
	}
	Check(Fiber::FiberInitialize(&d.invalid,"bad-context",reinterpret_cast<Fiber::FiberEntry>(entry_a),
	                            0,stack_a,Page,nullptr,0)==OK,"invalid target fixture initialize");
	d.invalid.addr_context=reinterpret_cast<void*>(UINT64_MAX-15);
	d.invalid.size_context=512;
	Code negative;
	negative.Prolog(); negative.Call(run,ptr(&d.invalid),0,ptr(&d.failure_output)); negative.Result(&d.failure_result);
	negative.Call(yield,123); negative.Result(&d.outside_return);
	negative.Call(run,ptr(&d.a)); negative.Result(&d.terminated_run); negative.Ret();
	const auto negative_entry=negative.Place(8192);
	Code stackless;
	stackless.Prolog(); stackless.Imm(0,ptr(&d.stackless_count)); stackless.Emit({0x48,0xff,0x00});
	stackless.Call(yield,77); stackless.Emit({0x0f,0x0b}); // stale-stack resumption must never reach UD2
	const auto stackless_entry=stackless.Place(9216);
	Check(Fiber::FiberInitialize(&d.stackless,"stackless",reinterpret_cast<Fiber::FiberEntry>(stackless_entry),
	                            0,nullptr,0,nullptr,0)==OK,"stackless fiber initialize");
	Code restart;
	restart.Prolog();
	for (unsigned i=0;i<2;++i) {
		restart.Call(run,ptr(&d.stackless),i,ptr(&d.stackless_return[i])); restart.Result(&d.stackless_result[i]);
	}
	restart.Ret();
	const auto restart_entry=restart.Place(10240);
	Check(mprotect(reinterpret_cast<void*>(code),CodeSize,PROT_READ)==0,"guest code sealed");
	Check(Run(drivers[0],stack_parent),"guest Run and ReturnToThread");
	Check(d.returned[0]==100 && d.result[0]==0 && d.a.state==Fiber::FIBER_STATE_IDLE &&
	      d.init_a==11 && d.run_a==111 && d.self_a==ptr(&d.a),"yield preserves original state and arguments");
	Check(d.initial_xmm==0 && d.initial_mmx==0,"first entry resets original SIMD and MM state");
	Check(static_cast<int32_t>(d.nested_run)==Fiber::FIBER_ERROR_PERMISSION &&
	      static_cast<int32_t>(d.self_switch)==Fiber::FIBER_ERROR_STATE,
	      "nested Run and Switch to running self rejected");
	bool migrated {};
	std::thread worker([&] { migrated=Run(drivers[1],stack_worker); }); worker.join();
	Check(migrated,"resume and two-fiber switch on another host thread");
	Check(d.resume_a==333 && d.switch_a==444 && d.init_b==22 && d.run_b==222 &&
	      d.returned[1]==300 && d.result[1]==0 && d.a.state==Fiber::FIBER_STATE_IDLE &&
	      d.b.state==Fiber::FIBER_STATE_IDLE,"guest Switch A to B to A");
	Check(d.rbx==0x12340003 && d.rbp==0x12340005 && d.r12==0x1234000c && d.r13==0x1234000d &&
	      d.r14==0x1234000e && d.r15==0x1234000f && d.xmm==0x1122334455667788ULL &&
	      d.mxcsr_restored==d.mxcsr_initial && d.fcw_restored==d.fcw_initial &&
	      d.rsp>=ptr(stack_a) && d.rsp<ptr(stack_a)+Page,"guest registers SIMD FP and stack restored");
	Check(Run(drivers[2],stack_parent) && d.switch_b==555 && d.returned[2]==0 &&
	      static_cast<int32_t>(d.result[2])==Fiber::FIBER_ERROR_STATE &&
	      d.b.state==Fiber::FIBER_STATE_TERMINATED,"entry RET terminates B and resumes Run");
	Check(Run(drivers[3],stack_parent) && d.returned[3]==0 &&
	      static_cast<int32_t>(d.result[3])==Fiber::FIBER_ERROR_STATE &&
	      d.a.state==Fiber::FIBER_STATE_TERMINATED,"entry RET terminates A and resumes Run");
	Check(Run(negative_entry,stack_parent) &&
	      static_cast<int32_t>(d.failure_result)==Fiber::FIBER_ERROR_INVALID && d.failure_output==0xfeed &&
	      d.invalid.state==Fiber::FIBER_STATE_IDLE &&
	      static_cast<int32_t>(d.outside_return)==Fiber::FIBER_ERROR_PERMISSION &&
	      static_cast<int32_t>(d.terminated_run)==Fiber::FIBER_ERROR_STATE,
	      "invalid target rollback and out-of-context controls");
	Check(Run(restart_entry,stack_parent) && d.stackless_count==2 && !d.stackless.context_valid &&
	      d.stackless_return[0]==77 && d.stackless_return[1]==77 &&
	      d.stackless_result[0]==0 && d.stackless_result[1]==0 && Fiber::FiberFinalize(&d.stackless)==OK,
	      "stackless fibers restart without retaining borrowed stack");
	Check(d.self_thread==0 && Fiber::FiberGetSelf(reinterpret_cast<Fiber::FiberObject**>(&d.self_thread))==OK &&
	      d.self_thread==0,"thread fiber context cleared");
	for (unsigned i=0;i<4;++i)
		Check(d.parent_register[i]==0xabcdef00+i,"parent callee-saved register restored");
	Check(Fiber::FiberFinalize(&d.a)==Fiber::FIBER_ERROR_STATE &&
	      Fiber::FiberInitialize(&d.a,"reuse",reinterpret_cast<Fiber::FiberEntry>(entry_a),0,
	                            stack_a,Page,nullptr,0)==OK &&
	      Fiber::FiberFinalize(&d.a)==OK,"finalize and guest object address reuse");
	std::printf("PS5_DIRECT_FIBER_V1 guest_run=1 guest_resume=1 guest_switch=1 thread_migration=1 "
	            "entry_return=1 gpr_simd_fp=1 negative_controls=1 stackless=1 host_calls=%u benchmark=0\n",host_calls);
	Check(Direct::BeginCodeMutation(),"fixture code mutation begins");
	for (const auto& r:regions) Check(munmap(reinterpret_cast<void*>(r.base),r.size)==0,"fixture mapping release");
	Check(Direct::EndCodeMutation(code,CodeSize,true),"fixture code invalidation");
	return 0;
}
