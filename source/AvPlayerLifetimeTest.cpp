#include "Emulator/Audio.h"

#include <cstdio>

int main()
{
#if !defined(LUMEN_AVPLAYER_LIFETIME_TEST_HOOK)
	std::fprintf(stderr, "AvPlayerLifetimeTest: test hook is disabled\n");
	return 2;
#else
	if (!Lumen::Libs::Audio::AvPlayer::AvPlayerLifetimeRaceTest() ||
	    !Lumen::Libs::Audio::AvPlayer::AvPlayerBufferHandoffTest())
	{
		std::fprintf(stderr, "AvPlayerLifetimeTest: FAIL\n");
		return 1;
	}
	std::printf("AvPlayerLifetimeTest: PASS\n");
	return 0;
#endif
}
