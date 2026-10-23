#ifndef EMULATOR_SRC_KERNEL_FILESYSTEMPATH_H_
#define EMULATOR_SRC_KERNEL_FILESYSTEMPATH_H_

#include "Lumen/Core/String.h"

namespace Lumen::Libs::LibKernel::FileSystem::Path {

struct MountTranslation
{
	bool   matched;
	String path;
};

// A miss returns mounted_path unchanged. real_directory is never normalized,
// preserving host paths such as UNC shares when a mount does match.
[[nodiscard]] MountTranslation TranslateMountedPath(const String& mounted_path, const String& mount_point, const String& real_directory,
                                                    bool directory);

} // namespace Lumen::Libs::LibKernel::FileSystem::Path

#endif /* EMULATOR_SRC_KERNEL_FILESYSTEMPATH_H_ */
