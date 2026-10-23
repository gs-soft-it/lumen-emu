#include "FexFpBoundaryLegacyProbe.h"

// Original synthetic numerical/FP-boundary fixtures, without game inputs or
// external test programs. Exact expected bits are representable powers of two.
// The dynamic-load mode preserves a counterexample against an installed APK.
#include <array>
#include <cfenv>
#include <cstdio>
#include <dlfcn.h>
#include <pthread.h>
#include <sys/auxv.h>
#include <time.h>

namespace Legacy = Lumen::FexCore::FpBoundaryLegacyProbe;
namespace {
struct Native { unsigned long fpcr, fpsr; };
Native Read() { Native v; asm volatile("mrs %0,fpcr" : "=r"(v.fpcr)); asm volatile("mrs %0,fpsr" : "=r"(v.fpsr)); return v; }
void Write(Native v) { asm volatile("msr fpcr,%0" :: "r"(v.fpcr)); asm volatile("msr fpsr,%0" :: "r"(v.fpsr)); }
bool Equal(Native a, Native b) { return a.fpcr == b.fpcr && a.fpsr == b.fpsr; }
// Original independently authored SSE/x87 fixture. RDI points to native-owned
// inputs and control words, RSI to result words; RBX is registered test HLE.
constexpr std::array<unsigned char, 66> Guest {
  0x0f,0xae,0x57,0x04, // ldmxcsr [rdi+4]
  0xd9,0x6f,0x08, // fldcw [rdi+8]
  0xf3,0x0f,0x10,0x07, // movss xmm0,[rdi]
  0xf3,0x0f,0x2d,0xc0, // cvtss2si eax,xmm0
  0x89,0x06, // mov [rsi],eax
  0xd9,0x07, // fld dword [rdi]
  0xdb,0x5e,0x04, // fistp dword [rsi+4]
  0x0f,0xae,0x5e,0x08, // stmxcsr [rsi+8]
  0xd9,0x7e,0x0c, // fnstcw [rsi+12]
  0xff,0xd3, // call rbx
  0xf3,0x0f,0x10,0x07,
  0xf3,0x0f,0x2d,0xc0,
  0x89,0x46,0x10,
  0xd9,0x07,
  0xdb,0x5e,0x14,
  0x0f,0xae,0x5e,0x18,
  0xd9,0x7e,0x1c,
  0xc3,
};
// Nested roots deliberately omit LDMXCSR: real V2 default state must execute
// with nearest SSE rounding irrespective of the callback's native setting.
constexpr std::array<unsigned char, 8> Nested {0xf3,0x0f,0x10,0x07,0xf3,0x0f,0x2d,0xc0};
constexpr std::array<unsigned char, 53> Denormal {
  0x0f,0xae,0x57,0x20, // ldmxcsr [rdi+32]
  0x0f,0x10,0x07,0x0f,0x10,0x4f,0x10, // movups xmm0,[rdi]; xmm1,[rdi+16]
  0x0f,0x59,0xc1,0x0f,0x11,0x06, // mulps xmm0,xmm1; movups [rsi],xmm0
  0x0f,0xae,0x5e,0x10, // stmxcsr [rsi+16]
  0x0f,0xae,0x57,0x24, // ldmxcsr [rdi+36] changes controls inside one root
  0x0f,0x10,0x07,0x0f,0x10,0x4f,0x10,
  0x0f,0x59,0xc1,0x0f,0x11,0x46,0x20,
  0x0f,0xae,0x5e,0x30,
  0xd9,0x07,0xd8,0x4f,0x10,0xd9,0x5e,0x40, // x87 independent multiply
  0xc3,
};
constexpr std::array<unsigned char, 30> DenormalCompareConvert {
  0x0f,0xae,0x57,0x20, // ldmxcsr [rdi+32]
  0x0f,0x10,0x07, // movups xmm0,[rdi]
  0x66,0x0f,0xef,0xc9, // pxor xmm1,xmm1
  0x0f,0xc2,0xc1,0x06, // cmpps xmm0,xmm1,6 (not less/equal)
  0x0f,0x11,0x06, // movups [rsi],xmm0 (integer masks)
  0xf3,0x0f,0x10,0x07, // movss xmm0,[rdi]
  0xf3,0x0f,0x2d,0xc0, // cvtss2si eax,xmm0
  0x89,0x46,0x10, // mov [rsi+16],eax
  0xc3,
};
struct Context {
  Legacy::Api api;
  unsigned long code, data, page, sentinel, target;
  Native host;
  bool nested;
  unsigned calls, hostMismatch, nestedMismatch;
  pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
  pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
};
void Identity() {}
unsigned Query(void* p, unsigned long address, unsigned long* base, unsigned long* size, unsigned* writable) noexcept {
  auto& c = *static_cast<Context*>(p);
  if (address < c.code || address-c.code >= c.page) return 0;
  *base=c.code; *size=c.page; *writable=0; return 1;
}
unsigned Callback(void* p, unsigned long target, unsigned long* gprs, LumenAndroidFexCoreSimdViewV2*, unsigned long* next) noexcept {
  auto& c = *static_cast<Context*>(p);
  if(target != c.target || gprs[Legacy::Rsp] < c.data || gprs[Legacy::Rsp]-c.data > c.page*4-8) return 0;
  unsigned long rip; std::memcpy(&rip,reinterpret_cast<void*>(gprs[Legacy::Rsp]),8);
  if (rip < c.code || rip-c.code >= c.page) return 0;
  ++c.calls; if(!Equal(Read(),c.host)) ++c.hostMismatch;
  const Native changed {0x3000000UL | (3UL<<22), 0x1f};
  Write(changed);
  timespec deadline{}; clock_gettime(CLOCK_REALTIME,&deadline); deadline.tv_nsec+=1000000;
  if(deadline.tv_nsec>=1000000000) {++deadline.tv_sec;deadline.tv_nsec-=1000000000;}
  pthread_mutex_lock(&c.mutex);
  (void)pthread_cond_timedwait(&c.condition,&c.mutex,&deadline);
  pthread_mutex_unlock(&c.mutex);
  if(c.nested) {
    const unsigned long stack=c.data+c.page*4-8;
    std::memcpy(reinterpret_cast<void*>(stack),&c.sentinel,8);
    LumenAndroidFexCoreGuestStateV1 seed{sizeof(seed),1,c.code+128,{},0,0,0,0,0};
    seed.gprs[Legacy::Rsp]=stack;seed.gprs[Legacy::Rdi]=c.data;
    const auto result=c.api.run(&seed,&Callback,&Query,&c);
    const int expected=(*reinterpret_cast<unsigned*>(c.data)&0x80000000U)?-2:2;
    if(result != 0 || static_cast<int>(seed.gprs[0]) != expected || !Equal(Read(),changed)) {
      ++c.nestedMismatch;
      std::printf("NESTED_FAIL code=%u value=%lx expected=%d control=%lx/%lx requested=%lx/%lx\n",result,seed.gprs[0],expected,Read().fpcr,Read().fpsr,changed.fpcr,changed.fpsr);
    }
  }
  gprs[Legacy::Rsp]+=8; *next=rip; return 1;
}
}
int main(int argc,char**argv) {
#if defined(LUMEN_PS5_FEX_C13)
    if (LumenAndroidFexCoreConfigureMemoryHandoffV1(
            LUMEN_FEX_MEMORY_EXCLUSIVE_PUBLICATION_V1) != 1) return 2;
#endif
#if defined(LUMEN_FEX_NUMERICAL_PROBE_DIRECT_LINK)
  if(argc!=1) return 2;
  (void)argv;
  Context c{};
  c.api={&LumenAndroidFexCoreRegisterHostCallTargetV1,
    &LumenAndroidFexCoreRootReturnAddressV1, &LumenAndroidFexCoreRunGuestV2,
    &LumenAndroidFexCoreBeginGuestMemoryMutationV1, &LumenAndroidFexCoreEndGuestMemoryMutationV2};
#else
  if(argc!=2) return 2;
  void* lib=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);
  if(!lib) {std::fprintf(stderr,"dlopen: %s\n",dlerror());return 2;}
  auto symbol=[&]<class T>(const char*name){return reinterpret_cast<T>(dlsym(lib,name));};
  Context c{};
  c.api={symbol.template operator()<decltype(c.api.registerTarget)>("LumenAndroidFexCoreRegisterHostCallTargetV1"),
    symbol.template operator()<decltype(c.api.rootReturn)>("LumenAndroidFexCoreRootReturnAddressV1"),
    symbol.template operator()<decltype(c.api.run)>("LumenAndroidFexCoreRunGuestV2"),
    symbol.template operator()<decltype(c.api.beginMutation)>("LumenAndroidFexCoreBeginGuestMemoryMutationV1"),
    symbol.template operator()<decltype(c.api.endMutation)>("LumenAndroidFexCoreEndGuestMemoryMutationV2")};
  // Keep the runtime image loaded: its process allocator/signal state owns it.
