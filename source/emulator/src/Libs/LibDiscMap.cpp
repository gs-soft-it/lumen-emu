#include "Lumen/Core/Common.h"
#include "Lumen/Core/String.h"

#include "Emulator/Common.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs {

LIB_VERSION("DiscMap", 1, "DiscMap", 1, 1);

namespace DiscMap {

constexpr int DISC_MAP_ERROR_INVALID_ARGUMENT = static_cast<int>(0x81100001u);
constexpr int DISC_MAP_ERROR_NO_BITMAP_INFO   = static_cast<int>(0x81100004u);

static LUMEN_SYSV_ABI int DiscMapIsRequestOnHDD(const char* file, int64_t offset, int64_t nbytes, int* result)
{
	PRINT_NAME();

	printf("\tfile = %s\n", file != nullptr ? file : "<null>");
	printf("\toffset = %" PRId64 "\n", offset);
	printf("\tnbytes = %" PRId64 "\n", nbytes);
	printf("\tresult = %p\n", static_cast<void*>(result));

	return DISC_MAP_ERROR_NO_BITMAP_INFO;
}

static LUMEN_SYSV_ABI int GetMappingInfo(const char* file, int64_t offset, int64_t nbytes, int* flags, int* result1, int* result2)
{
	PRINT_NAME();

	printf("\tfile = %s\n", file != nullptr ? file : "<null>");
	printf("\toffset = %" PRId64 "\n", offset);
	printf("\tnbytes = %" PRId64 "\n", nbytes);

	if (flags == nullptr || result1 == nullptr || result2 == nullptr)
	{
		return DISC_MAP_ERROR_INVALID_ARGUMENT;
	}

	*flags   = 0;
	*result1 = 0;
	*result2 = 0;

	return 0;
}

static LUMEN_SYSV_ABI int GetMappingInfoOnHDD(const char* file, int64_t offset, int64_t nbytes, int* flags, int* result1, int* result2)
{
	PRINT_NAME();

	printf("\tfile = %s\n", file != nullptr ? file : "<null>");
	printf("\toffset = %" PRId64 "\n", offset);
	printf("\tnbytes = %" PRId64 "\n", nbytes);
	printf("\tflags = %p\n", static_cast<void*>(flags));
	printf("\tresult1 = %p\n", static_cast<void*>(result1));
	printf("\tresult2 = %p\n", static_cast<void*>(result2));

	return DISC_MAP_ERROR_NO_BITMAP_INFO;
}

} // namespace DiscMap

LIB_DEFINE(InitDiscMap_1)
{
	LIB_FUNC("lbQKqsERhtE", DiscMap::DiscMapIsRequestOnHDD);
	LIB_FUNC("fJgP+wqifno", DiscMap::GetMappingInfo);
	LIB_FUNC("ioKMruft1ek", DiscMap::GetMappingInfoOnHDD);
}

} // namespace Lumen::Libs

#endif // LUMEN_EMU_ENABLED
