// SPDX-License-Identifier: GPL-2.0-only
// Original PS4/FEX guest-memory adapter around the shared Gen5 IME host model.
// Imported model provenance and license: source/gen4/gen5/UPSTREAM.json and LICENSE.
#include "Emulator/Libs/ImeDialog.h"
#include "Emulator/Loader/RuntimeLinker.h"
#include "Lumen/Core/VirtualMemory.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#ifdef LUMEN_EMU_ENABLED
namespace Lumen::Libs::Dialog::ImeDialog {
namespace {
namespace Host = ::Libs::Dialog::ImeDialog;
namespace Vm = Core::VirtualMemory;

constexpr int32_t ERROR_BUSY = static_cast<int32_t>(0x80bc0001);
constexpr int32_t ERROR_INVALID_MAX_TEXT_LENGTH = static_cast<int32_t>(0x80bc0016);
constexpr int32_t ERROR_INVALID_TEXT_BUFFER = static_cast<int32_t>(0x80bc0017);
constexpr int32_t ERROR_INVALID_EXTENDED = static_cast<int32_t>(0x80bc001c);
constexpr int32_t ERROR_INVALID_PARAM = static_cast<int32_t>(0x80bc0030);
constexpr int32_t ERROR_INVALID_ADDRESS = static_cast<int32_t>(0x80bc0031);
constexpr int32_t ERROR_INVALID_RESERVED = static_cast<int32_t>(0x80bc0032);
constexpr int32_t ERROR_INVALID_TITLE = static_cast<int32_t>(0x80bc0101);
constexpr int32_t ERROR_NOT_FINISHED = static_cast<int32_t>(0x80bc0106);
constexpr int32_t ERROR_NOT_IN_USE = static_cast<int32_t>(0x80bc0107);

struct Session {
    uint64_t address = 0, bytes = 0, original_generation = UINT64_MAX;
    bool published = false, revoked = false;
    std::vector<char16_t> shadow;
    std::u16string title, placeholder;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
    Vm::GuestDataRangeSeal input_seal;
#endif
};

std::mutex g_bridge_mutex;
std::unique_ptr<Session> g_session;
uint32_t g_diagnostic_records = 0;
Status g_reported_status = Status::None;

void Report(Status status, const char* reason)
{
    if (status == g_reported_status && reason == nullptr) return;
    g_reported_status = status;
    if (g_diagnostic_records++ >= 32) return;
    std::fprintf(stderr, "[IME_DIALOG_PS4] status=%u reason=%s host_text=owned guest_callbacks=0\n",
        static_cast<uint32_t>(status), reason != nullptr ? reason : "transition");
}

bool Range(uint64_t address, uint64_t bytes)
{
    return address != 0 && bytes != 0 && address <= UINTPTR_MAX && bytes <= UINTPTR_MAX - address;
}

bool AllZero(const int8_t* bytes, size_t size)
{
    return std::all_of(bytes, bytes + size, [](int8_t value) { return value == 0; });
}

bool Read(uint64_t address, void* output, size_t bytes)
{
    if (!Range(address, bytes) || output == nullptr) return false;
#if defined(__aarch64__) || (LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64)
    return Loader::ReadGuestMemory(address, output, bytes);
#else
    return false;
#endif
}

// Every output is acquired before touching model state or any other output.
// Mac and Android retain real readers through the complete copy. Windows has
// the existing validated native policy, which does not pin concurrent unmaps.
class GuestOutput {
public:
    bool Acquire(uint64_t address, size_t bytes, bool readable = false)
    {
        if (!Range(address, bytes) || address % alignof(uint32_t) != 0) return false;
        m_address = address; m_bytes = bytes;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
        if (!Vm::TryAcquireGuestDataLease(address, bytes, readable ? Vm::Mode::ReadWrite : Vm::Mode::Write,
                &m_lease, false)) return false;
        m_host = m_lease.HostBegin();
        return m_lease.IsActive() && m_host == address && m_lease.ByteCount() == bytes;
#elif LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64
        return Loader::ValidateGuestMemory(address, bytes, true) &&
            (!readable || Loader::ValidateGuestMemory(address, bytes, false));
#else
        return false;
#endif
    }
    bool ReadLocal(void* output) const
    {
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
        if (!m_lease.IsActive() || output == nullptr) return false;
        std::memcpy(output, reinterpret_cast<const void*>(m_host), m_bytes);
        return true;
#else
        return Read(m_address, output, m_bytes);
#endif
    }
    bool WriteLocal(const void* input) const
    {
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
        if (!m_lease.IsActive() || input == nullptr) return false;
        std::memcpy(reinterpret_cast<void*>(m_host), input, m_bytes);
        return true;
#elif LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64
        return Loader::WriteGuestMemory(m_address, input, m_bytes);
#else
        return false;
#endif
    }
private:
    uint64_t m_address = 0, m_host = 0;
    size_t m_bytes = 0;
    Vm::GuestDataLease m_lease;
};

bool ManagedWritableRange(uint64_t address, uint64_t bytes)
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
    address &= UINT64_C(0x00ffffffffffffff);
#endif
    if (!Range(address, bytes)) return false;
    const uint64_t end = address + bytes;
    while (address < end)
    {
        Vm::ManagedRegion region {};
        if (Vm::QueryManagedRegion(address, &region) != Vm::ManagedRegionState::Visible ||
            region.begin > address || region.end <= address || Vm::IsExecute(region.mode) ||
            (static_cast<uint32_t>(region.mode) & static_cast<uint32_t>(Vm::Mode::ReadWrite)) !=
                static_cast<uint32_t>(Vm::Mode::ReadWrite)) return false;
        address = std::min(end, region.end);
    }
    return true;
#else
    (void)address; (void)bytes;
    return true;
#endif
}

enum class InputAccess { Ready, Unavailable, Revoked };

InputAccess BorrowInput(Session& session, Vm::GuestDataLease* lease, bool original)
{
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
    Vm::GuestDataLeaseDenialMetadata denial;
    bool acquired = false;
    for (unsigned attempt = 0; attempt != 16; ++attempt)
    {
        acquired = Vm::TryAcquireGuestDataLease(session.address, session.bytes, Vm::Mode::ReadWrite, lease, false, &denial);
        if (acquired || denial.reason != Vm::GuestDataLeaseDenialReason::GateTrylockUnavailable) break;
    }
    if (!acquired)
    {
        if (denial.reason == Vm::GuestDataLeaseDenialReason::LogicalSnapshotDenied ||
            denial.reason == Vm::GuestDataLeaseDenialReason::LogicalRegistryDenied ||
            denial.reason == Vm::GuestDataLeaseDenialReason::PhysicalQueryOrRightsDenied)
            return InputAccess::Revoked;
        // An unknown/closed gate is not evidence of revoked mapping rights.
        return InputAccess::Unavailable;
    }
    if (!lease->IsActive() || lease->HostBegin() != session.address || lease->ByteCount() != session.bytes ||
        lease->MutationGeneration() == UINT64_MAX || !ManagedWritableRange(session.address, session.bytes))
        return InputAccess::Revoked;
    if (original)
    {
        session.original_generation = lease->MutationGeneration();
#if LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
        if (!Vm::CaptureGuestDataRangeSeal(*lease, &session.input_seal)) return InputAccess::Unavailable;
#endif
        return InputAccess::Ready;
    }
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX
    return Vm::ValidateGuestDataLeaseRangeContinuity(*lease, session.address, session.bytes, session.original_generation)
        ? InputAccess::Ready : InputAccess::Revoked;
#else
    // Android retains its counted HLE reader while checking the original
    // sealed range. Every overlapping VM mutation revokes that seal before
    // touching memory, including operations which fail and restore rights.
    return Vm::ValidateGuestDataRangeSeal(*lease, session.input_seal)
        ? InputAccess::Ready : InputAccess::Revoked;
#endif
#elif LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64
    (void)lease;
    if (!Loader::ValidateGuestMemory(session.address, session.bytes, true) ||
        !Loader::ValidateGuestMemory(session.address, session.bytes, false)) return InputAccess::Revoked;
    if (original) session.original_generation = Vm::GetMutationGeneration();
    return InputAccess::Ready;
#else
    (void)session; (void)lease; (void)original;
    return InputAccess::Unavailable;
#endif
}

int32_t CopyParam(const Param* guest, const ExtendedParam* extended_guest, Param* param, ExtendedParam* extended)
{
    if (reinterpret_cast<uint64_t>(guest) % alignof(Param) != 0 ||
        (extended_guest != nullptr && reinterpret_cast<uint64_t>(extended_guest) % alignof(ExtendedParam) != 0))
        return ERROR_INVALID_ADDRESS;
    if (!Read(reinterpret_cast<uint64_t>(guest), param, sizeof(*param))) return ERROR_INVALID_ADDRESS;
    *extended = {};
    if (extended_guest != nullptr && !Read(reinterpret_cast<uint64_t>(extended_guest), extended, sizeof(*extended)))
        return ERROR_INVALID_ADDRESS;
    if (!AllZero(param->reserved, sizeof(param->reserved))) return ERROR_INVALID_RESERVED;
    // These are x86 guest callbacks and guest filesystem pointers. They must
    // never enter a host function pointer or native dictionary API.
    if (param->filter != nullptr) return ERROR_INVALID_PARAM;
    if (extended->ext_keyboard_filter != nullptr || extended->additional_dictionary_path != nullptr)
        return ERROR_INVALID_EXTENDED;
    // SDL2 text events do not identify physical versus software keyboard
    // sources. A request to exclude the former cannot be honored by this UI.
    if ((extended->disable_device & Host::DISABLE_DEVICE_EXT_KEYBOARD) != 0)
        return ERROR_INVALID_EXTENDED;
    return 0;
}

bool ReadText(uint64_t address, uint32_t limit, std::u16string* text)
{
    text->clear();
    if (address == 0) return true;
    if (address % sizeof(char16_t) != 0) return false;
    for (uint32_t index = 0; index <= limit; ++index)
    {
        char16_t value = 0;
        const uint64_t offset = uint64_t(index) * sizeof(char16_t);
        if (address > UINT64_MAX - offset || !Read(address + offset, &value, sizeof(value))) return false;
        if (value == 0) return ::Libs::ImeCommon::IsValidUtf16(*text);
        if (index == limit) return false;
        text->push_back(value);
    }
    return false;
}

void SanitizeUnusedPointers(Param* param, ExtendedParam* extended)
{
    param->input_text_buffer = nullptr; param->title = nullptr; param->placeholder = nullptr; param->filter = nullptr;
    extended->additional_dictionary_path = nullptr; extended->ext_keyboard_filter = nullptr;
}

InputAccess PublishFinished()
{
    if (g_session == nullptr) return InputAccess::Revoked;
    auto& session = *g_session;
    if (session.revoked) return InputAccess::Revoked;
    if (session.published) return InputAccess::Ready;
    Vm::GuestDataLease lease;
    const auto access = BorrowInput(session, &lease, false);
    if (access == InputAccess::Revoked)
    {
        session.revoked = true;
        if (Host::ImeDialogGetStatus() == static_cast<int32_t>(Status::Running)) Host::ImeDialogAbort();
        Report(Status::Finished, "destination-revoked");
        return access;
    }
    if (access != InputAccess::Ready) return access;
    size_t units = 0;
    while (units < session.shadow.size() && session.shadow[units] != 0) ++units;
    if (units == session.shadow.size())
    {
        session.revoked = true;
        return InputAccess::Revoked;
    }
    const size_t bytes = (units + 1) * sizeof(char16_t);
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
    std::memcpy(reinterpret_cast<void*>(lease.HostBegin()), session.shadow.data(), bytes);
#elif LUMEN_PLATFORM == LUMEN_PLATFORM_WINDOWS && LUMEN_ABI == LUMEN_ABI_X86_64
    if (!Loader::WriteGuestMemory(session.address, session.shadow.data(), bytes)) return InputAccess::Unavailable;
#else
    return InputAccess::Unavailable;
#endif
    session.published = true;
    return InputAccess::Ready;
}

int32_t GetStatusLocked()
{
    const auto status = static_cast<Status>(Host::ImeDialogGetStatus());
    if (status == Status::Finished)
    {
        const auto published = PublishFinished();
        if (published == InputAccess::Unavailable) return static_cast<int32_t>(Status::Running);
    }
    Report(status, nullptr);
    return static_cast<int32_t>(status);
}

} // namespace

