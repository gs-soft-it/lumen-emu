// SPDX-FileCopyrightText: Copyright 2026 Lumen Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <thread>
#include "core/libraries/np/np_lookup.h"
#include "core/libraries/np/np_lookup_state.h"

using namespace Libraries::Np;
using namespace Libraries::Np::NpLookup;

static void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

int main() {
    try {
        auto& state = Detail::LookupState();
        state.Reset();
        Check(sceNpLookupCreateTitleCtx(nullptr) == ORBIS_NP_COMMUNITY_ERROR_INSUFFICIENT_ARGUMENT,
              "null title argument");
        Check(sceNpLookupCreateRequest(1) == ORBIS_NP_COMMUNITY_ERROR_INVALID_ID,
              "request without title");
        OrbisNpId identity{};
        std::memcpy(identity.handle.data, "Lumen", 5);
        const auto title = sceNpLookupCreateTitleCtx(&identity);
        Check(title == 1, "initial title ID");
        identity.handle.data[0] = 'X';
        Check(state.ReadTitle(title)->handle.data[0] == 'L', "owned identity snapshot");
        std::array<s32, 32> request_ids{};
        for (auto& id : request_ids) {
            id = sceNpLookupCreateRequest(title);
            Check(id > 0, "request allocation");
        }
        Check(sceNpLookupCreateRequest(title) == ORBIS_NP_COMMUNITY_ERROR_TOO_MANY_OBJECTS,
              "request capacity");
        auto retained = state.FindRequest(request_ids[0]);
        Check(retained && !retained->result, "request pending");
        Check(sceNpLookupDeleteRequest(request_ids[0]) == ORBIS_OK, "request deletion");
        Check(retained->aborted && retained->result == ORBIS_NP_COMMUNITY_ERROR_ABORTED,
              "retained request cancelled");
        Check(sceNpLookupDeleteRequest(request_ids[0]) == ORBIS_NP_COMMUNITY_ERROR_INVALID_ID,
              "stale request ID");
        Check(sceNpLookupCreateRequest(title) == 33, "request ID does not reuse freed slot");
        auto child = state.FindRequest(request_ids[1]);
        Check(sceNpLookupDeleteTitleCtx(title) == ORBIS_OK, "title deletion");
        Check(child->aborted && child->result == ORBIS_NP_COMMUNITY_ERROR_ABORTED,
              "title deletion cancels retained child");
        Check(!state.FindRequest(request_ids[1]), "child handle removed");
        Check(sceNpLookupCreateRequest(title) == ORBIS_NP_COMMUNITY_ERROR_INVALID_ID,
              "deleted title rejected");
        Check(sceNpLookupDeleteTitleCtx(title) == ORBIS_NP_COMMUNITY_ERROR_INVALID_ID,
              "stale title rejected");

        state.Reset();
        std::array<s32, 48> ids{};
        std::array<std::thread, 48> workers;
        for (std::size_t i = 0; i < workers.size(); ++i) {
            workers[i] = std::thread([&, i] { ids[i] = sceNpLookupCreateTitleCtx(&identity); });
        }
        for (auto& worker : workers) worker.join();
        std::sort(ids.begin(), ids.end());
        Check(std::count(ids.begin(), ids.end(), ORBIS_NP_COMMUNITY_ERROR_TOO_MANY_OBJECTS) == 16,
              "concurrent capacity");
        for (std::size_t i = 16; i < ids.size(); ++i) {
            Check(ids[i] == static_cast<s32>(i - 15), "concurrent IDs unique and sequential");
        }
        Check(sceNpLookupDeleteTitleCtx(8) == ORBIS_OK, "free concurrent title");
        Check(sceNpLookupCreateTitleCtx(&identity) == 33, "title ID does not reuse freed slot");
        state.Reset();
        Check(sceNpLookupCreateTitleCtx(&identity) == 1, "new session reset");
        state.Reset();
        std::puts("NP lookup lifecycle contract passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
