#ifndef EMULATOR_INCLUDE_EMULATOR_KERNEL_UMTX_H_
#define EMULATOR_INCLUDE_EMULATOR_KERNEL_UMTX_H_

#include "Lumen/Core/Common.h"
#include "Lumen/Core/VirtualMemory.h"

#include "Emulator/Common.h"

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs::LibKernel::Umtx
{

// The POSIX namespace exported by libkernel carries the FreeBSD `_umtx_op`
// kernel entry point (NID `04AjkP0jO9U`, library libkernel v1 / module
// libkernel v1.1). Guest code compiled against the console SDK uses it as the
// low-level wait and wake primitive behind semaphores and other user-space
// synchronisation: the address passed in `obj` is the wait key and the console
// kernel wakes that address when the matching release runs.
//
// This adapter keeps a process-local object table keyed by the guest address
// and waits in bounded host slices, delivering pending guest exceptions
// between slices like the other HLE waits.
int LUMEN_SYSV_ABI UmtxOp(uint64_t obj, int32_t op, uint32_t val, uint64_t uaddr1, uint64_t uaddr2);

// --- Console semaphore profile ----------------------------------------------
// The `sem_t` the console kernel exposes is the sixteen-byte record the guest
// publishes itself: {magic | registration handle, waiters hint, count, flags}.
// The waiters hint is the address the console waits on and the release path
// reads before deciding to wake, so this adapter publishes the hint while a
// waiter is inside the wait and re-checks the adjacent count every slice. The
// profile is shared by `_umtx_op` SEM_WAIT and the POSIX semaphore entry
// points, which keeps a title's own statically linked semaphore code and the
// HLE on the same object.

// Atomic 32-bit guest word access: a lease on the FEX targets, a validated
// direct pointer elsewhere. The active targets map guest memory at its guest
// addresses, so a host atomic on the word is the same operation the guest's
// own lock-prefixed access performs.
class LeasedGuestWord final
{
public:
	LeasedGuestWord() = default;
	LUMEN_CLASS_NO_COPY(LeasedGuestWord);

	[[nodiscard]] bool Open(uint64_t address, bool write);
	[[nodiscard]] uint32_t* Word() const noexcept { return m_word; }

private:
#if defined(LUMEN_FEXCORE_RUNTIME) && \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID)
	Core::VirtualMemory::GuestDataLease m_lease {};
#endif
	uint32_t* m_word = nullptr;
};

// Wake every waiter of the semaphore whose waiters word is `waiters_word`.
void WakeSemaphoreWaitersWord(uint64_t waiters_word);

// Wait for a token on the semaphore whose waiters word is `waiters_word`: the
// hint is published before the wait with a bounded borrow and cleared after it,
// the adjacent count word is re-checked every bounded slice with a validated
// read, and the wait completes as soon as a token is available or a wake
// arrives. The lease never spans a slice, so a pending VM mutation can drain
// its readers while this wait sleeps. Returns false only when the budget
// expired first (`infinite` keeps waiting). A no-op outside a guest thread.
[[nodiscard]] bool WaitSemaphoreWaitersWord(uint64_t waiters_word, uint32_t relative_micros, bool infinite);

// Consume one token when the count is positive. `taken` reports whether a
// token was consumed; a false return reports an unreadable or unwritable word
// through `error` (POSIX errno).
[[nodiscard]] bool TakeSemaphoreToken(uint64_t slot, bool* taken, int* error);

// Release one token and wake the waiters. Returns 0 or a POSIX errno
// (`EOVERFLOW` when the count is already at its maximum).
int ReleaseSemaphoreToken(uint64_t slot);

} // namespace Lumen::Libs::LibKernel::Umtx

#endif // LUMEN_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_KERNEL_UMTX_H_ */
