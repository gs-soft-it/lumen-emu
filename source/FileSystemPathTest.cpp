#include "Kernel/FileSystemPath.h"

#include <cstdio>

namespace {

namespace FileSystemPath = Lumen::Libs::LibKernel::FileSystem::Path;

struct TranslationCase
{
	const char*     label;
	const char32_t* mounted_path;
	const char32_t* mount_point;
	const char32_t* real_directory;
	bool            directory;
	bool            expected_match;
	const char32_t* expected_path;
};

bool ExpectTranslation(const char* label, const Lumen::String& mounted_path, const Lumen::String& mount_point,
                       const Lumen::String& real_directory, bool directory, bool expected_match, const Lumen::String& expected_path)
{
	const auto actual = FileSystemPath::TranslateMountedPath(mounted_path, mount_point, real_directory, directory);

	if (actual.matched != expected_match || actual.path != expected_path)
	{
		std::fprintf(stderr, "filesystem-path-test failed (%s): matched=%d path=%s\n", label, actual.matched ? 1 : 0, actual.path.C_Str());
		return false;
	}

	return true;
}

} // namespace

int main()
{
	constexpr TranslationCase cases[] = {
	    {"regular file", U"/app0/assets/audio/project.bank", U"/app0/", U"/host/game/", false, true,
	     U"/host/game/assets/audio/project.bank"},
	    {"duplicate leading slash", U"//app0/assets/audio/project.bank", U"/app0/", U"/host/game/", false, true,
	     U"/host/game/assets/audio/project.bank"},
	    {"duplicate interior slash", U"//app0//assets///audio/project.bank", U"/app0/", U"/host/game/", false, true,
	     U"/host/game/assets/audio/project.bank"},
	    {"duplicate mount slash", U"/app0/assets/audio/project.bank", U"//app0//", U"/host/game/", false, true,
	     U"/host/game/assets/audio/project.bank"},
	    {"backslash file", U"\\app0\\assets\\audio\\project.bank", U"/app0/", U"/host/game/", false, true,
	     U"/host/game/assets/audio/project.bank"},
	    {"directory", U"//app0//assets/audio", U"/app0/", U"/host/game/", true, true, U"/host/game/assets/audio/"},
	    {"unmatched duplicate path", U"/data//audio/project.bank", U"/app0/", U"/host/game/", false, false, U"/data//audio/project.bank"},
	    {"unmatched UNC file", U"//server/share//audio/project.bank", U"/app0/", U"/host/game/", false, false,
	     U"//server/share//audio/project.bank"},
	    {"unmatched UNC directory", U"//server/share//audio", U"/app0/", U"/host/game/", true, false, U"//server/share//audio"},
	    {"near mount prefix", U"//app01/assets/audio/project.bank", U"/app0/", U"/host/game/", false, false,
	     U"//app01/assets/audio/project.bank"},
	    {"UNC host mount", U"//app0/assets/audio/project.bank", U"/app0/", U"//server/share/game/", false, true,
	     U"//server/share/game/assets/audio/project.bank"},
	};

	bool ok = true;
	for (const auto& test: cases)
	{
		ok = ExpectTranslation(test.label, test.mounted_path, test.mount_point, test.real_directory, test.directory, test.expected_match,
		                       test.expected_path) &&
		     ok;
	}

	if (!ok)
	{
		return 1;
	}

	std::printf("filesystem-path-test passed\n");
	return 0;
}