int32_t LUMEN_SYSV_ABI ImeDialogInit(const Param* param_guest, const ExtendedParam* extended_guest)
{
    std::lock_guard<std::mutex> lock(g_bridge_mutex);
    if (g_session != nullptr) return ERROR_BUSY;
    Param param {};
    ExtendedParam extended {};
    const int32_t copied = CopyParam(param_guest, extended_guest, &param, &extended);
    if (copied != 0) return copied;
    if (param.max_text_length == 0 || param.max_text_length > IME_DIALOG_MAX_TEXT_LENGTH)
        return ERROR_INVALID_MAX_TEXT_LENGTH;
    auto session = std::make_unique<Session>();
    session->address = reinterpret_cast<uint64_t>(param.input_text_buffer);
    session->bytes = (uint64_t(param.max_text_length) + 1) * sizeof(char16_t);
    if (!Range(session->address, session->bytes) || session->address % sizeof(char16_t) != 0)
        return ERROR_INVALID_TEXT_BUFFER;
    session->shadow.resize(param.max_text_length + 1);
    Vm::GuestDataLease lease;
    if (BorrowInput(*session, &lease, true) != InputAccess::Ready) return ERROR_INVALID_TEXT_BUFFER;
#if LUMEN_PLATFORM == LUMEN_PLATFORM_OSX || LUMEN_PLATFORM == LUMEN_PLATFORM_ANDROID
    std::memcpy(session->shadow.data(), reinterpret_cast<const void*>(lease.HostBegin()), session->bytes);
#else
    if (!Read(session->address, session->shadow.data(), session->bytes)) return ERROR_INVALID_TEXT_BUFFER;
#endif
    const bool title_present = param.title != nullptr, placeholder_present = param.placeholder != nullptr;
    if (!ReadText(reinterpret_cast<uint64_t>(param.title), Host::IME_DIALOG_MAX_TITLE_LENGTH, &session->title))
        return ERROR_INVALID_TITLE;
    if (!ReadText(reinterpret_cast<uint64_t>(param.placeholder), Host::IME_DIALOG_MAX_PLACEHOLDER_LENGTH, &session->placeholder))
        return ERROR_INVALID_PARAM;
    param.input_text_buffer = session->shadow.data();
    param.title = title_present ? session->title.c_str() : nullptr;
    param.placeholder = placeholder_present ? session->placeholder.c_str() : nullptr;
    param.filter = nullptr;
    extended.additional_dictionary_path = nullptr; extended.ext_keyboard_filter = nullptr;
    const int32_t result = Host::ImeDialogInit(&param, extended_guest != nullptr ? &extended : nullptr);
    if (result != 0) return result;
    g_session = std::move(session);
    Report(Status::Running, "initialized");
    return 0;
}

