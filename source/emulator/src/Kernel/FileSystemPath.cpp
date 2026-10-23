#include "FileSystemPath.h"

namespace Lumen::Libs::LibKernel::FileSystem::Path {

namespace {

String NormalizeForMountLookup(const String& path, bool directory)
{
	// This spelling exists only for matching and translated output. Paths that
	// miss every mount retain their original byte-for-byte representation.
	String normalized;
	bool   previous_was_slash = false;

	for (uint32_t index = 0; index < path.Size(); index++)
	{
		const char32_t ch       = path.At(index);
		const bool     is_slash = (ch == U'/' || ch == U'\\');

		if (!is_slash)
		{
			normalized += ch;
			previous_was_slash = false;
		} else if (!previous_was_slash)
		{
			normalized += U'/';
			previous_was_slash = true;
		}
	}

	if (directory && !normalized.EndsWith(U'/'))
	{
		normalized += U'/';
	}

	return normalized;
}

} // namespace

MountTranslation TranslateMountedPath(const String& mounted_path, const String& mount_point, const String& real_directory, bool directory)
{
	const auto lookup_path        = NormalizeForMountLookup(mounted_path, directory);
	const auto lookup_mount_point = NormalizeForMountLookup(mount_point, true);

	if (!lookup_path.StartsWith(lookup_mount_point))
	{
		return {false, mounted_path};
	}

	return {true, real_directory + lookup_path.RemoveFirst(lookup_mount_point.Size())};
}

} // namespace Lumen::Libs::LibKernel::FileSystem::Path
