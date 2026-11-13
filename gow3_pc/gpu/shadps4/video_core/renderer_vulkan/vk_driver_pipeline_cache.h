// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// gow3 13 Nov 2026: the driver's pipeline cache (VkPipelineCache data) kept on disk for GPUs
// without VK_KHR_pipeline_binary. Intel's Windows drivers (UHD 630 101.2145, Arc B580 101.9150)
// and AMD's APU drivers (Vega 8, Radeon Graphics) have no pipeline binaries, so the port kept
// nothing of the driver's compiles between sessions and relied on the driver's own disk cache
// (B580: 13.8 ms a pipeline without it, 0.5 ms with it). This store keeps the compiled pipelines
// when that cache is off, full, or was emptied (AMD's "Reset Shader Cache", Disk Cleanup).
// <cache>/vulkan-pipelines.bin: a header naming the device and driver, the data's size and hash,
// then the driver's data. GOW3_VK_PIPELINE_CACHE=0: off; =1: on with pipeline binaries too.

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <vector>

namespace Vulkan {

struct DriverCacheStamp {
    uint32_t vendor_id = 0;
    uint32_t device_id = 0;
    uint32_t driver_version = 0;
    std::array<uint8_t, 16> cache_uuid{};
};

/// The file's bytes for the driver's `data`.
std::vector<uint8_t> EncodeDriverPipelineCache(const DriverCacheStamp& stamp,
                                               std::span<const uint8_t> data);
/// The driver's data in `file` when it was written for this device and driver and is whole
/// (also checked: the data's own VkPipelineCacheHeaderVersionOne).
std::optional<std::span<const uint8_t>> DecodeDriverPipelineCache(const DriverCacheStamp& stamp,
                                                                  std::span<const uint8_t> file);

/// Reads and checks the store; empty when there is none or it does not fit this driver.
std::vector<uint8_t> LoadDriverPipelineCache(const DriverCacheStamp& stamp,
                                             const std::filesystem::path& file);
/// Writes beside and renames into place.
bool SaveDriverPipelineCache(const DriverCacheStamp& stamp, const std::filesystem::path& file,
                             std::span<const uint8_t> data);

/// Whether the store is used: GOW3_VK_PIPELINE_CACHE, else only without pipeline binaries.
bool DriverPipelineCacheWanted(bool pipeline_binaries);

/// At window close: the driver's cache to disk, waiting at most `timeout_ms` (vk_pipeline_cache.cpp).
void SaveDriverPipelineCacheOnExit(unsigned timeout_ms);

} // namespace Vulkan
