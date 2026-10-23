#ifndef INCLUDE_LUMEN_SCRIPTS_SCRIPTSLOADER_H_
#define INCLUDE_LUMEN_SCRIPTS_SCRIPTSLOADER_H_

#include "Lumen/Core/String.h"

namespace Lumen::Scripts {

void   SetLoadError(const String& err);
String GetLoadError();
void   ResetLoadError();
bool   RunScript(const String& lua_file_name);

} // namespace Lumen::Scripts

#endif /* INCLUDE_LUMEN_SCRIPTS_SCRIPTSLOADER_H_ */
