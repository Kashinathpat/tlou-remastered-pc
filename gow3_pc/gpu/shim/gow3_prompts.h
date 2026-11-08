#pragma once
// Keyboard and mouse button prompts (gow3_prompts.cpp).

#include <memory>
#include <vector>

#include "common/types.h"

namespace Vulkan {
class Instance;
class Scheduler;
class Runtime;
}

namespace Core { class MemoryManager; }
namespace AmdGpu { struct Image; }

namespace Gow3Prompts {

struct PromptGeometry {
    u32 num_indices = 0;
    u32 index_offset = 0;
    bool is_index16 = true;
    VAddr ib_addr = 0;
    VAddr vb_addr = 0;
    float viewport_w = 1920.0f;
    float viewport_h = 1080.0f;
};

bool IsFloatingPrompt(const AmdGpu::Image& s0, const AmdGpu::Image& s1, Core::MemoryManager* memory,
                      const PromptGeometry* geom = nullptr, u64 desc_hash = 0, int* out_button = nullptr);

// Texels to upload instead of an image's guest data: linear, per mip its offset, row length and
// height in texels (BC: whole blocks).
struct Texels {
    std::vector<u8> data;
    struct Mip {
        u32 offset, row, height;
    };
    std::vector<Mip> mips;
};

// Texture cache upload (TextureCache::RefreshImage) of a 512x512 RGBA8 image or a BC1 one (64-512):
// when its first mip is one of the games' button icon atlases or button pictures and other icons
// than the PlayStation ones show, the texels to upload instead; nullptr otherwise.
std::shared_ptr<const Texels> Replacement(VAddr address, u32 guest_size, u32 mip0_size, u32 width, u32 height,
                                          u32 levels, bool bc1, u32 tile_mode);

bool BindFloatingPrompt(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler,
                        Vulkan::Runtime& runtime, void* p_img0_info, void* p_img1_info,
                        int button);
void ClearFloatingPrompts();

} // namespace Gow3Prompts

// Pad side (runtime_pad.c): the labels per PS4 button (GOW3_PROMPT_* in gow3gpu.h) of set 0 (keys
// and mouse buttons) or 1 (other controllers), and the device whose icons show.
extern "C" void gow3gpu_prompts_set_labels(int set, const char* const* labels, int count);
extern "C" void gow3gpu_prompts_set_device(int device);
