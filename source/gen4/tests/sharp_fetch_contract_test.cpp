// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <bit>
#include <cstddef>
#include <iostream>
#include <stdexcept>
#include "shader_recompiler/resource.h"

namespace {

void Require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

struct FixtureInfo {
    std::array<u32, 16> user_data{};
    std::array<u32, 64> flattened_ud_buf{};
    u64 pgm_base{};
};

AmdGpu::Buffer MakeBuffer() {
    AmdGpu::Buffer buffer{};
    buffer.base_address = 0x8102030405;
    buffer.stride = 16;
    buffer.num_records = 512;
    buffer.data_format = 4;
    buffer.num_format = 7;
    buffer.dst_sel_x = 4;
    buffer.dst_sel_y = 5;
    buffer.dst_sel_z = 6;
    buffer.dst_sel_w = 7;
    return buffer;
}

Shader::BufferResource ImmediateResource(AmdGpu::Buffer buffer) {
    Shader::BufferResource resource{};
    resource.sharp_fetch.immediates = std::bit_cast<std::array<u32, 4>>(buffer);
    return resource;
}

void TestIndependentWords() {
    FixtureInfo info{};
    // A neighboring float vector must never be read as the rest of a descriptor.
    info.flattened_ud_buf.fill(0x3f800000);
    const auto expected = MakeBuffer();
    const auto words = std::bit_cast<std::array<u32, 4>>(expected);
    for (u32 mask = 0; mask != 16; ++mask) {
        auto resource = ImmediateResource(expected);
        resource.sharp_fetch.offsets = {2, 19, 41, 57};
        resource.sharp_fetch.load_mask = mask;
        for (u32 i = 0; i != 4; ++i) {
            info.flattened_ud_buf[resource.sharp_fetch.offsets[i]] = words[i];
            if (mask & (1u << i)) {
                resource.sharp_fetch.immediates[i] = ~words[i];
            }
        }
        Require(resource.GetSharp(info) == expected,
                "mixed immediate/noncontiguous descriptor fetch changed a word");
    }
    for (u32 word = 0; word != 4; ++word) {
        auto resource = ImmediateResource(expected);
        resource.sharp_fetch.offsets[word] = Shader::UNKNOWN_LOCATION;
        Require(resource.GetSharp(info) == AmdGpu::Buffer::Null(),
                "unknown descriptor provenance was accepted");
    }
}

void TestPostOperations() {
    FixtureInfo info{};
    auto expected = MakeBuffer();
    auto resource = ImmediateResource(expected);
    resource.post_op = Shader::SharpFetchPostOp::BitwiseOrDw1WithImm;
    resource.post_op_dw1_mask = 0x00400000;
    expected.stride |= 0x40;
    Require(resource.GetSharp(info) == expected, "stride post operation lost descriptor bits");

    expected = MakeBuffer();
    expected.base_address = 0xfffffffff0;
    resource = ImmediateResource(expected);
    resource.post_op = Shader::SharpFetchPostOp::OffsetByProgramBase;
    info.pgm_base = 0x40;
    expected.base_address = 0x30;
    Require(resource.GetSharp(info) == expected,
            "program relative address did not preserve the 40-bit descriptor address");
}

void TestDescriptorValidation() {
    FixtureInfo info{};
    auto buffer = MakeBuffer();
    buffer.type = 1;
    Require(ImmediateResource(buffer).GetSharp(info) == AmdGpu::Buffer::Null(),
            "non-buffer descriptor type accepted");

    buffer = MakeBuffer();
    buffer.data_format = 15;
    auto result = ImmediateResource(buffer).GetSharp(info);
    Require(result.data_format == 0 && result.num_format == 0,
            "reserved format was not canonicalized");
    for (u32 format = 0; format != 15; ++format) {
        buffer = MakeBuffer();
        buffer.data_format = format;
        buffer.num_format = 6;
        const bool supported = format == 1 || format == 2 || format == 3 || format == 5 ||
                               format == 10 || format == 12;
        result = ImmediateResource(buffer).GetSharp(info);
        Require(result.data_format == (supported ? format : 0) &&
                    result.num_format == (supported ? 6 : 0),
                "number/data format compatibility differs");
    }
    buffer = MakeBuffer();
    buffer.dst_sel_x = 2;
    buffer.dst_sel_y = 3;
    result = ImmediateResource(buffer).GetSharp(info);
    Require(result.dst_sel_x == 0 && result.dst_sel_y == 0 && result.dst_sel_z == 6 &&
                result.dst_sel_w == 7,
            "reserved component selectors were not canonicalized independently");
}

static_assert(sizeof(Shader::BufferResource) == 44);
static_assert(offsetof(Shader::BufferResource, sharp_fetch) == 0);
static_assert(offsetof(Shader::BufferResource, buffer_type) == 32);
static_assert(offsetof(Shader::BufferResource, post_op) == 36);
static_assert(offsetof(Shader::BufferResource, post_op_dw1_mask) == 40);

} // namespace

int main() {
    try {
        TestIndependentWords();
        TestPostOperations();
        TestDescriptorValidation();
        std::cout << "sharp fetch mixed-source, post-op and descriptor validation PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
