#include "Emulator/Graphics/Graphics.h"

#include "Emulator/Graphics/Pm4.h"
#include "Emulator/Loader/RuntimeLinker.h"

#include "Lumen/Core/DbgAssert.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#ifdef LUMEN_EMU_ENABLED

namespace Lumen::Libs::Graphics::Gen4 {
namespace {

constexpr size_t MAX_MARKER_STRING_BYTES = 32768;
constexpr uint64_t MAX_PM4_DWORDS = 0x4001;
constexpr uint32_t SET_MARKER_DISCRIMINATOR = 0x68750003u;

bool ReadMarkerString(const char* guest_string, std::string* text)
{
#if defined(__APPLE__) && \
    (defined(LUMEN_FEXCORE_RUNTIME) || defined(LUMEN_RUNTIME_LINKER_ADDRESS_TEST_HOOK))
	return Loader::ReadGuestString(reinterpret_cast<uint64_t>(guest_string),
	                               MAX_MARKER_STRING_BYTES + 1, text, false);
#elif defined(__aarch64__) || \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64)
	text->clear();
	const uint64_t address = reinterpret_cast<uint64_t>(guest_string);
	for (size_t index = 0; index <= MAX_MARKER_STRING_BYTES; ++index)
	{
		if (address > UINT64_MAX - index) return false;
		char byte = 0;
		if (!Loader::ReadGuestMemory(address + index, &byte, sizeof(byte))) return false;
		if (byte == '\0') return true;
		text->push_back(byte);
	}
	return true;
#else
	(void)guest_string;
	(void)text;
	return false;
#endif
}

bool WriteMarkerPacket(uint32_t* guest_command, const std::vector<uint8_t>& packet)
{
#if defined(__aarch64__) || \
    (LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64)
	// The Apple FEX implementation of WriteGuestMemory holds a guest-data
	// lease across this entire validated copy.
	return Loader::WriteGuestMemory(reinterpret_cast<uint64_t>(guest_command), packet.data(), packet.size());
#else
	(void)guest_command;
	(void)packet;
	return false;
#endif
}

} // namespace

int32_t LUMEN_SYSV_ABI GraphicsInsertSetMarker(uint32_t* command, uint32_t words, const char* marker)
{
	if (command == nullptr || marker == nullptr) return -1;

	std::string text;
	if (!ReadMarkerString(marker, &text))
	{
		EXIT("Gnm SetMarker string is not readable");
	}
	// ReadGuestString also returns true when it reaches its bound without a
	// NUL. Such input has no qualified command-buffer representation.
	if (text.size() > MAX_MARKER_STRING_BYTES)
	{
		EXIT("Gnm SetMarker string is not terminated within the limit");
	}

	const uint64_t length = text.size();
	const uint64_t required_words = 2 + (length + 8) / 4 + 2 * ((length + 12) / 8);
	if (required_words > MAX_PM4_DWORDS)
	{
		EXIT("Gnm SetMarker packet exceeds the PM4 count field");
	}
	if (words != required_words) return -1;

	const uint64_t command_address = reinterpret_cast<uint64_t>(command);
	const uint64_t marker_address = reinterpret_cast<uint64_t>(marker);
	const uint64_t packet_bytes = required_words * sizeof(uint32_t);
	const uint64_t source_bytes = length + 1;
	if (command_address > UINT64_MAX - packet_bytes || marker_address > UINT64_MAX - source_bytes ||
	    (command_address < marker_address + source_bytes && marker_address < command_address + packet_bytes))
	{
		EXIT("Gnm SetMarker source and destination ranges are invalid or overlap");
	}

	std::vector<uint8_t> packet(static_cast<size_t>(packet_bytes), 0);
	const uint32_t header = LUMEN_PM4(required_words, Pm4::IT_NOP, Pm4::R_ZERO);
	std::memcpy(packet.data(), &header, sizeof(header));
	std::memcpy(packet.data() + sizeof(header), &SET_MARKER_DISCRIMINATOR, sizeof(SET_MARKER_DISCRIMINATOR));
	if (!text.empty()) std::memcpy(packet.data() + 2 * sizeof(uint32_t), text.data(), text.size());
	if (!WriteMarkerPacket(command, packet))
	{
		EXIT("Gnm SetMarker command buffer is not writable");
	}
	return 0;
}

} // namespace Lumen::Libs::Graphics::Gen4

#endif // LUMEN_EMU_ENABLED
