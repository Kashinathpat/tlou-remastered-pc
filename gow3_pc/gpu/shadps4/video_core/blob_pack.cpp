// SPDX-License-Identifier: GPL-2.0-or-later
// gow3 13 Nov 2026: see blob_pack.h.

#include "video_core/blob_pack.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <unordered_set>

#include <xxhash.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace Storage {

namespace {

constexpr uint32_t PackMagic = 0x4b415047u; // "GPAK"
constexpr uint32_t PackVersion = 1;
constexpr uint64_t MaxEntryBytes = 64ull << 20;
constexpr uint64_t MaxPackBytes = 4ull << 30;

struct Header {
    uint32_t magic;
    uint32_t version;
    uint64_t entry_count;
    uint64_t index_offset;
    uint64_t index_size;
    uint64_t index_hash;
    uint64_t started; ///< when its session listed the files (file time units): newer ones are not in it
};
static_assert(sizeof(Header) == 48);

// Index record: u64 offset, u64 size, u64 time, u64 hash, u16 name length, name bytes.
constexpr size_t RecordFixed = 4 * sizeof(uint64_t) + sizeof(uint16_t);

std::filesystem::path WithSuffix(const std::filesystem::path& path, const char* suffix) {
    auto result = path;
    result += suffix;
    return result;
}

[[maybe_unused]] bool Stamp(const std::filesystem::path& path, BlobPack::FileStamp& stamp) {
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) ||
        (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        return false;
    }
    stamp.size = (uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    stamp.time = (uint64_t(data.ftLastWriteTime.dwHighDateTime) << 32) |
                 data.ftLastWriteTime.dwLowDateTime;
    return true;
#else
    struct stat st{};
    if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
        return false;
    }
    stamp.size = uint64_t(st.st_size);
    stamp.time = uint64_t(st.st_mtim.tv_sec) * 1000000000ull + uint64_t(st.st_mtim.tv_nsec);
    return true;
#endif
}

/// Now, in the units of FileStamp::time.
uint64_t NowStamp() {
#ifdef _WIN32
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    return (uint64_t(now.dwHighDateTime) << 32) | now.dwLowDateTime;
#else
    timespec now{};
    clock_gettime(CLOCK_REALTIME, &now);
    return uint64_t(now.tv_sec) * 1000000000ull + uint64_t(now.tv_nsec);
#endif
}

} // namespace

struct BlobPack::Mapping {
    std::unordered_map<std::string, uint32_t> index; ///< every entry of the pack file
    std::vector<Entry> entries;
    std::vector<FileStamp> stamps;
    std::unique_ptr<std::atomic<uint8_t>[]> valid;
    size_t valid_count = 0;
    uint64_t started = 0;
    const uint8_t* base = nullptr;
    uint64_t size = 0;
#ifdef _WIN32
    HANDLE file = INVALID_HANDLE_VALUE;
    HANDLE section = nullptr;
#else
    int fd = -1;
#endif

    ~Mapping() {
#ifdef _WIN32
        if (base) {
            UnmapViewOfFile(base);
        }
        if (section) {
            CloseHandle(section);
        }
        if (file != INVALID_HANDLE_VALUE) {
            CloseHandle(file);
        }
#else
        if (base) {
            munmap(const_cast<uint8_t*>(base), size);
        }
        if (fd >= 0) {
            close(fd);
        }
#endif
    }

    bool Map(const std::filesystem::path& path) {
#ifdef _WIN32
        file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            return false;
        }
        LARGE_INTEGER length{};
        if (!GetFileSizeEx(file, &length) || length.QuadPart <= LONGLONG(sizeof(Header)) ||
            uint64_t(length.QuadPart) > MaxPackBytes) {
            return false;
        }
        size = uint64_t(length.QuadPart);
        section = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!section) {
            return false;
        }
        base = static_cast<const uint8_t*>(MapViewOfFile(section, FILE_MAP_READ, 0, 0, 0));
        if (!base) {
            return false;
        }
        // The warm-up reads nearly all of it, in the order it was written: ask for the whole
        // file in large reads now (Windows 8 and later; a hint, failures are harmless).
        using PrefetchFn = BOOL(WINAPI*)(HANDLE, ULONG_PTR, PWIN32_MEMORY_RANGE_ENTRY, ULONG);
        static const auto prefetch = reinterpret_cast<PrefetchFn>(reinterpret_cast<void*>(
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "PrefetchVirtualMemory")));
        if (prefetch) {
            WIN32_MEMORY_RANGE_ENTRY range{const_cast<uint8_t*>(base), size_t(size)};
            prefetch(GetCurrentProcess(), 1, &range, 0);
        }
        return true;
