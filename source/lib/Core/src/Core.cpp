#include "Lumen/Core/Core.h"

#include "Lumen/Core/ArrayWrapper.h" // IWYU pragma: associated
#include "Lumen/Core/ByteBuffer.h"   // IWYU pragma: associated
#include "Lumen/Core/Common.h"       // IWYU pragma: associated
#include "Lumen/Core/Database.h"
#include "Lumen/Core/Debug.h"
#include "Lumen/Core/File.h"
#include "Lumen/Core/Hash.h" // IWYU pragma: associated
#include "Lumen/Core/Language.h"
#include "Lumen/Core/LinkList.h"  // IWYU pragma: associated
#include "Lumen/Core/MagicEnum.h" // IWYU pragma: associated
#include "Lumen/Core/MemoryAlloc.h"
#include "Lumen/Core/RefCounter.h"  // IWYU pragma: associated
#include "Lumen/Core/SafeDelete.h"  // IWYU pragma: associated
#include "Lumen/Core/SimpleArray.h" // IWYU pragma: associated
#include "Lumen/Core/Singleton.h"   // IWYU pragma: associated
#include "Lumen/Core/Vector.h"      // IWYU pragma: associated
#include "Lumen/Core/VirtualMemory.h"

namespace Lumen::Core {

LUMEN_SUBSYSTEM_INIT(Core)
{
	core_memory_init();
	core_file_init();
	core_debug_init(parent->GetArgv()[0]);
	Language::Init();
	Database::Init();
	VirtualMemory::Init();
}

LUMEN_SUBSYSTEM_UNEXPECTED_SHUTDOWN(Core) {}

LUMEN_SUBSYSTEM_DESTROY(Core) {}

} // namespace Lumen::Core
