#ifndef INCLUDE_LUMEN_CORE_BYTEBUFFER_H_
#define INCLUDE_LUMEN_CORE_BYTEBUFFER_H_

#include "Lumen/Core/Common.h"
#include "Lumen/Core/Vector.h"

#include <cstddef> // IWYU pragma: export

namespace Lumen::Core {

using Byte = std::byte;

class ByteBuffer: public Vector<Byte>
{
public:
	ByteBuffer() = default;
	using Vector<Byte>::Vector;

	ByteBuffer(std::initializer_list<uint8_t> list): ByteBuffer(static_cast<uint32_t>(list.size()), false) // @suppress("Ambiguous problem")
	{
		Byte* values_ptr = GetData();
		for (const uint8_t& e: list)
		{
			*(values_ptr++) = static_cast<Byte>(e);
		}
	}

	explicit ByteBuffer(const void* buf, uint32_t size): ByteBuffer(size, false) // @suppress("Ambiguous problem")
	{
		Byte* values_ptr = GetData();
		std::memcpy(values_ptr, buf, size);
	}
};

} // namespace Lumen::Core

#endif /* INCLUDE_LUMEN_CORE_BYTEBUFFER_H_ */