#else
        fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            return false;
        }
        struct stat st{};
        if (fstat(fd, &st) != 0 || st.st_size <= off_t(sizeof(Header)) ||
            uint64_t(st.st_size) > MaxPackBytes) {
            return false;
        }
        size = uint64_t(st.st_size);
        void* view = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (view == MAP_FAILED) {
            return false;
        }
        base = static_cast<const uint8_t*>(view);
        madvise(view, size, MADV_WILLNEED);
        return true;
#endif
    }

    /// The index, checked against the file's bounds and its hash.
    bool Parse() {
        Header header{};
        std::memcpy(&header, base, sizeof(header));
        if (header.magic != PackMagic || header.version != PackVersion ||
            header.index_offset < sizeof(Header) || header.index_offset > size ||
            header.index_size > size - header.index_offset || header.entry_count > (1u << 24)) {
            return false;
        }
        const uint8_t* at = base + header.index_offset;
        const uint8_t* end = at + header.index_size;
        if (XXH3_64bits(at, header.index_size) != header.index_hash) {
            return false;
        }
        started = header.started;
        entries.reserve(header.entry_count);
        stamps.reserve(header.entry_count);
        index.reserve(header.entry_count);
        for (uint64_t i = 0; i < header.entry_count; ++i) {
            if (size_t(end - at) < RecordFixed) {
                return false;
            }
            uint64_t fields[4];
            uint16_t name_size;
            std::memcpy(fields, at, sizeof(fields));
            std::memcpy(&name_size, at + sizeof(fields), sizeof(name_size));
            at += RecordFixed;
            if (size_t(end - at) < name_size || fields[0] < sizeof(Header) ||
                fields[0] > header.index_offset || fields[1] > header.index_offset - fields[0]) {
                return false;
            }
            std::string file_name(reinterpret_cast<const char*>(at), name_size);
            at += name_size;
            const auto slot = uint32_t(entries.size());
            if (!index.try_emplace(std::move(file_name), slot).second) {
                return false;
            }
            entries.push_back({fields[0], fields[1], fields[3]});
            stamps.push_back({fields[1], fields[2]});
        }
        return at == end;
    }
};

BlobPack::BlobPack(std::filesystem::path directory_, std::string name_, Filter filter_)
    : directory{std::move(directory_)}, name{std::move(name_)}, filter{std::move(filter_)} {}

bool BlobPack::Enabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("GOW3_CACHE_PACK");
        return !(env && env[0] == '0');
    }();
    return enabled;
}

std::unordered_map<std::string, BlobPack::FileStamp> BlobPack::List(
    const std::filesystem::path& directory, const Filter& filter) {
    std::unordered_map<std::string, FileStamp> files;
#ifdef _WIN32
    WIN32_FIND_DATAW data{};
    const auto pattern = directory / L"*";
    HANDLE find = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch,
                                   nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE) {
        return files;
    }
    do {
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            continue;
        }
        const std::string file_name = std::filesystem::path{data.cFileName}.string();
        if (filter && !filter(file_name)) {
            continue;
        }
        files.emplace(file_name,
                      FileStamp{(uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow,
                                (uint64_t(data.ftLastWriteTime.dwHighDateTime) << 32) |
                                    data.ftLastWriteTime.dwLowDateTime});
    } while (FindNextFileW(find, &data));
    FindClose(find);
#else
    DIR* dir = opendir(directory.c_str());
    if (!dir) {
        return files;
    }
    while (const dirent* entry = readdir(dir)) {
        const std::string file_name = entry->d_name;
        if (file_name == "." || file_name == ".." || (filter && !filter(file_name))) {
            continue;
        }
        FileStamp stamp;
        if (Stamp(directory / file_name, stamp)) {
            files.emplace(file_name, stamp);
        }
    }
    closedir(dir);
#endif
    return files;
}

