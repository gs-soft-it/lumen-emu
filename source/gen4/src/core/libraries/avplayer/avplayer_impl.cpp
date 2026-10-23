// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstring>

#include "common/logging/log.h"
#include "core/libraries/avplayer/avplayer_common.h"
#include "core/libraries/avplayer/avplayer_error.h"
#include "core/libraries/avplayer/avplayer_file_io.h"
#include "core/libraries/avplayer/avplayer_impl.h"
#include "core/libraries/avplayer/avplayer_path.h"
#include "core/libraries/avplayer/avplayer_read.h"
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
#include "core/guest_cpu/guest_callback.h"
#include "core/guest_cpu/guest_callback_scratch.h"
#endif

namespace Libraries::AvPlayer {

#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
template <typename Callback>
const void* CallbackAddress(Callback callback) {
    return reinterpret_cast<const void*>(callback);
}

#endif

void* PS4_SYSV_ABI AvPlayer::Allocate(void* handle, u32 alignment, u32 size) {
    const auto* const self = reinterpret_cast<AvPlayer*>(handle);
    const auto allocate = self->m_init_data_original.memory_replacement.allocate;
    const auto ptr = self->m_init_data_original.memory_replacement.object_ptr;
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
    return reinterpret_cast<void*>(Core::GuestCpu::RunGuestFunctionOrAbort(
        CallbackAddress(allocate), "AvPlayer allocate", ptr, alignment, size));
#else
    return allocate(ptr, alignment, size);
#endif
}

void PS4_SYSV_ABI AvPlayer::Deallocate(void* handle, void* memory) {
    const auto* const self = reinterpret_cast<AvPlayer*>(handle);
    const auto deallocate = self->m_init_data_original.memory_replacement.deallocate;
    const auto ptr = self->m_init_data_original.memory_replacement.object_ptr;
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
    Core::GuestCpu::RunGuestFunctionOrAbort(CallbackAddress(deallocate),
                                            "AvPlayer deallocate", ptr, memory);
#else
    deallocate(ptr, memory);
#endif
}

void* PS4_SYSV_ABI AvPlayer::AllocateTexture(void* handle, u32 alignment, u32 size) {
    const auto* const self = reinterpret_cast<AvPlayer*>(handle);
    const auto allocate = self->m_init_data_original.memory_replacement.allocate_texture;
    const auto ptr = self->m_init_data_original.memory_replacement.object_ptr;
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
    return reinterpret_cast<void*>(Core::GuestCpu::RunGuestFunctionOrAbort(
        CallbackAddress(allocate), "AvPlayer allocate texture", ptr, alignment, size));
#else
    return allocate(ptr, alignment, size);
#endif
}

void PS4_SYSV_ABI AvPlayer::DeallocateTexture(void* handle, void* memory) {
    const auto* const self = reinterpret_cast<AvPlayer*>(handle);
    const auto deallocate = self->m_init_data_original.memory_replacement.deallocate_texture;
    const auto ptr = self->m_init_data_original.memory_replacement.object_ptr;
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
    Core::GuestCpu::RunGuestFunctionOrAbort(
        CallbackAddress(deallocate), "AvPlayer deallocate texture", ptr, memory);
#else
    deallocate(ptr, memory);
#endif
}

int PS4_SYSV_ABI AvPlayer::OpenFile(void* handle, const char* filename) {
    auto const self = reinterpret_cast<AvPlayer*>(handle);
    AvPlayerFileOperation operation(self->m_file_operation);
    if (!operation) return -1;
    self->m_file_cache.clear();
    self->m_file_cache_ready = false;

    const auto open = self->m_init_data_original.file_replacement.open;
    const auto ptr = self->m_init_data_original.file_replacement.object_ptr;
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
    const auto normalized = AvPlayerNormalizeGuestPath(filename ? filename : "");
    const auto name_len = static_cast<u32>(normalized.size() + 1);
    void* guest_name = Allocate(handle, 1, name_len);
    if (guest_name == nullptr) {
        return -1;
    }
    bool copied = false;
    {
        Core::Fex::MemoryMutationScope publication;
        if (publication.Valid() &&
            Core::GuestCpu::IsMappedCallbackMemory(guest_name, name_len, true)) {
            std::memcpy(guest_name, normalized.c_str(), name_len);
            publication.Publish(reinterpret_cast<VAddr>(guest_name), name_len);
            copied = true;
        }
    }
    if (!copied) {
        Deallocate(handle, guest_name);
        return -1;
    }
    const auto fd = static_cast<int>(Core::GuestCpu::RunGuestFunctionOrAbort(
        CallbackAddress(open), "AvPlayer open", ptr, guest_name));
    Deallocate(handle, guest_name);
    return fd;
#else
    const auto normalized = AvPlayerNormalizeGuestPath(filename ? filename : "");
    return open(ptr, normalized.c_str());
#endif
}

int PS4_SYSV_ABI AvPlayer::CloseFile(void* handle) {
    auto const self = reinterpret_cast<AvPlayer*>(handle);
    AvPlayerFileOperation operation(self->m_file_operation);
    if (!operation) return -1;
    self->m_file_cache.clear();
    self->m_file_cache_ready = false;

    const auto close = self->m_init_data_original.file_replacement.close;
    const auto ptr = self->m_init_data_original.file_replacement.object_ptr;
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
    return static_cast<int>(Core::GuestCpu::RunGuestFunctionOrAbort(
        CallbackAddress(close), "AvPlayer close", ptr));
#else
    return close(ptr);
#endif
}

bool AvPlayer::EnsureFileCache() {
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
    if (m_file_cache_ready) {
        return true;
    }
    const auto read_offset = m_init_data_original.file_replacement.read_offset;
    const auto size_fn = m_init_data_original.file_replacement.size;
    const auto ptr = m_init_data_original.file_replacement.object_ptr;
    u64 expected = 0;
    if (size_fn != nullptr) {
        expected = Core::GuestCpu::RunGuestFunctionOrAbort(CallbackAddress(size_fn),
                                                          "AvPlayer size", ptr);
    }
    if (expected > INT64_MAX) return false;
    auto* const bounce = Allocate(this, 16, kAvPlayerGuestReadChunkMax);
    if (bounce == nullptr) {
        LOG_ERROR(Lib_AvPlayer, "AvPlayer preload bounce allocate failed");
        return false;
    }
    const auto release = [this](void* memory) { Deallocate(this, memory); };
    const std::unique_ptr<void, decltype(release)> owned_bounce(bounce, release);
    m_file_cache_ready = AvPlayerPreloadFile(expected,
        [&](u64 position, u32 length) {
            return static_cast<s32>(Core::GuestCpu::RunGuestFunctionOrAbort(
                CallbackAddress(read_offset), "AvPlayer read_offset", ptr, bounce, position, length));
        },
        [&](u8* output, u32 length) {
            Core::Fex::MemoryMutationScope publication;
            if (!publication.Valid() ||
                !Core::GuestCpu::IsMappedCallbackMemory(bounce, length, false)) return false;
            std::memcpy(output, bounce, length);
            return true;
        }, m_file_cache);
    if (!m_file_cache_ready) {
        LOG_ERROR(Lib_AvPlayer, "AvPlayer file preload failed (size_cb={})", expected);
        return false;
    }
    LOG_INFO(Lib_AvPlayer, "AvPlayer preloaded {} bytes (size_cb={})", m_file_cache.size(),
             expected);
    return true;
#else
    m_file_cache_ready = true;
    return true;
#endif
}

int PS4_SYSV_ABI AvPlayer::ReadOffsetFile(void* handle, u8* buffer, u64 position, u32 length) {
    auto const self = reinterpret_cast<AvPlayer*>(handle);
    AvPlayerFileOperation operation(self->m_file_operation);
    if (!operation) return -1;

    const auto read_offset = self->m_init_data_original.file_replacement.read_offset;
    const auto ptr = self->m_init_data_original.file_replacement.object_ptr;
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
    if (!self->EnsureFileCache()) {
        return -1;
    }
    return static_cast<int>(
        AvPlayerCopyFromCache(self->m_file_cache.data(), self->m_file_cache.size(), position,
                              buffer, length));
#else
    return read_offset(ptr, buffer, position, length);
#endif
}

u64 PS4_SYSV_ABI AvPlayer::SizeFile(void* handle) {
    auto const self = reinterpret_cast<AvPlayer*>(handle);
    AvPlayerFileOperation operation(self->m_file_operation);
    if (!operation) return UINT64_MAX;

    const auto size = self->m_init_data_original.file_replacement.size;
    const auto ptr = self->m_init_data_original.file_replacement.object_ptr;
#ifdef LUMEN_GEN4_ENABLE_FEX_GUEST_CPU
    if (self->EnsureFileCache()) {
        return self->m_file_cache.size();
    }
    return UINT64_MAX;
#else
    return size(ptr);
#endif
}

AvPlayerInitData AvPlayer::StubInitData(const AvPlayerInitData& data) {
    AvPlayerInitData result = data;
    result.memory_replacement.object_ptr = this;
    result.memory_replacement.allocate = &AvPlayer::Allocate;
    result.memory_replacement.deallocate = &AvPlayer::Deallocate;
    result.memory_replacement.allocate_texture = &AvPlayer::AllocateTexture;
    result.memory_replacement.deallocate_texture = &AvPlayer::DeallocateTexture;
    const bool missing_file_callback =
        data.file_replacement.open == nullptr || data.file_replacement.close == nullptr ||
        data.file_replacement.read_offset == nullptr || data.file_replacement.size == nullptr;
    if (missing_file_callback) {
        result.file_replacement = {};
    } else {
        result.file_replacement.object_ptr = this;
        result.file_replacement.open = &AvPlayer::OpenFile;
        result.file_replacement.close = &AvPlayer::CloseFile;
        result.file_replacement.read_offset = &AvPlayer::ReadOffsetFile;
        result.file_replacement.size = &AvPlayer::SizeFile;
    }
    return result;
}

// Only Init/InitEx supply this original table; all of its callbacks belong to the guest.
// StubInitData installs a separate native thunk table for the decoder.
AvPlayer::AvPlayer(const AvPlayerInitData& data)
    : m_init_data(StubInitData(data)), m_init_data_original(data),
      m_state(std::make_unique<AvPlayerState>(m_init_data)) {}

AvPlayer::~AvPlayer() {
    // State owns the decoder workers and file streamer's final close callback.
    // Stop/join them while the callback table, cache and operation gate still live.
    m_state.reset();
    m_file_cache.clear();
    m_file_cache_ready = false;
}

s32 AvPlayer::PostInit(const AvPlayerPostInitData& data) {
    m_state->PostInit(data);
    return ORBIS_OK;
}

s32 AvPlayer::AddSource(std::string_view path) {
    return AddSourceEx(path, AvPlayerSourceType::Unknown);
}

s32 AvPlayer::AddSourceEx(std::string_view path, AvPlayerSourceType source_type) {
    if (source_type == AvPlayerSourceType::Unknown) {
        source_type = GetSourceType(path);
    }
    if (source_type == AvPlayerSourceType::Hls) {
        LOG_ERROR(Lib_AvPlayer, "HTTP Live Streaming is not implemented");
        return ORBIS_AVPLAYER_ERROR_NOT_SUPPORTED;
    }
    if (!m_state->AddSource(path, GetSourceType(path))) {
        return ORBIS_AVPLAYER_ERROR_OPERATION_FAILED;
    }
    return ORBIS_OK;
}

s32 AvPlayer::GetStreamCount() {
    if (m_state == nullptr) {
        return ORBIS_AVPLAYER_ERROR_OPERATION_FAILED;
    }
    const auto res = m_state->GetStreamCount();
    if (AVPLAYER_IS_ERROR(res)) {
        return ORBIS_AVPLAYER_ERROR_OPERATION_FAILED;
    }
    return res;
}

s32 AvPlayer::GetStreamInfo(u32 stream_index, AvPlayerStreamInfo& info) {
    if (!m_state->GetStreamInfo(stream_index, info)) {
        return ORBIS_AVPLAYER_ERROR_OPERATION_FAILED;
    }
    return ORBIS_OK;
}

s32 AvPlayer::EnableStream(u32 stream_index) {
    if (m_state == nullptr) {
        return ORBIS_AVPLAYER_ERROR_OPERATION_FAILED;
    }
    if (!m_state->EnableStream(stream_index)) {
        return ORBIS_AVPLAYER_ERROR_OPERATION_FAILED;
    }
    return ORBIS_OK;
}

s32 AvPlayer::Start() {
    if (m_state == nullptr || !m_state->Start()) {
        return ORBIS_AVPLAYER_ERROR_OPERATION_FAILED;
    }
    return ORBIS_OK;
}

s32 AvPlayer::Pause() {
    if (m_state == nullptr || !m_state->Pause()) {
        return ORBIS_AVPLAYER_ERROR_OPERATION_FAILED;
    }
    return ORBIS_OK;
}

s32 AvPlayer::Resume() {
    if (m_state == nullptr || !m_state->Resume()) {
        return ORBIS_AVPLAYER_ERROR_OPERATION_FAILED;
    }
    return ORBIS_OK;
}

s32 AvPlayer::SetAvSyncMode(AvPlayerAvSyncMode sync_mode) {
    if (m_state == nullptr) {
        return ORBIS_AVPLAYER_ERROR_OPERATION_FAILED;
    }
    m_state->SetAvSyncMode(sync_mode);
    return ORBIS_OK;
}

bool AvPlayer::GetVideoData(AvPlayerFrameInfo& video_info) {
    if (m_state == nullptr) {
        return false;
    }
    return m_state->GetVideoData(video_info);
}

bool AvPlayer::GetVideoData(AvPlayerFrameInfoEx& video_info) {
    if (m_state == nullptr) {
        return false;
    }
    return m_state->GetVideoData(video_info);
}

bool AvPlayer::GetAudioData(AvPlayerFrameInfo& audio_info) {
    if (m_state == nullptr) {
        return false;
    }
    return m_state->GetAudioData(audio_info);
}

bool AvPlayer::IsActive() {
    if (m_state == nullptr) {
        return false;
    }
    return m_state->IsActive();
}

u64 AvPlayer::CurrentTime() {
    if (m_state == nullptr) {
        return 0;
    }
    return m_state->CurrentTime();
}

s32 AvPlayer::Stop() {
    if (m_state == nullptr || !m_state->Stop()) {
        return ORBIS_AVPLAYER_ERROR_OPERATION_FAILED;
    }
    return ORBIS_OK;
}

bool AvPlayer::SetLooping(bool is_looping) {
    if (m_state == nullptr) {
        return false;
    }
    return m_state->SetLooping(is_looping);
}

} // namespace Libraries::AvPlayer
