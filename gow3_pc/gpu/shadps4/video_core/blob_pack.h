// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// gow3 13 Nov 2026: a read-only pack of a cache directory's small files. The shader cache is
// ~27,000 files (.key/.meta/.spv) and the pipeline binaries ~13,000 more; the background warm-up
// opens each one (0.8-1.4 ms a file with the disk cache warm on the GTX 1050 Ti laptop, ~40 s of
// reader threads cold, 6 s for 214 shaders on a Vega 8 laptop), while one file of the same bytes
// reads in 0.8 s. The loose files stay the cache (written, replaced and removed as before); the
// pack holds copies of them and serves a read only while the loose file has the size and write
// time the copy was taken with, and the copy's hash matches. When the pack misses enough files, the
// session writes a new one (<name>.new) from the bytes the warm-up reads anyway, in that order (a
// second read of every file took ~5 minutes), and it replaces the old one at the next start (a
// mapped file cannot be replaced on Windows).

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Storage {

class BlobPack {
public:
    using Filter = std::function<bool(std::string_view file_name)>;

    struct FinishResult {
        bool built = false;
        size_t loose_files = 0; ///< at the start of the session
        size_t covered = 0;     ///< of those, held by the pack as they were
        size_t written = 0;     ///< entries of the new pack
        uint64_t bytes = 0;     ///< its data bytes
    };

    /// `directory`'s files (not its subdirectories) accepted by `filter`, in <directory>/<name>.
    BlobPack(std::filesystem::path directory, std::string name, Filter filter);
    ~BlobPack();
    BlobPack(const BlobPack&) = delete;
    BlobPack& operator=(const BlobPack&) = delete;

    /// The file's bytes from the pack, when it holds them as the loose file is now. Opens the pack
    /// on first use. Thread-safe.
    bool Read(std::string_view file_name, std::vector<uint8_t>& out);
    bool Read(std::string_view file_name, std::vector<uint32_t>& out);
    /// A file read from disk (Read missed): kept for the new pack when one is being written.
    void Keep(std::string_view file_name, std::span<const uint8_t> bytes);
    /// The pack's copy is not used again this session (the file is being replaced or removed).
    void Invalidate(std::string_view file_name);
    /// Unmaps the pack and drops a pack being written; the next read opens it again (the
    /// directory was cleared or swapped).
    void Close();
    /// The files read so far become <name>.new, used from the next start, when the current pack
    /// missed enough of them (decided at the first read).
    FinishResult Finish();

    /// Reads served from the pack and from loose files since the start (for the logs).
    uint64_t Hits() const { return hits.load(std::memory_order_relaxed); }
    uint64_t Misses() const { return misses.load(std::memory_order_relaxed); }
    /// Valid entries of the opened pack (0 before the first read or without one).
    size_t ValidEntries();

    /// GOW3_CACHE_PACK=0 turns packs off (reads and writes).
    static bool Enabled();

    struct FileStamp {
        uint64_t size = 0;
        uint64_t time = 0;
    };
    /// The directory's files accepted by `filter`, with size and write time, in one listing.
    static std::unordered_map<std::string, FileStamp> List(const std::filesystem::path& directory,
                                                           const Filter& filter);

private:
    struct Mapping;
    struct Entry {
        uint64_t offset;
        uint64_t size;
        uint64_t hash;
    };
    /// The new pack being written from this session's reads (<name>.new.tmp).
    struct Capture {
        std::unique_ptr<std::ofstream> out;
        std::unordered_map<std::string, FileStamp> stamps; ///< the listing at the start
        std::unordered_set<std::string> kept;
        std::vector<uint8_t> index;
        uint64_t offset = 0;
        uint64_t count = 0;
        uint64_t started = 0;
    };
    std::shared_ptr<Mapping> Acquire();
    template <typename T>
    bool ReadAs(std::string_view file_name, std::vector<T>& out);
    std::span<const uint8_t> Find(const Mapping& map, std::string_view file_name,
                                  size_t& index) const;
    void KeepLocked(std::string_view file_name, std::span<const uint8_t> bytes);
    void AbandonLocked();

    std::filesystem::path directory;
    std::string name;
    Filter filter;

    std::mutex mutex; ///< open/close and the pack being written
    bool tried = false;
    std::shared_ptr<Mapping> mapping;
    std::unique_ptr<Capture> capture;
    size_t loose_files = 0, covered = 0;

    std::atomic<uint64_t> hits{0}, misses{0};
};

} // namespace Storage