int32_t LUMEN_SYSV_ABI ImeDialogGetStatus()
{
    std::lock_guard<std::mutex> lock(g_bridge_mutex);
    return GetStatusLocked();
}

int32_t LUMEN_SYSV_ABI ImeDialogAbort()
{
    std::lock_guard<std::mutex> lock(g_bridge_mutex);
    const int32_t result = Host::ImeDialogAbort();
    if (result == 0) GetStatusLocked();
    return result;
}

int32_t LUMEN_SYSV_ABI ImeDialogGetResult(Result* guest)
{
    std::lock_guard<std::mutex> lock(g_bridge_mutex);
    if (g_session == nullptr) return ERROR_NOT_IN_USE;
    GuestOutput output;
    Result result {};
    if (!output.Acquire(reinterpret_cast<uint64_t>(guest), sizeof(result), true) || !output.ReadLocal(&result))
        return ERROR_INVALID_ADDRESS;
    if (!AllZero(result.reserved, sizeof(result.reserved))) return ERROR_INVALID_RESERVED;
    if (Host::ImeDialogGetStatus() != static_cast<int32_t>(Status::Finished)) return ERROR_NOT_FINISHED;
    const auto published = PublishFinished();
    if (published == InputAccess::Unavailable) return ERROR_INVALID_ADDRESS;
    Result staged {};
    const int32_t result_code = Host::ImeDialogGetResult(&staged);
    if (result_code != 0) return result_code;
    if (g_session->revoked) staged.endstatus = EndStatus::Aborted;
    if (!output.WriteLocal(&staged)) return ERROR_INVALID_ADDRESS;
    Report(Status::Finished, nullptr);
    return 0;
}

