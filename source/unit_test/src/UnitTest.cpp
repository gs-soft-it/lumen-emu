#include "Lumen/UnitTest.h"

namespace Lumen::UnitTest {

UT_LINK(CoreCharString);
UT_LINK(CoreCharString8);
UT_LINK(CoreMSpace);
UT_LINK(CoreDateTime);

LUMEN_SUBSYSTEM_INIT(UnitTest)
{
	testing::InitGoogleTest(LUMEN_SUBSYSTEM_ARGC, LUMEN_SUBSYSTEM_ARGV);
}

LUMEN_SUBSYSTEM_UNEXPECTED_SHUTDOWN(UnitTest) {}

LUMEN_SUBSYSTEM_DESTROY(UnitTest) {}

bool unit_test_all()
{
	return RUN_ALL_TESTS() == 0;
}

} // namespace Lumen::UnitTest
