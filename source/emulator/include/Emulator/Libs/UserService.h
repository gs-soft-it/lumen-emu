#ifndef LUMEN_EMULATOR_LIBS_USER_SERVICE_H_
#define LUMEN_EMULATOR_LIBS_USER_SERVICE_H_

#include "Emulator/Common.h"

#include <cstdint>

namespace Lumen::Libs::UserService {

// Local profile for sceUserServiceGetUserColor (NID lUoqwTQu4Go), the first
// UserService operation reached by a title beyond the offline login model. The
// runtime keeps exactly one logged-in user - the identity published by
// UserServiceGetInitialUser and UserServiceGetLoginUserIdList - so a query for
// that user publishes the first value of the documented
// SceUserServiceUserColor enumeration into the caller's writable dword; other
// user ids are answered with the documented not-logged-in error. Every
// rejection publishes nothing. The console value depends on the colour stored
// in the user profile and is not measured; the remaining UserService
// identities of this library keep their existing legacy registrations.
int32_t LUMEN_SYSV_ABI UserServiceGetUserColor(int32_t user_id, int32_t* color);

} // namespace Lumen::Libs::UserService

#endif // LUMEN_EMULATOR_LIBS_USER_SERVICE_H_
