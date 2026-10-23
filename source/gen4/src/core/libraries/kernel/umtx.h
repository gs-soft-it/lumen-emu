// SPDX-License-Identifier: MIT
#pragma once
#include "common/types.h"
namespace Libraries::Kernel {
s32 PS4_SYSV_ABI posix_umtx_op(u64 object, s32 operation, u64 value, u64 address1, u64 address2);
}