#endif
  if(!c.api.run || !c.api.rootReturn || !c.api.registerTarget || !c.api.beginMutation || !c.api.endMutation)return 2;
  const Native saved=Read();
  c.page=sysconf(_SC_PAGESIZE);c.sentinel=c.api.rootReturn();c.target=reinterpret_cast<unsigned long>(&Identity);
  if(!c.sentinel || !c.api.registerTarget(c.target))return 2;
  void*code=mmap(nullptr,c.page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
  void*data=mmap(nullptr,c.page*4,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
  if(code==MAP_FAILED || data==MAP_FAILED)return 2;
  c.code=reinterpret_cast<unsigned long>(code);c.data=reinterpret_cast<unsigned long>(data);
  std::memcpy(code,Guest.data(),Guest.size());
  std::memcpy(static_cast<char*>(code)+128,Nested.data(),Nested.size());
  static_cast<unsigned char*>(code)[136]=0xc3;
  std::memcpy(static_cast<char*>(code)+256,Denormal.data(),Denormal.size());
  std::memcpy(static_cast<char*>(code)+384,DenormalCompareConvert.data(),DenormalCompareConvert.size());
  if(mprotect(code,c.page,PROT_READ)!=0)return 2;
  unsigned cases=0,failures=0;
  auto*w=static_cast<unsigned*>(data);
  constexpr int positive[4]={2,1,2,1},negative[4]={-2,-2,-1,-1};
  for(unsigned sign=0;sign!=2;++sign)for(unsigned sse=0;sse!=4;++sse)for(unsigned x87=0;x87!=4;++x87)for(unsigned nested=0;nested!=2;++nested) {
    w[0]=0x3fe00000U | (sign<<31);w[1]=0x1f80 | (sse<<13);w[2]=0x037f | (x87<<10);
    std::memset(w+16,0,32);
    c.nested=nested;c.calls=0;
    // Caller carries a different rounding mode, flush mode and accrued flags.
    Write({0x1000000UL | (static_cast<unsigned long>((sse+1)%4)<<22),0x11});c.host=Read();
    const unsigned long stack=c.data+c.page*2-8;
    std::memcpy(reinterpret_cast<void*>(stack),&c.sentinel,8);
    LumenAndroidFexCoreGuestStateV1 seed{sizeof(seed),1,c.code,{},0,0,0,0,0};
    seed.gprs[Legacy::Rsp]=stack;seed.gprs[Legacy::Rdi]=c.data;seed.gprs[Legacy::Rsi]=c.data+64;seed.gprs[Legacy::Rbx]=c.target;
    const auto result=c.api.run(&seed,&Callback,&Query,&c);
    const int* expected=sign?negative:positive;
    bool ok=result==0 && c.calls==1 && Equal(Read(),c.host);
    for(unsigned offset:{16U,20U}) ok &= static_cast<int>(w[offset])==expected[sse] && static_cast<int>(w[offset+1])==expected[x87] &&
       (w[offset+2]&0xffc0U)==w[1] && (w[offset+3]&0xffffU)==w[2];
    ++cases;
    if(!ok){++failures;std::printf("FAIL sign=%u sse=%u x87=%u nested=%u code=%u calls=%u before=%08x/%08x/%08x/%08x after=%08x/%08x/%08x/%08x callerRestored=%u\n",sign,sse,x87,nested,result,c.calls,w[16],w[17],w[18],w[19],w[20],w[21],w[22],w[23],Equal(Read(),c.host));}
  }
  unsigned denormalCases=0,denormalFailures=0;
  constexpr unsigned input[4]={0x00000001U,0x00800000U,0x80000001U,0x80800000U};
  constexpr unsigned scale[4]={0x4b800000U,0x3f000000U,0x4b800000U,0x3f000000U};
  constexpr unsigned gradual[4]={0x01000000U,0x00400000U,0x81000000U,0x80400000U};
  const auto controls=[](unsigned bits){return 0x1f80U | ((bits&1U)?0x40U:0U) | ((bits&2U)?0x8000U:0U);};
  for(unsigned initial=0;initial!=4;++initial)for(unsigned second=0;second!=4;++second){
    std::memcpy(w,input,sizeof(input));std::memcpy(w+4,scale,sizeof(scale));w[8]=controls(initial);w[9]=controls(second);
    std::memset(w+32,0,80);
    Write({3UL<<22,0x11});c.host=Read();
    const unsigned long stack=c.data+c.page*2-8;std::memcpy(reinterpret_cast<void*>(stack),&c.sentinel,8);
    LumenAndroidFexCoreGuestStateV1 seed{sizeof(seed),1,c.code+256,{},0,0,0,0,0};
    seed.gprs[Legacy::Rsp]=stack;seed.gprs[Legacy::Rdi]=c.data;seed.gprs[Legacy::Rsi]=c.data+128;
    const auto result=c.api.run(&seed,&Callback,&Query,&c);
    bool ok=result==0 && Equal(Read(),c.host);
    for(unsigned phase=0;phase!=2;++phase){
      const unsigned pair=phase?second:initial;
      for(unsigned lane=0;lane!=4;++lane){
        const bool flush=(lane%2==0)?(pair&1U):(pair&2U);
        const unsigned expected=flush?(input[lane]&0x80000000U):gradual[lane];
        ok &= w[32+phase*8+lane]==expected;
      }
      ok &= (w[32+phase*8+4]&0xffc0U)==controls(pair);
    }
    ok &= w[48]==gradual[0];
    ++denormalCases;
    if(!ok){++denormalFailures;std::printf("DENORMAL_FAIL initial=%u second=%u code=%u phase0=%08x/%08x/%08x/%08x phase1=%08x/%08x/%08x/%08x x87=%08x nativeRestored=%u\n",initial,second,result,w[32],w[33],w[34],w[35],w[40],w[41],w[42],w[43],w[48],Equal(Read(),c.host));}
  }
  unsigned compareConvertCases=0,compareConvertFailures=0;
  for(unsigned pair=0;pair!=4;++pair) {
    std::memcpy(w,input,sizeof(input));w[8]=controls(pair) | (2U<<13); // guest upward
    std::memset(w+32,0,32);
    Write({3UL<<22,0x11});c.host=Read();
    const unsigned long stack=c.data+c.page*2-8;std::memcpy(reinterpret_cast<void*>(stack),&c.sentinel,8);
    LumenAndroidFexCoreGuestStateV1 seed{sizeof(seed),1,c.code+384,{},0,0,0,0,0};
    seed.gprs[Legacy::Rsp]=stack;seed.gprs[Legacy::Rdi]=c.data;seed.gprs[Legacy::Rsi]=c.data+128;
    const auto result=c.api.run(&seed,&Callback,&Query,&c);
    const unsigned expectedTiny=(pair&1U)?0U:0xffffffffU;
    const unsigned expectedInteger=(pair&1U)?0U:1U;
    const bool ok=result==0 && Equal(Read(),c.host) && w[32]==expectedTiny &&
      w[33]==0xffffffffU && w[34]==0 && w[35]==0 && w[36]==expectedInteger;
    ++compareConvertCases;
    if(!ok) {++compareConvertFailures;std::printf("COMPARE_CONVERT_FAIL control=%u code=%u masks=%08x/%08x/%08x/%08x integer=%08x nativeRestored=%u\n",pair,result,w[32],w[33],w[34],w[35],w[36],Equal(Read(),c.host));}
  }
  Write(saved);
  const auto token=c.api.beginMutation();
  bool closed=false;
  struct Close {Context*c;bool*closed;} close{&c,&closed};
  const auto ended=token?c.api.endMutation(token,c.code,c.page,1,+[](void*p)noexcept{auto&v=*static_cast<Close*>(p);*v.closed=munmap(reinterpret_cast<void*>(v.c->code),v.c->page)==0 && munmap(reinterpret_cast<void*>(v.c->data),v.c->page*4)==0;},&close):0;
  const bool pass=cases==64 && !failures && !c.hostMismatch && !c.nestedMismatch &&
    denormalCases==16 && !denormalFailures && compareConvertCases==4 && !compareConvertFailures &&
    ended && closed && Equal(Read(),saved);
  std::printf("LUMEN_FEX_NUMERICAL_PROBE_V1 cases=%u failures=%u callbackEnvironmentMismatch=%u nestedMismatch=%u denormalCases=%u denormalFailures=%u compareConvertCases=%u compareConvertFailures=%u page=%lu hwcap=%lx hwcap2=%lx mappingsClosed=%u nativeRestored=%u pass=%u\n",cases,failures,c.hostMismatch,c.nestedMismatch,denormalCases,denormalFailures,compareConvertCases,compareConvertFailures,c.page,getauxval(AT_HWCAP),getauxval(AT_HWCAP2),closed,Equal(Read(),saved),pass);
  return pass?0:1;
}
