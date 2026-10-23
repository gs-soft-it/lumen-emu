// SPDX-License-Identifier: GPL-2.0-only
#include "FexTranslationCache.h"
#include "FexBlockConfiguration.h"
#include "AndroidRuntimeReceipt.h"
#include <fstream>

namespace Cache = Lumen::FexCore::TranslationCache;
using Bytes = std::vector<unsigned char>;
#define Check(condition) do { if (!(condition)) { std::fprintf(stderr, "check failed line %d\n", __LINE__); std::abort(); } } while (false)

template <typename T> void Append(Bytes& bytes, T value) {
    const auto size = bytes.size();
    bytes.resize(size + sizeof(value));
    std::memcpy(bytes.data() + size, &value, sizeof(value));
}

std::uint64_t Record(Bytes& output, const std::array<unsigned char, 40>& key, const Bytes& payload) {
    output.insert(output.end(), key.begin(), key.end());
    Append(output, static_cast<std::uint32_t>(payload.size()));
    Append(output, UINT32_C(1));
    Append(output, UINT32_C(0));
    Append(output, static_cast<std::uint32_t>(payload.size()));
    const auto offset = output.size();
    output.insert(output.end(), payload.begin(), payload.end());
    return offset;
}

void Index(Bytes& index, const std::array<unsigned char, 40>& key, std::uint64_t hash,
           std::uint64_t offset, std::uint32_t size) {
    Bytes entry;
    Append(entry, hash);
    Append(entry, size);
    Append(entry, UINT64_C(0));
    Append(entry, offset);
    Record(index, key, entry);
}

void AppendFile(const std::string& path, std::span<const unsigned char> bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::app);
    Check(stream.good());
    stream.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    stream.close();
    Check(stream.good());
}

std::string Current(const std::string& root) {
    Bytes current;
    Check(Cache::ReadFile(root + "/current", current, 80));
    return std::string(current.begin(), current.end());
}

unsigned SnapshotCount(const std::string& root) {
    unsigned count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        if (entry.path().filename().string().starts_with("snapshot-")) ++count;
    }
    return count;
}

