// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/path_util.h"
#include "common/singleton.h"
#include "common/types.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <thread>
#include <vector>

namespace Storage {

enum class BlobType : u32 {
    ShaderMeta,
    ShaderBinary,
    PipelineKey,
    ShaderProfile,
};

class DataBase {
public:
    static DataBase& Instance() {
        return *Common::Singleton<DataBase>::Instance();
    }

    void Open();
    void Close();
    /// gow3: waits for queued and running writes; false if the timeout expires first.
    bool Flush(std::chrono::milliseconds timeout);
    [[nodiscard]] bool IsOpened() const {
        return opened;
    }
    void FinishPreload();
    /// gow3: removes every cached blob (an incompatible cache is rebuilt, not ignored).
    void Clear();
    /// gow3: puts the current cache aside (it belongs to another GPU) and moves back a cache put
    /// aside earlier whose stored profile `matches`. True when one was moved back; otherwise the
    /// cache starts empty.
    bool SwapCache(const std::function<bool(const std::vector<u8>&)>& matches);
    /// gow3: whether two blobs were written by the same save (within seconds of each other, no
    /// write of either left unfinished): a pair that is not was half replaced by a later session.
    [[nodiscard]] bool WrittenTogether(BlobType type_a, const std::string& name_a,
                                       BlobType type_b, const std::string& name_b) const;
    /// gow3: deletes a blob, in order with the saves queued before it.
    void Remove(BlobType type, const std::string& name);

    bool Save(BlobType type, const std::string& name, std::vector<u8>&& data);
    bool Save(BlobType type, const std::string& name, std::vector<u32>&& data);

    void Load(BlobType type, const std::string& name, std::vector<u8>& data);
    void Load(BlobType type, const std::string& name, std::vector<u32>& data);

    /// gow3: `stop` returning true ends the walk early (background warm-up at exit).
    void ForEachBlob(BlobType type, const std::function<void(std::vector<u8>&& data)>& func,
                     const std::function<bool()>& stop = {});
    /// gow3: blobs are plain files written beside and renamed into place, so other threads
    /// may read them while the IO thread saves (the zip archive cannot).
    [[nodiscard]] bool ConcurrentReads() const;
    /// gow3: number of blobs ForEachBlob visits (the loading screen's total).
    size_t CountBlobs(BlobType type);
    /// gow3: the names of the blobs of a type (as Load takes them), without reading them.
    void ForEachBlobName(BlobType type, const std::function<void(const std::string& name)>& func);
    /// gow3: whether a blob is stored (Load of a missing one logs an error). Plain files only:
    /// false for the zip archive.
    [[nodiscard]] bool Exists(BlobType type, const std::string& name) const;

private:
    std::jthread io_worker{};
    std::filesystem::path cache_path{};
    std::atomic<bool> opened{}; ///< gow3: read by the window thread's exit flush
};

} // namespace Storage