std::shared_ptr<BlobPack::Mapping> BlobPack::Acquire() {
    std::scoped_lock lock{mutex};
    if (tried) {
        return mapping;
    }
    tried = true;
    if (!Enabled()) {
        return nullptr;
    }
    const auto pack = directory / name;
    const auto fresh = WithSuffix(pack, ".new");
    std::error_code error;
    std::filesystem::remove(WithSuffix(fresh, ".tmp"), error); // a session that ended mid-write
    const auto marker = WithSuffix(pack, ".stale");
    const bool marked = std::filesystem::exists(marker, error);
    std::filesystem::remove(marker, error);
    if (std::filesystem::exists(fresh, error)) {
        // Written by an earlier session: it takes the old pack's place now that nothing maps it.
        std::filesystem::remove(pack, error);
        std::filesystem::rename(fresh, pack, error);
        if (error) {
            std::filesystem::remove(fresh, error);
        }
    }
    auto loose = List(directory, filter);
    std::shared_ptr<Mapping> map;
    if (std::filesystem::exists(pack, error)) {
        map = std::make_shared<Mapping>();
        if (!map->Map(pack) || !map->Parse()) {
            map.reset();
            std::filesystem::remove(pack, error); // damaged or of another version: written again
        }
    }
    size_t pack_entries = 0;
    const uint64_t listed_at = NowStamp();
    if (map) {
        pack_entries = map->entries.size();
        map->valid = std::make_unique<std::atomic<uint8_t>[]>(map->entries.size());
        for (const auto& [file_name, slot] : map->index) {
            const auto it = loose.find(file_name);
            const bool same = it != loose.end() && it->second.size == map->stamps[slot].size &&
                              it->second.time == map->stamps[slot].time;
            map->valid[slot].store(same ? 1 : 0, std::memory_order_relaxed);
            map->valid_count += same;
        }
    }
    covered = map ? map->valid_count : 0;
    loose_files = loose.size();
    mapping = map;
    // A new pack is written from what this session reads when there is none, when enough files
    // came after it (new shaders; older files left out were not read by its session, like the
    // binaries of pipelines no longer used), when it holds many that are gone, or when the last
    // session read many files from disk (the ".stale" marker Finish leaves).
    size_t newer = 0;
    if (map) {
        for (const auto& [file_name, stamp] : loose) {
            if (stamp.time >= map->started) {
                const auto it = map->index.find(file_name);
                newer += it == map->index.end() || !map->valid[it->second].load();
            }
        }
    }
    const size_t dead = pack_entries - covered;
    const bool worth = loose.size() >= 32 &&
                       (!map || marked || (newer >= 32 && newer * 20 >= loose.size()) ||
                        newer >= 512 || (dead >= 256 && dead * 4 >= pack_entries));
    if (worth) {
        auto out = std::make_unique<std::ofstream>(WithSuffix(fresh, ".tmp"),
                                                   std::ios::binary | std::ios::trunc);
        if (*out) {
            const Header header{};
            out->write(reinterpret_cast<const char*>(&header), sizeof(header));
            capture = std::make_unique<Capture>();
            capture->out = std::move(out);
            capture->stamps = std::move(loose);
            capture->offset = sizeof(Header);
            capture->started = listed_at;
        }
    }
    return mapping;
}

size_t BlobPack::ValidEntries() {
    std::scoped_lock lock{mutex};
    return mapping ? mapping->valid_count : 0;
}

std::span<const uint8_t> BlobPack::Find(const Mapping& map, std::string_view file_name,
                                        size_t& slot) const {
    const auto it = map.index.find(std::string{file_name});
    if (it == map.index.end() || !map.valid[it->second].load(std::memory_order_relaxed)) {
        return {};
    }
    slot = it->second;
    const auto& entry = map.entries[slot];
    return {map.base + entry.offset, size_t(entry.size)};
}

void BlobPack::KeepLocked(std::string_view file_name, std::span<const uint8_t> bytes) {
    if (!capture || !capture->out) {
        return;
    }
    const auto it = capture->stamps.find(std::string{file_name});
    // Only a file listed at the start, as big as it was then: a later save of it changed its
    // write time, so a copy of the new bytes under the old stamp is never used.
    if (it == capture->stamps.end() || it->second.size != bytes.size() || bytes.empty() ||
        bytes.size() > MaxEntryBytes || capture->offset + bytes.size() > MaxPackBytes / 2 ||
        !capture->kept.insert(it->first).second) {
        return;
    }
    capture->out->write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    if (!*capture->out) {
        capture->out.reset(); // disk full or the like: abandoned at Finish
        return;
    }
    const uint64_t fields[4] = {capture->offset, it->second.size, it->second.time,
                                XXH3_64bits(bytes.data(), bytes.size())};
    const auto name_size = uint16_t(std::min<size_t>(file_name.size(), 0xffff));
    auto& index = capture->index;
    const size_t at = index.size();
    index.resize(at + RecordFixed + name_size);
    std::memcpy(index.data() + at, fields, sizeof(fields));
    std::memcpy(index.data() + at + sizeof(fields), &name_size, sizeof(name_size));
    std::memcpy(index.data() + at + RecordFixed, file_name.data(), name_size);
    capture->offset += bytes.size();
    ++capture->count;
}

