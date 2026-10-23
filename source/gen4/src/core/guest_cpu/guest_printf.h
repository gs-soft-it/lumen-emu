// SPDX-License-Identifier: MIT
#pragma once

#include "core/guest_cpu/hle_call_adapter.h"
#include <algorithm>
#include <bit>
#include <climits>
#include <cstdio>
#include <optional>

namespace Core::GuestCpu {

// A guest SysV va_list is data, not a native va_list. Each access is validated
// using the same mapping lifetime as the HLE invocation.
struct GuestVaList {
    u32 gp_offset{}, fp_offset{};
    u64 overflow_arg_area{}, reg_save_area{};
};
static_assert(sizeof(GuestVaList) == 24);

class GuestFormatArguments {
public:
    GuestFormatArguments(HleCallFrame& state, size_t fixed_gp)
        : frame(state), cursor{state, fixed_gp} {}
    GuestFormatArguments(HleCallFrame& state, GuestVaList list)
        : frame(state), cursor{state}, saved(list) {}

    bool Valid(u64 address, size_t size, bool writable = false) const {
        return address && size && address <= UINT64_MAX - size && frame.validate_range &&
               frame.validate_range(frame.validate_context, address, size, writable);
    }
    std::optional<u64> Integer() { return Next(false); }
    std::optional<u64> Vector() { return Next(true); }
    std::optional<std::string> String(u64 address, size_t limit, bool require_terminator) const {
        std::string result;
        if (!address) return std::string{"(null)"}.substr(0, limit);
        while (result.size() < limit) {
            const auto next = address + result.size();
            if (next < address || !Valid(next, 1)) return std::nullopt;
            const char c = *reinterpret_cast<const char*>(next);
            if (!c) return result;
            result.push_back(c);
        }
        return require_terminator ? std::nullopt : std::optional{std::move(result)};
    }
private:
    std::optional<u64> Next(bool vector) {
        if (!saved) return vector ? cursor.NextVector() : cursor.NextInteger();
        auto& list = *saved;
        const auto offset = vector ? list.fp_offset : list.gp_offset;
        if ((!vector && (offset > 48 || offset % 8)) ||
            (vector && (offset < 48 || offset > 176 || (offset - 48) % 16))) return std::nullopt;
        u64 address{};
        if (offset < (vector ? 176u : 48u)) {
            if (list.reg_save_area > UINT64_MAX - offset) return std::nullopt;
            address = list.reg_save_area + offset;
            (vector ? list.fp_offset : list.gp_offset) += vector ? 16 : 8;
        } else {
            address = list.overflow_arg_area;
            if (address > UINT64_MAX - 8) return std::nullopt;
            list.overflow_arg_area += 8;
        }
        if (!Valid(address, 8)) return std::nullopt;
        u64 value; std::memcpy(&value, reinterpret_cast<void*>(address), 8);
        return value;
    }
    HleCallFrame& frame;
    detail::CallCursor cursor;
    std::optional<GuestVaList> saved;
};

enum class GuestFormatError { None, Invalid, Fault, Overflow, Unsupported };
struct GuestFormatWrite { u64 address{}, value{}; size_t size{}; };
struct GuestFormatResult {
    std::string text;
    std::vector<GuestFormatWrite> count_writes;
    GuestFormatError error{};
};

inline GuestFormatResult FormatGuestPrintf(GuestFormatArguments& args, u64 format_address) {
    constexpr size_t MaxOutput = 1024 * 1024;
    GuestFormatResult result;
    const auto fail = [&](GuestFormatError error) { result.error = error; return result; };
    if (!format_address) return fail(GuestFormatError::Fault);
    const auto format = args.String(format_address, 65536, true);
    if (!format) return fail(GuestFormatError::Fault);
    const auto append = [&](const std::string& conversion, auto value) -> bool {
        const int length = std::snprintf(nullptr, 0, conversion.c_str(), value);
        if (length < 0 || static_cast<size_t>(length) > MaxOutput - result.text.size()) return false;
        std::vector<char> buffer(static_cast<size_t>(length) + 1);
        std::snprintf(buffer.data(), buffer.size(), conversion.c_str(), value);
        result.text.append(buffer.data(), static_cast<size_t>(length));
        return true;
    };
    for (size_t pos = 0; pos < format->size();) {
        if ((*format)[pos] != '%') {
            if (result.text.size() == MaxOutput) return fail(GuestFormatError::Overflow);
            result.text += (*format)[pos++]; continue;
        }
        ++pos;
        if (pos < format->size() && (*format)[pos] == '%') {
            if (result.text.size() == MaxOutput) return fail(GuestFormatError::Overflow);
            ++pos; result.text += '%'; continue;
        }
        std::string conversion{"%"};
        while (pos < format->size() && std::string_view{"-+ #0"}.find((*format)[pos]) != std::string_view::npos)
            conversion += (*format)[pos++];
        auto read_number = [&]() -> std::optional<unsigned> {
            unsigned n{};
            while (pos < format->size() && (*format)[pos] >= '0' && (*format)[pos] <= '9') {
                const unsigned digit = (*format)[pos++] - '0';
                if (n > (MaxOutput - digit) / 10) return std::nullopt;
                n = n * 10 + digit;
            }
            return n;
        };
        if (pos < format->size() && (*format)[pos] == '*') {
            ++pos; auto width = args.Integer();
            if (!width) return fail(GuestFormatError::Fault);
            const auto signed_width = static_cast<s32>(*width);
            if (signed_width == INT_MIN || std::abs(signed_width) > MaxOutput)
                return fail(GuestFormatError::Overflow);
            if (signed_width < 0) conversion += '-';
            conversion += std::to_string(std::abs(signed_width));
        } else {
            const auto width = read_number();
            if (!width) return fail(GuestFormatError::Overflow);
            if (*width) conversion += std::to_string(*width);
        }
        std::optional<size_t> precision;
        if (pos < format->size() && (*format)[pos] == '.') {
            ++pos;
            if (pos < format->size() && (*format)[pos] == '*') {
                ++pos; const auto p = args.Integer();
                if (!p) return fail(GuestFormatError::Fault);
                if (static_cast<s32>(*p) >= 0) precision = static_cast<s32>(*p);
            } else {
                const auto p = read_number();
                if (!p) return fail(GuestFormatError::Overflow);
                precision = *p;
            }
            if (precision) {
                if (*precision > MaxOutput) return fail(GuestFormatError::Overflow);
                conversion += '.' + std::to_string(*precision);
            }
        }
        std::string length;
        if (pos < format->size() && std::string_view{"hljztL"}.find((*format)[pos]) != std::string_view::npos) {
            length += (*format)[pos++];
            if (pos < format->size() && (length == "h" || length == "l") && (*format)[pos] == length[0])
                length += (*format)[pos++];
        }
        if (pos == format->size()) return fail(GuestFormatError::Invalid);
        const char spec = (*format)[pos++];
        if (std::string_view{"diuoxX"}.find(spec) != std::string_view::npos) {
            if (length == "L") return fail(GuestFormatError::Invalid);
            const auto value = args.Integer(); if (!value) return fail(GuestFormatError::Fault);
            const unsigned bits = length == "hh" ? 8 : length == "h" ? 16 : length.empty() ? 32 : 64;
            if (spec == 'd' || spec == 'i') {
                const s64 signed_value = bits == 8 ? static_cast<s8>(*value) : bits == 16 ? static_cast<s16>(*value) :
                    bits == 32 ? static_cast<s32>(*value) : static_cast<s64>(*value);
                if (!append(conversion + "ll" + spec, static_cast<long long>(signed_value))) return fail(GuestFormatError::Overflow);
            } else {
                const u64 unsigned_value = bits == 64 ? *value : *value & ((1ull << bits) - 1);
                if (!append(conversion + "ll" + spec, static_cast<unsigned long long>(unsigned_value))) return fail(GuestFormatError::Overflow);
            }
        } else if (std::string_view{"fFeEgGaA"}.find(spec) != std::string_view::npos) {
            if (length == "L") return fail(GuestFormatError::Unsupported); // x87 long double is memory-class.
            if (!length.empty() && length != "l") return fail(GuestFormatError::Invalid);
            const auto value = args.Vector(); if (!value) return fail(GuestFormatError::Fault);
            if (!append(conversion + spec, std::bit_cast<double>(*value))) return fail(GuestFormatError::Overflow);
        } else if (spec == 's') {
            if (!length.empty()) return fail(GuestFormatError::Unsupported);
            const auto pointer = args.Integer(); if (!pointer) return fail(GuestFormatError::Fault);
            const auto string = args.String(*pointer, precision.value_or(MaxOutput), !precision);
            if (!string) return fail(GuestFormatError::Fault);
            if (!append(conversion + spec, string->c_str())) return fail(GuestFormatError::Overflow);
        } else if (spec == 'c' || spec == 'p') {
            if (!length.empty()) return fail(GuestFormatError::Unsupported);
            const auto value = args.Integer(); if (!value) return fail(GuestFormatError::Fault);
            const bool ok = spec == 'c' ? append(conversion + spec, static_cast<int>(*value)) :
                append(conversion + spec, reinterpret_cast<void*>(*value));
            if (!ok) return fail(GuestFormatError::Overflow);
        } else if (spec == 'n') {
            if (length == "L") return fail(GuestFormatError::Invalid);
            const auto pointer = args.Integer(); if (!pointer) return fail(GuestFormatError::Fault);
            const size_t size = length == "hh" ? 1 : length == "h" ? 2 : length.empty() ? 4 : 8;
            if (!args.Valid(*pointer, size, true)) return fail(GuestFormatError::Fault);
            result.count_writes.push_back({*pointer, result.text.size(), size});
        } else {
            return fail(GuestFormatError::Unsupported); // Includes positional arguments; never consume them incorrectly.
        }
    }
    return result;
}
} // namespace Core::GuestCpu
