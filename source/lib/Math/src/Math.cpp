#include "Lumen/Math/MathAll.h" // IWYU pragma: associated
#include "Lumen/Math/Rand.h"

namespace Lumen::Math {

LUMEN_SUBSYSTEM_INIT(Math)
{
	Rand::Init();
}

LUMEN_SUBSYSTEM_UNEXPECTED_SHUTDOWN(Math) {}

LUMEN_SUBSYSTEM_DESTROY(Math) {}

} // namespace Lumen::Math