void BlobPack::Keep(std::string_view file_name, std::span<const uint8_t> bytes) {
    if (!Enabled()) {
        return;
    }
    Acquire();
    std::scoped_lock lock{mutex};
    KeepLocked(file_name, bytes);
}

template <typename T>
bool BlobPack::ReadAs(std::string_view file_name, std::vector<T>& out) {
    if (!Enabled()) {
        return false;
    }
    const auto map = Acquire();
    size_t slot = 0;
    const auto bytes = map ? Find(*map, file_name, slot) : std::span<const uint8_t>{};
    if (bytes.empty() || bytes.size() % sizeof(T) != 0) {
        misses.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (XXH3_64bits(bytes.data(), bytes.size()) != map->entries[slot].hash) {
        map->valid[slot].store(0, std::memory_order_relaxed); // damaged on disk: the loose file
        misses.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    out.resize(bytes.size() / sizeof(T));
    std::memcpy(out.data(), bytes.data(), bytes.size());
    hits.fetch_add(1, std::memory_order_relaxed);
    {
        std::scoped_lock lock{mutex};
        KeepLocked(file_name, bytes); // the next pack holds it too
    }
    return true;
}

bool BlobPack::Read(std::string_view file_name, std::vector<uint8_t>& out) {
    return ReadAs(file_name, out);
}

bool BlobPack::Read(std::string_view file_name, std::vector<uint32_t>& out) {
    return ReadAs(file_name, out);
}

void BlobPack::Invalidate(std::string_view file_name) {
    std::shared_ptr<Mapping> map;
    {
        std::scoped_lock lock{mutex};
        map = mapping;
    }
    if (!map) {
        return;
    }
    if (const auto it = map->index.find(std::string{file_name}); it != map->index.end()) {
        map->valid[it->second].store(0, std::memory_order_relaxed);
    }
}

void BlobPack::AbandonLocked() {
    if (!capture) {
        return;
    }
    capture->out.reset();
    capture.reset();
    std::error_code error;
    std::filesystem::remove(WithSuffix(WithSuffix(directory / name, ".new"), ".tmp"), error);
}

void BlobPack::Close() {
    std::scoped_lock lock{mutex};
    AbandonLocked();
    mapping.reset();
    tried = false;
}

BlobPack::~BlobPack() {
    std::scoped_lock lock{mutex};
    AbandonLocked();
}

BlobPack::FinishResult BlobPack::Finish() {
    std::scoped_lock lock{mutex};
    FinishResult result;
    result.loose_files = loose_files;
    result.covered = covered;
    if (!capture) {
        // Many reads from disk this session (files the pack's session did not read): the next
        // start writes a new one.
        const uint64_t missed = misses.load(), served = hits.load();
        if (mapping && (missed >= 512 || (missed >= 32 && missed * 20 >= missed + served))) {
            std::ofstream{WithSuffix(directory / name, ".stale")};
        }
        return result;
    }
    // Too little was read to replace the current pack (a short session): it stays.
    if (!capture->out || capture->count == 0 || capture->count < covered / 2) {
        AbandonLocked();
        return result;
    }
    Header header{};
    header.magic = PackMagic;
    header.version = PackVersion;
    header.entry_count = capture->count;
    header.index_offset = capture->offset;
    header.index_size = capture->index.size();
    header.index_hash = XXH3_64bits(capture->index.data(), capture->index.size());
    header.started = capture->started;
    auto& out = *capture->out;
    out.write(reinterpret_cast<const char*>(capture->index.data()),
              std::streamsize(capture->index.size()));
    out.seekp(0);
    out.write(reinterpret_cast<const char*>(&header), sizeof(header));
    out.flush();
    const bool written = bool(out);
    out.close();
    const auto fresh = WithSuffix(directory / name, ".new");
    std::error_code error;
    if (written) {
        std::filesystem::rename(WithSuffix(fresh, ".tmp"), fresh, error);
    }
    if (!written || error) {
        AbandonLocked();
        return result;
    }
    result.written = size_t(capture->count);
    result.bytes = capture->offset - sizeof(Header);
    result.built = true;
    capture.reset();
    return result;
}

} // namespace Storage
