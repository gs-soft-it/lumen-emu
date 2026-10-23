#ifndef EMULATOR_INCLUDE_EMULATOR_LOADER_SYSTEMCONTENT_H_
#define EMULATOR_INCLUDE_EMULATOR_LOADER_SYSTEMCONTENT_H_

#include "Lumen/Core/Common.h"
#include "Lumen/Core/String.h"

#include "Emulator/Common.h"

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs::Graphics {
class Image;
} // namespace Lumen::Libs::Graphics

namespace Lumen::Loader {

// Drop metadata and decoded icon data owned by the completed guest session.
// The singleton itself remains alive for the next SystemContentLoadParamSfo.
// The caller must have already stopped all guest and window consumers.
[[nodiscard]] bool SystemContentResetForNextSession();

void                   SystemContentLoadParamSfo(const String& file_name);
bool                   SystemContentParamSfoGetInt(const char* name, int32_t* value);
bool                   SystemContentParamSfoGetString(const char* name, String* value);
bool                   SystemContentParamSfoGetString(const char* name, char* value, size_t value_size);
Libs::Graphics::Image* SystemContentGetIcon();
bool                   SystemContentGetChunksNum(uint32_t* num);

#if defined(LUMEN_RUNTIME_LIFECYCLE_TEST_HOOK)
[[nodiscard]] bool SystemContentSessionResetContractTest();
#endif

} // namespace Lumen::Loader

#endif // LUMEN_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_LOADER_SYSTEMCONTENT_H_ */