int main() {
    using Lumen::FexCore::ParseMaximumBlockInstructions;
    Check(ParseMaximumBlockInstructions(nullptr) == 128);
    Check(ParseMaximumBlockInstructions("5000") == 5000);
    Check(ParseMaximumBlockInstructions("128") == 128);
    for (const char* denied : {"", "0", "1", "127", "129", "256", "500", "5001",
                               "0128", "+128", "-128", "128 ", " 128", "128x", "5000\n"}) {
        Check(ParseMaximumBlockInstructions(denied) == 0);
    }
    using Lumen::FexCore::MaximumBlockInstructionsFromReceipt;
    Check(MaximumBlockInstructionsFromReceipt("ready=1 maxInst=5000 smc=mtrack-rx") == 5000);
    Check(MaximumBlockInstructionsFromReceipt("ready=1 maxInst=128 smc=mtrack-rx") == 128);
    for (const char* denied : {"", "ready=1", "ready=1 maxInst=", "ready=1 maxInst=128",
                               "ready=1 maxInst=5000", "ready=1 maxInst=0128 smc=mtrack-rx",
                               "ready=1 maxInst=128x smc=mtrack-rx", "ready=1 maxInst=1280 smc=mtrack-rx",
                               "ready=1 maxInst=50000 smc=mtrack-rx", "ready=1 maxInst=0 smc=mtrack-rx",
                               "ready=1 maxInst=128 maxInst=5000 smc=mtrack-rx"}) {
        Check(MaximumBlockInstructionsFromReceipt(denied) == 0);
    }
    Bytes data(Cache::Magic.begin(), Cache::Magic.end()), index = data;
    std::array<unsigned char, 40> metadata;
    metadata.fill(0xff);
    const Bytes config(11, 0);
    Index(index, metadata, UINT64_MAX, Record(data, metadata, config), config.size());
    std::array<unsigned char, 40> key {};
    std::fill_n(key.begin(), 16, '0');
    Bytes blob;
    for (const auto value : {1U, 80U, 1U, 1U, 0U, 1U}) Append(blob, value);
    blob.resize(40, 0);
    const unsigned char guest = 0xc3;
    const auto guestHash = XXH3_128bits(&guest, 1);
    std::memcpy(blob.data() + 24, &guestHash, 16);
    Bytes host(80, 0);
    const auto put = [&host]<typename T>(std::size_t offset, T value) { std::memcpy(host.data() + offset, &value, sizeof(value)); };
    put(0, UINT32_C(16)); // inline tail offset
    put(16, UINT64_C(80));
    put(32, UINT64_C(1));
    put(40, UINT32_C(1)); // one RIP entry
    put(44, UINT32_C(48));
    host[52] = 1;
    host[64] = 0x80; host[65] = 1; // host PC +4, guest RIP +0
    blob.insert(blob.end(), host.begin(), host.end());
    Append(blob, UINT64_C(0)); // root page
    Append(blob, UINT64_C(0)); // root guest entry
    Append(blob, UINT32_C(4)); // host entry after inline header
    Append(blob, UINT32_C(24)); // tail RIP relocation
    blob.push_back(2);
    Append(blob, UINT64_C(0));
    blob.push_back(0);
    blob.push_back(0xc3);
    const auto blobOffset = Record(data, key, blob);
    Index(index, key, 0, blobOffset, blob.size());
    Check(Cache::ValidateDatabase(data, index));
    // The shared callback range is code-local; neither an old tail layout nor
    // overflow, alignment or metadata overlap can authorize dynamic RIP use.
    const auto thunkBlob = [&blob](std::uint32_t offset, std::uint32_t size) {
        auto result = blob;
        std::memcpy(result.data() + 40 + 16 + 40, &offset, sizeof(offset));
        std::memcpy(result.data() + 40 + 16 + 44, &size, sizeof(size));
        return result;
    };
    Check(Cache::ValidateBlob(thunkBlob(8, 8)));
    Check(!Cache::ValidateBlob(thunkBlob(4, 4))); // A guest entry may never enter the cold body directly.
    for (const auto range : {std::pair{0U, 4U}, {8U, 0U}, {9U, 4U}, {8U, 2U},
            {16U, 4U}, {12U, 8U}, {8U, UINT32_MAX}, {UINT32_MAX, 4U}})
        Check(!Cache::ValidateBlob(thunkBlob(range.first, range.second)));
    auto oldTail = blob;
    const std::uint32_t obsoleteOffset = 40;
    std::memcpy(oldTail.data() + 40 + 16 + 28, &obsoleteOffset, sizeof(obsoleteOffset));
    Check(!Cache::ValidateBlob(oldTail));
    // The adapter must admit the relocation vocabulary used by the audited
    // handoff overlay before its native loader can rebind those providers.
    const auto namedBlob = [&blob](std::uint32_t symbol) {
        auto result = blob;
        const std::uint32_t twoRelocations = 2;
        std::memcpy(result.data() + 12, &twoRelocations, sizeof(twoRelocations));
        Bytes relocation;
        Append(relocation, UINT32_C(4));
        relocation.push_back(0);
        Append(relocation, symbol);
        Append(relocation, UINT32_C(0));
        relocation.push_back(0);
        result.insert(result.end() - 1, relocation.begin(), relocation.end());
        return result;
    };
    Check(Cache::ValidateBlob(namedBlob(0)));
    for (std::uint32_t symbol = 1; symbol <= 7; ++symbol) {
#if defined(LUMEN_FEXCORE_MEMORY_HANDOFF_INSTRUMENTED) && \
    LUMEN_FEXCORE_MEMORY_HANDOFF_INSTRUMENTED == 1
        Check(Cache::ValidateBlob(namedBlob(symbol)));
#else
        Check(!Cache::ValidateBlob(namedBlob(symbol)));
#endif
    }
    Check(!Cache::ValidateBlob(namedBlob(8)));
    auto bad = blob;
    bad[128] = 1; // missing root guest entry
    Check(!Cache::ValidateBlob(bad));
    bad = blob;
    bad[132] = 0xff; // guest entry beyond bytes
    Check(!Cache::ValidateBlob(bad));
    bad = blob;
    bad[136] = 16; // host entry outside instructions
    Check(!Cache::ValidateBlob(bad));
    bad = data;
    bad.pop_back();
    Check(!Cache::ValidateDatabase(bad, index));
    bad = index;
    bad.back() = 0xff;
    Check(!Cache::ValidateDatabase(data, bad));
    bad = blob;
    bad[12] = 0xff; bad[13] = 0xff; bad[14] = 0xff; bad[15] = 0xff;
    Check(!Cache::ValidateBlob(bad));
    // A one-byte RET at page offset4081 ends before the boundary, while the
    // native decoder's 15-byte lookahead records the next page at relative15.
    // Guest instruction/entry bounds remain one byte despite the extra page.
    auto nearPageEdge = blob;
    const std::uint32_t twoPages = 2;
    std::memcpy(nearPageEdge.data() + 20, &twoPages, sizeof(twoPages));
    const std::uint64_t edgeRip = 4081;
    std::memcpy(nearPageEdge.data() + 40 + 16 + 8, &edgeRip, sizeof(edgeRip));
    const std::int64_t edgeRootPage = -4081;
    std::memcpy(nearPageEdge.data() + 40 + 80, &edgeRootPage, sizeof(edgeRootPage));
    Bytes lookaheadPage;
    Append(lookaheadPage, INT64_C(15));
    nearPageEdge.insert(nearPageEdge.begin() + 40 + 80 + 8, lookaheadPage.begin(), lookaheadPage.end());
    Check(Cache::ValidateBlob(nearPageEdge));
    bad = nearPageEdge;
    const std::int64_t tooFarPage = 4096 + 15;
    std::memcpy(bad.data() + 40 + 80 + 8, &tooFarPage, sizeof(tooFarPage));
    Check(!Cache::ValidateBlob(bad));

    std::string directory = "/private/tmp/lumen-fex-cache-storage-XXXXXX";
    Check(mkdtemp(directory.data()) != nullptr);
    // Pre-capability code, exclusive-publication code and instrumented code
    // must never be loaded from each other's persisted snapshot domain.
    const std::string oldIdentity = "same-build/content/features/counter-frequency";
    auto exclusiveIdentity = oldIdentity, intervalIdentity = oldIdentity;
    Cache::AppendMemoryHandoffDomain(exclusiveIdentity, false);
    Cache::AppendMemoryHandoffDomain(intervalIdentity, true);
    const std::array policyBuckets {Cache::Digest(oldIdentity), Cache::Digest(exclusiveIdentity), Cache::Digest(intervalIdentity)};
    Check(policyBuckets[0] != policyBuckets[1] && policyBuckets[0] != policyBuckets[2] && policyBuckets[1] != policyBuckets[2]);
    for (const auto& policyBucket : policyBuckets) {
        Cache::Session coldPolicy;
        Check(coldPolicy.Start(directory, policyBucket) && !coldPolicy.Loaded());
        Check(Cache::WriteFile(coldPolicy.Path() + "/RWCacheDB.foz", data));
        Check(Cache::WriteFile(coldPolicy.Path() + "/RWCacheDB_idx.foz", index));
        Check(coldPolicy.Seal());
        Cache::Session warmPolicy;
        Check(warmPolicy.Start(directory, policyBucket) && warmPolicy.Loaded());
    }
    const auto privateLeaf = directory + "/new-parent/private-leaf";
    Check(Cache::PrepareDirectory(privateLeaf));
    Check(Cache::PrepareDirectory(privateLeaf + "/"));
    struct stat privateStat {};
    Check(lstat(privateLeaf.c_str(), &privateStat) == 0 &&
          S_ISDIR(privateStat.st_mode) && privateStat.st_uid == geteuid() &&
          (privateStat.st_mode & 0777) == 0700);
    for (const auto mode : {0770, 0702}) {
        Check(chmod(privateLeaf.c_str(), mode) == 0);
        Check(!Cache::PrepareDirectory(privateLeaf));
    }
    Check(chmod(privateLeaf.c_str(), 0700) == 0);
    const auto intermediateLink = directory + "/linked-parent";
    Check(symlink((directory + "/new-parent").c_str(), intermediateLink.c_str()) == 0);
    Check(!Cache::PrepareDirectory(intermediateLink + "/private-leaf"));
    Check(!Cache::PrepareDirectory(intermediateLink + "/must-not-create"));
    Check(!std::filesystem::exists(directory + "/new-parent/must-not-create"));
    const auto fileParent = directory + "/file-parent";
    Check(Cache::WriteFile(fileParent, "regular file"));
    Check(!Cache::PrepareDirectory(fileParent + "/child"));
    Check(!Cache::PrepareDirectory(directory + "/./new-parent"));
    Check(!Cache::PrepareDirectory(directory + "/new-parent/../escape"));
    Check(!Cache::PrepareDirectory(privateLeaf + std::string("\0ignored", 8)));
    Check(!Cache::PrepareDirectory(""));
    const std::string bucket(32, 'a');
    const std::string root = directory + "/" + bucket;
    {
        Cache::Session cold;
        Check(cold.Start(directory, bucket) && !cold.Loaded());
        Check(Cache::WriteFile(cold.Path() + "/RWCacheDB.foz", data));
        Check(Cache::WriteFile(cold.Path() + "/RWCacheDB_idx.foz", index));
        const auto checkpointBusy = [&cold] {
            const auto start = std::chrono::steady_clock::now();
            Check(!cold.Checkpoint());
            Check(std::chrono::steady_clock::now() - start < std::chrono::seconds(1));
        };
        {
            Cache::CacheReadLock nativeWriterData(cold.Path() + "/RWCacheDB.foz", Cache::MaxDataBytes);
            Check(nativeWriterData.Valid());
            checkpointBusy();
        }
        {
            Cache::CacheReadLock nativeWriterIndex(cold.Path() + "/RWCacheDB_idx.foz", Cache::MaxIndexBytes);
            Check(nativeWriterIndex.Valid());
            checkpointBusy();
            Cache::CacheReadLock releasedData(cold.Path() + "/RWCacheDB.foz", Cache::MaxDataBytes);
            Check(releasedData.Valid()); // failed index lock does not retain data lock
        }
        Check(cold.Checkpoint());
        const auto first = Current(root);
        const auto activeSession = cold.Path();
        Check(!activeSession.empty() && SnapshotCount(root) == 1);
        Check(cold.Checkpoint() && Current(root) == first && SnapshotCount(root) == 1);
        {
            Cache::Session liveConsumer;
            Check(liveConsumer.Start(directory, bucket) && liveConsumer.Loaded());
        }
        // A complete data record without its paired index entry must not be
        // published, even though a native reader could ignore the dangling data.
        auto nextData = data, nextIndex = index;
        key[15] = '1';
        const auto nextOffset = Record(nextData, key, blob);
        AppendFile(activeSession + "/RWCacheDB.foz", std::span(nextData).subspan(data.size()));
        Check(!cold.Checkpoint() && Current(root) == first);
        Index(nextIndex, key, 1, nextOffset, blob.size());
        AppendFile(activeSession + "/RWCacheDB_idx.foz", std::span(nextIndex).subspan(index.size()));
        Check(cold.Checkpoint() && Current(root) != first && SnapshotCount(root) == 2);
        Bytes immutableData;
        Check(Cache::ReadFile(root + "/" + first + "/RWCacheDB.foz", immutableData, Cache::MaxDataBytes));
        Check(immutableData == data); // live native writer never mutates a published snapshot
        data = nextData; index = nextIndex;
        key[15] = '2';
        const auto thirdOffset = Record(nextData, key, blob);
        Index(nextIndex, key, 2, thirdOffset, blob.size());
        AppendFile(activeSession + "/RWCacheDB.foz", std::span(nextData).subspan(data.size()));
        AppendFile(activeSession + "/RWCacheDB_idx.foz", std::span(nextIndex).subspan(index.size()));
        Check(cold.Checkpoint() && SnapshotCount(root) == 2);
        Check(!std::filesystem::exists(root + "/" + first));
        Check(cold.Path() == activeSession); // the writer's directory stays private and live
        Check(cold.Seal() && SnapshotCount(root) == 2);
    }
    {
        Cache::Session warm;
        Check(warm.Start(directory, bucket) && warm.Loaded());
    }
    std::vector<unsigned char> current;
    Check(Cache::ReadFile(directory + "/" + bucket + "/current", current, 80));
    const std::string snapshot(current.begin(), current.end());
    const std::string source = directory + "/" + bucket + "/" + snapshot + "/RWCacheDB.foz";
    {
        std::fstream stream(source, std::ios::binary | std::ios::in | std::ios::out);
        stream.seekp(blobOffset + 40);
        stream.put(1); // host bytes tampered, while structural layout is intact
    }
    {
        Cache::Session corrupt;
        Check(corrupt.Start(directory, bucket) && !corrupt.Loaded());
    }
    const auto link = directory + "/symlink";
    Check(symlink(directory.c_str(), link.c_str()) == 0);
    Cache::Session denied;
    Check(!denied.Start(link, bucket));
    Check(!denied.Start("relative", bucket));
    std::filesystem::remove_all(directory);
    std::puts("PASS: live completed checkpoints, nonblocking native flock, immutable clones, no-op and eviction, cold/warm integrity, bounded FOZ/blob validation, path denial");
}
