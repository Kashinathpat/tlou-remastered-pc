// Cache packs (video_core/blob_pack.h) and the driver pipeline cache store
// (vk_driver_pipeline_cache.h).
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>
#include "video_core/blob_pack.h"
#include "video_core/renderer_vulkan/vk_driver_pipeline_cache.h"

namespace fs = std::filesystem;
using Storage::BlobPack;

namespace {

void Write(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

std::string Bytes(const std::vector<uint8_t>& data) {
    return {data.begin(), data.end()};
}

bool Shader(std::string_view name) {
    return name.ends_with(".spv") || name.ends_with(".meta");
}

// What the game does: read every file (from the pack or from disk, then Keep), Finish.
int ReadAll(BlobPack& pack, const fs::path& dir, int count) {
    int served = 0;
    for (int i = 0; i < count; ++i) {
        for (const char* ext : {".spv", ".meta"}) {
            const auto file_name = "s" + std::to_string(i) + ext;
            std::vector<uint8_t> data;
            if (pack.Read(file_name, data)) {
                ++served;
                continue;
            }
            std::ifstream in(dir / file_name, std::ios::binary);
            data.assign(std::istreambuf_iterator<char>(in), {});
            pack.Keep(file_name, data);
        }
    }
    return served;
}

void TestPack(const fs::path& dir) {
    for (int i = 0; i < 40; ++i) {
        Write(dir / ("s" + std::to_string(i) + ".spv"), "spirv-" + std::to_string(i) + "-abcd");
        Write(dir / ("s" + std::to_string(i) + ".meta"), "meta-" + std::to_string(i));
    }
    Write(dir / "profile.bin", "not packed");
    {
        BlobPack pack{dir, "shaders.pack", Shader};
        assert(ReadAll(pack, dir, 40) == 0); // no pack yet
        pack.Keep("profile.bin", std::vector<uint8_t>(10)); // not listed: ignored
        assert(fs::exists(dir / "shaders.pack.new.tmp"));
        const auto result = pack.Finish();
        assert(result.built && result.written == 80 && result.loose_files == 80);
        assert(fs::exists(dir / "shaders.pack.new") && !fs::exists(dir / "shaders.pack"));
        assert(!pack.Finish().built); // once
    }
    // The next start: the new pack takes the place, every file is served from it.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    Write(dir / "s5.spv", "rewritten-5"); // replaced after the pack was written
    {
        BlobPack pack{dir, "shaders.pack", Shader};
        std::vector<uint8_t> data;
        assert(pack.Read("s1.spv", data) && Bytes(data) == "spirv-1-abcd");
        assert(pack.Read("s7.meta", data) && Bytes(data) == "meta-7");
        assert(fs::exists(dir / "shaders.pack") && !fs::exists(dir / "shaders.pack.new"));
        assert(!fs::exists(dir / "shaders.pack.new.tmp")); // one stale file: no new pack
        assert(!pack.Read("s5.spv", data));         // stale: the loose file is read instead
        assert(!pack.Read("profile.bin", data));    // filtered out
        assert(!pack.Read("missing.spv", data));
        std::vector<uint32_t> words;
        assert(!pack.Read("s7.meta", words));       // 6 bytes: not whole words
        assert(!pack.Read("s10.spv", words));       // 13 bytes
        assert(pack.Read("s1.spv", words) && words.size() == 3);
        pack.Invalidate("s2.spv");
        assert(!pack.Read("s2.spv", data));
        assert(pack.Hits() == 3 && pack.ValidEntries() == 79);
        assert(!pack.Finish().built);
        // Close unmaps (Clear and SwapCache need that); the next read maps it again.
        pack.Close();
        assert(pack.Read("s1.spv", data) && Bytes(data) == "spirv-1-abcd");
        pack.Close();
    }
    // Many new files: a new pack is written from the reads, old entries and new files alike.
    for (int i = 40; i < 80; ++i) {
        Write(dir / ("s" + std::to_string(i) + ".spv"), "new-spirv-" + std::to_string(i));
        Write(dir / ("s" + std::to_string(i) + ".meta"), "new-meta-" + std::to_string(i));
    }
    {
        BlobPack pack{dir, "shaders.pack", Shader};
        assert(ReadAll(pack, dir, 80) == 79); // the old pack still serves (s5 stale)
        const auto result = pack.Finish();
        assert(result.built && result.written == 160 && result.covered == 79);
    }
    {
        BlobPack pack{dir, "shaders.pack", Shader};
        assert(ReadAll(pack, dir, 80) == 160);
        std::vector<uint8_t> data;
        assert(pack.Read("s5.spv", data) && Bytes(data) == "rewritten-5");
        assert(pack.Read("s60.meta", data) && Bytes(data) == "new-meta-60");
        pack.Close();
    }
    // Many reads from disk (here: entries dropped) mark the pack; the next start writes a new one.
    {
        BlobPack pack{dir, "shaders.pack", Shader};
        for (int i = 0; i < 40; ++i) {
            pack.Invalidate("s" + std::to_string(i) + ".meta");
        }
        std::vector<uint8_t> data;
        pack.Read("s0.spv", data); // opens it
        for (int i = 0; i < 40; ++i) {
            pack.Invalidate("s" + std::to_string(i) + ".meta");
        }
        assert(ReadAll(pack, dir, 80) == 120);
        assert(!pack.Finish().built && fs::exists(dir / "shaders.pack.stale"));
    }
    {
        BlobPack pack{dir, "shaders.pack", Shader};
        assert(ReadAll(pack, dir, 80) == 160);
        assert(!fs::exists(dir / "shaders.pack.stale"));
        assert(pack.Finish().built);
    }
    {
        BlobPack pack{dir, "shaders.pack", Shader};
        assert(ReadAll(pack, dir, 80) == 160);
        assert(!fs::exists(dir / "shaders.pack.new.tmp")); // nothing new: no new pack
        pack.Close();
    }
    // A damaged entry is a miss; a damaged index drops the pack.
    {
        std::fstream file(dir / "shaders.pack", std::ios::binary | std::ios::in | std::ios::out);
        file.seekp(48 + 2); // inside the first entry's data
        file.put('#');
    }
    {
        BlobPack pack{dir, "shaders.pack", Shader};
        assert(ReadAll(pack, dir, 80) == 159);
        pack.Close();
    }
    {
        const auto size = fs::file_size(dir / "shaders.pack");
        std::fstream file(dir / "shaders.pack", std::ios::binary | std::ios::in | std::ios::out);
        file.seekp(std::streamoff(size - 3));
        file.put('#');
    }
    {
        BlobPack pack{dir, "shaders.pack", Shader};
        std::vector<uint8_t> data;
        assert(!pack.Read("s1.spv", data));
        assert(!fs::exists(dir / "shaders.pack")); // removed: written again
        // A session that ends before Finish leaves no pack; the next start removes its rest.
    }
    assert(!fs::exists(dir / "shaders.pack.new.tmp") && !fs::exists(dir / "shaders.pack.new"));
    {
        BlobPack pack{dir, "shaders.pack", Shader};
        ReadAll(pack, dir, 10); // 20 of 160: too few to stand for the cache? still the only pack
        const auto result = pack.Finish();
        assert(result.built && result.written == 20);
    }
}

void TestDriverStore(const fs::path& dir) {
    using namespace Vulkan;
    DriverCacheStamp stamp{0x8086, 0x3e9b, 0x1234, {}};
    for (int i = 0; i < 16; ++i) {
        stamp.cache_uuid[i] = uint8_t(i * 7);
    }
    // VkPipelineCacheHeaderVersionOne + payload.
    std::vector<uint8_t> data(32 + 100, 0x5a);
    const uint32_t fields[4] = {32, 1, stamp.vendor_id, stamp.device_id};
    std::memcpy(data.data(), fields, sizeof(fields));
    std::memcpy(data.data() + 16, stamp.cache_uuid.data(), 16);

    const auto file = dir / "vulkan-pipelines.bin";
    assert(SaveDriverPipelineCache(stamp, file, data));
    assert(LoadDriverPipelineCache(stamp, file) == data);
    auto other = stamp;
    other.driver_version = 0x1235; // a driver update
    assert(LoadDriverPipelineCache(other, file).empty());
    other = stamp;
    other.cache_uuid[3] ^= 1;
    assert(LoadDriverPipelineCache(other, file).empty());
    // Damaged data.
    auto encoded = EncodeDriverPipelineCache(stamp, data);
    encoded.back() ^= 1;
    assert(!DecodeDriverPipelineCache(stamp, encoded));
    // The driver's own header must name this device.
    auto foreign = data;
    foreign[8] ^= 1;
    assert(!DecodeDriverPipelineCache(stamp, EncodeDriverPipelineCache(stamp, foreign)));
    assert(!SaveDriverPipelineCache(stamp, file, std::vector<uint8_t>(8)));
    assert(!fs::exists(dir / "vulkan-pipelines.bin.tmp"));

    assert(DriverPipelineCacheScope(false) == DriverCacheScope::All &&
           DriverPipelineCacheScope(true) == DriverCacheScope::Compute);
}

} // namespace

int main() {
#ifdef _WIN32
    _set_error_mode(_OUT_TO_STDERR);
#endif
    const auto dir = fs::temp_directory_path() /
                     ("gow3-blob-pack-test-" + std::to_string(std::chrono::steady_clock::now()
                                                                   .time_since_epoch()
                                                                   .count()));
    fs::create_directories(dir);
    TestPack(dir);
    TestDriverStore(dir);
    fs::remove_all(dir);
    std::puts("blob pack test: ok");
    return 0;
}
