#ifndef INCLUDE_LUMEN_CORE_REFCOUNTERPOLICY_H_
#define INCLUDE_LUMEN_CORE_REFCOUNTERPOLICY_H_

namespace Lumen::Core {

// Default OFF. Configure after platform environment setup and before workers
// start; each new RefCounter captures its mode, so existing objects are safe.
void ConfigureAtomicRefsFromEnvironment() noexcept;
bool AtomicRefsEnabled() noexcept;

} // namespace Lumen::Core

#endif