int32_t LUMEN_SYSV_ABI ImeDialogTerm()
{
    std::lock_guard<std::mutex> lock(g_bridge_mutex);
    if (g_session == nullptr) return ERROR_NOT_IN_USE;
    if (Host::ImeDialogGetStatus() != static_cast<int32_t>(Status::Finished)) return ERROR_NOT_FINISHED;
    if (PublishFinished() == InputAccess::Unavailable) return ERROR_INVALID_ADDRESS;
    const int32_t result = Host::ImeDialogTerm();
    if (result == 0) { g_session.reset(); Report(Status::None, nullptr); }
    return result;
}

int32_t LUMEN_SYSV_ABI ImeDialogGetPanelSize(const Param* param, uint32_t* width, uint32_t* height)
{
    return Lumen::Libs::Dialog::ImeDialog::ImeDialogGetPanelSizeExtended(param, nullptr, width, height);
}

int32_t LUMEN_SYSV_ABI ImeDialogGetPanelSizeExtended(const Param* guest, const ExtendedParam* guest_extended,
                                                 uint32_t* width_guest, uint32_t* height_guest)
{
    std::lock_guard<std::mutex> lock(g_bridge_mutex);
    Param param {};
    ExtendedParam extended {};
    const int32_t copied = CopyParam(guest, guest_extended, &param, &extended);
    if (copied != 0) return copied;
    const uint64_t width_address = reinterpret_cast<uint64_t>(width_guest), height_address = reinterpret_cast<uint64_t>(height_guest);
    if (!Range(width_address, sizeof(uint32_t)) || !Range(height_address, sizeof(uint32_t)) ||
        (width_address < height_address + sizeof(uint32_t) && height_address < width_address + sizeof(uint32_t)))
        return ERROR_INVALID_ADDRESS;
    GuestOutput width_output, height_output;
    if (!width_output.Acquire(width_address, sizeof(uint32_t)) || !height_output.Acquire(height_address, sizeof(uint32_t)))
        return ERROR_INVALID_ADDRESS;
    SanitizeUnusedPointers(&param, &extended);
    uint32_t width = 0, height = 0;
    const int32_t result = Host::ImeDialogGetPanelSizeExtended(&param, guest_extended != nullptr ? &extended : nullptr, &width, &height);
    if (result != 0) return result;
    return width_output.WriteLocal(&width) && height_output.WriteLocal(&height) ? 0 : ERROR_INVALID_ADDRESS;
}

