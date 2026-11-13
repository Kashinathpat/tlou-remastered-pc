// SPDX-License-Identifier: GPL-2.0-or-later
// gow3 13 Nov 2026: see vk_driver_pipeline_cache.h.

#include "video_core/renderer_vulkan/vk_driver_pipeline_cache.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>

#include <xxhash.h>

namespace Vulkan {

namespace {

constexpr uint32_t StoreMagic = 0x43505647u; // "GVPC"
constexpr uint32_t StoreVersion = 1;
constexpr uint64_t MaxStoreBytes = 512ull << 20;

struct StoreHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t vendor_id;
    uint32_t device_id;
    uint32_t driver_version;
    uint32_t reserved;
    uint8_t cache_uuid[16];
    uint64_t data_size;
    uint64_t data_hash;
};
static_assert(sizeof(StoreHeader) == 56);

// VkPipelineCacheHeaderVersionOne, as the driver writes it at the start of its data.
struct DriverHeader {
    uint32_t header_size;
    uint32_t header_version;
    uint32_t vendor_id;
    uint32_t device_id;
    uint8_t cache_uuid[16];
};
static_assert(sizeof(DriverHeader) == 32);

} // namespace

std::vector<uint8_t> EncodeDriverPipelineCache(const DriverCacheStamp& stamp,
                                               std::span<const uint8_t> data) {
    StoreHeader header{};
    header.magic = StoreMagic;
    header.version = StoreVersion;
    header.vendor_id = stamp.vendor_id;
    header.device_id = stamp.device_id;
    header.driver_version = stamp.driver_version;
    std::memcpy(header.cache_uuid, stamp.cache_uuid.data(), sizeof(header.cache_uuid));
    header.data_size = data.size();
    header.data_hash = XXH3_64bits(data.data(), data.size());
    std::vector<uint8_t> bytes(sizeof(header) + data.size());
    std::memcpy(bytes.data(), &header, sizeof(header));
    if (!data.empty()) {
        std::memcpy(bytes.data() + sizeof(header), data.data(), data.size());
    }
    return bytes;
}

std::optional<std::span<const uint8_t>> DecodeDriverPipelineCache(const DriverCacheStamp& stamp,
                                                                  std::span<const uint8_t> file) {
    StoreHeader header{};
    if (file.size() < sizeof(header) + sizeof(DriverHeader)) {
        return std::nullopt;
    }
    std::memcpy(&header, file.data(), sizeof(header));
    if (header.magic != StoreMagic || header.version != StoreVersion ||
        header.vendor_id != stamp.vendor_id || header.device_id != stamp.device_id ||
        header.driver_version != stamp.driver_version ||
        std::memcmp(header.cache_uuid, stamp.cache_uuid.data(), sizeof(header.cache_uuid)) != 0 ||
        header.data_size != file.size() - sizeof(header)) {
        return std::nullopt;
    }
    const auto data = file.subspan(sizeof(header));
    if (XXH3_64bits(data.data(), data.size()) != header.data_hash) {
        return std::nullopt;
    }
    DriverHeader driver{};
    std::memcpy(&driver, data.data(), sizeof(driver));
    if (driver.header_size < sizeof(driver) || driver.header_size > data.size() ||
        driver.header_version != 1 || driver.vendor_id != stamp.vendor_id ||
        driver.device_id != stamp.device_id ||
        std::memcmp(driver.cache_uuid, stamp.cache_uuid.data(), sizeof(driver.cache_uuid)) != 0) {
        return std::nullopt;
    }
    return data;
}

std::vector<uint8_t> LoadDriverPipelineCache(const DriverCacheStamp& stamp,
                                             const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary | std::ios::ate);
    if (!in) {
        return {};
    }
    const auto size = in.tellg();
    if (size <= 0 || uint64_t(size) > MaxStoreBytes) {
        return {};
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    in.seekg(0);
    if (!in.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()))) {
        return {};
    }
    const auto data = DecodeDriverPipelineCache(stamp, bytes);
    if (!data) {
        return {};
    }
    return {data->begin(), data->end()};
}

bool SaveDriverPipelineCache(const DriverCacheStamp& stamp, const std::filesystem::path& file,
                             std::span<const uint8_t> data) {
    if (data.size() < sizeof(DriverHeader) || data.size() > MaxStoreBytes) {
        return false;
    }
    const auto bytes = EncodeDriverPipelineCache(stamp, data);
    static std::mutex writing; // the periodic save and the one at exit share the temporary file
    std::scoped_lock lock{writing};
    auto temp = file;
    temp += ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) {
            return false;
        }
        out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        out.flush();
        if (!out) {
            out.close();
            std::error_code error;
            std::filesystem::remove(temp, error);
            return false;
        }
    }
    std::error_code error;
    std::filesystem::rename(temp, file, error);
    if (error) {
        std::filesystem::remove(temp, error);
        return false;
    }
    return true;
}

bool DriverPipelineCacheWanted(bool pipeline_binaries) {
    if (const char* env = std::getenv("GOW3_VK_PIPELINE_CACHE"); env && *env) {
        return env[0] != '0';
    }
    return !pipeline_binaries;
}

} // namespace Vulkan