int32_t LUMEN_SYSV_ABI ImeDialogGetPanelPositionAndForm(PositionAndForm* guest)
{
    std::lock_guard<std::mutex> lock(g_bridge_mutex);
    if (g_session == nullptr) return ERROR_NOT_IN_USE;
    GuestOutput output;
    if (!output.Acquire(reinterpret_cast<uint64_t>(guest), sizeof(PositionAndForm))) return ERROR_INVALID_ADDRESS;
    PositionAndForm form {};
    const int32_t result = Host::ImeDialogGetPanelPositionAndForm(&form);
    if (result != 0) return result;
    return output.WriteLocal(&form) ? 0 : ERROR_INVALID_ADDRESS;
}

VisualState GetVisualState() noexcept
{
    std::lock_guard<std::mutex> lock(g_bridge_mutex);
    return Host::GetVisualState();
}
void SetVisibilityCallback(VisibilityCallback callback) noexcept
{
    std::lock_guard<std::mutex> lock(g_bridge_mutex);
    Host::SetVisibilityCallback(callback);
}
bool GetHostSnapshot(HostSnapshot* snapshot)
{
    std::lock_guard<std::mutex> lock(g_bridge_mutex);
    return g_session != nullptr && !g_session->revoked && Host::GetHostSnapshot(snapshot);
}
bool HostInsertText(uint64_t generation, std::u16string_view text)
{
    std::lock_guard<std::mutex> lock(g_bridge_mutex);
    return g_session != nullptr && !g_session->revoked && Host::HostInsertText(generation, text);
}
bool HostReplaceText(uint64_t generation, std::u16string_view text, uint32_t cursor)
{
    std::lock_guard<std::mutex> lock(g_bridge_mutex);
    return g_session != nullptr && !g_session->revoked && Host::HostReplaceText(generation, text, cursor);
}
bool HostBackspace(uint64_t generation)
{
    std::lock_guard<std::mutex> lock(g_bridge_mutex);
    return g_session != nullptr && !g_session->revoked && Host::HostBackspace(generation);
}
bool HostMoveCursor(uint64_t generation, int delta)
{
    std::lock_guard<std::mutex> lock(g_bridge_mutex);
    return g_session != nullptr && !g_session->revoked && Host::HostMoveCursor(generation, delta);
}
bool HostAccept(uint64_t generation)
{
    std::lock_guard<std::mutex> lock(g_bridge_mutex);
    return g_session != nullptr && !g_session->revoked && Host::HostAccept(generation);
}
bool HostCancel(uint64_t generation)
{
    std::lock_guard<std::mutex> lock(g_bridge_mutex);
    return g_session != nullptr && !g_session->revoked && Host::HostCancel(generation);
}
void ResetSessionState()
{
    std::lock_guard<std::mutex> lock(g_bridge_mutex);
    if (Host::ImeDialogGetStatus() == static_cast<int32_t>(Status::Running)) Host::ImeDialogAbort();
    if (Host::ImeDialogGetStatus() == static_cast<int32_t>(Status::Finished)) Host::ImeDialogTerm();
    g_session.reset();
    g_diagnostic_records = 0; g_reported_status = Status::None;
}

} // namespace Lumen::Libs::Dialog::ImeDialog
#endif
