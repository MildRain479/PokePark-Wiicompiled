/* Vulkan device for the GX decoder (gx_device.h), as gl_backend.c is the
 * OpenGL one, and drawing the same pictures: the EFB an offscreen image at
 * the internal resolution (multisampled with anti-aliasing), TEV and the
 * transform unit in shaders driven by the state each draw gives them,
 * copies to textures, and the picture shown in the window, with the
 * settings menu over it.
 *
 * Vulkan 1.3 (dynamic rendering, synchronization2), loaded from
 * vulkan-1.dll as the game starts: without it, or without a GPU that does
 * 1.3, the device can't start and OpenGL draws instead. The shaders are
 * in vk_shaders/, compiled to SPIR-V by the build (gen/vk_shaders.h).
 *
 * How it draws:
 * - Every texture (the game's, EFB copies, the EFB itself, the menu) has
 *   a number, its place in one array of textures the shaders index; a
 *   draw names its eight in push constants.
 * - A draw's XF state (packed: the parts the vertex shader reads) and TEV
 *   state go into the frame's data buffer, written only when they differ
 *   from the last draw's; the shaders read them at the offsets the push
 *   constants give.
 * - Pipelines differ in blending, color mask and whether there is an
 *   alpha test (made the first time each is needed); depth, culling,
 *   viewport and scissor are set per draw. Each TEV setup also gets a
 *   pipeline of its own (gx.frag specialized for it), made on a thread of
 *   its own while the general one draws; they are kept on disk
 *   (vk_pipelines.bin, beside settings.ini) for the next start.
 * - The EFB is kept with its top row first, as GX has it: the viewport is
 *   flipped, so copies and presenting read it as it is.
 * - Two frames are in flight. Uploads (textures, the recorded frame's
 *   vertices) go in a command buffer of their own, submitted before the
 *   frame's; a texture changed after a draw of the frame used it is
 *   changed in the frame's own commands instead, in order.
 *
 * Environment:
 *   RECOMP_VK_VALIDATE=1       the validation layers (Vulkan SDK), printing what they find
 *   RECOMP_VK_GENERAL=1        only the general TEV shader, none made for a setup */
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gx_device.h"

#ifndef RECOMP_VULKAN

static int vk_unavailable(void) {
    fprintf(stderr, "vulkan: not built in (no Vulkan SDK when the game was built)\n");
    return 0;
}
const GXDevice g_vk_device = {"Vulkan", vk_unavailable};

#else

#define VK_NO_PROTOTYPES
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define VK_USE_PLATFORM_WIN32_KHR
#define SURFACE_EXTENSION VK_KHR_WIN32_SURFACE_EXTENSION_NAME
#define SURFACE_FUNCS(X) X(vkCreateWin32SurfaceKHR)
#else
#include <dlfcn.h>
#include <pthread.h>
#include <sys/resource.h>
#include <android/native_window.h>
#define VK_USE_PLATFORM_ANDROID_KHR
#define SURFACE_EXTENSION VK_KHR_ANDROID_SURFACE_EXTENSION_NAME
#define SURFACE_FUNCS(X) X(vkCreateAndroidSurfaceKHR)
#endif
#include <vulkan/vulkan.h>

#include "host_settings.h"
#include "vk_shaders.h"

/* ------------------------------------------------------------------------ */
/* Entry points */

#define VK_GLOBAL_FUNCS(X)                       \
    X(vkCreateInstance)                          \
    X(vkEnumerateInstanceLayerProperties)        \
    X(vkEnumerateInstanceExtensionProperties)

#define VK_INSTANCE_FUNCS(X)                     \
    X(vkEnumeratePhysicalDevices)                \
    X(vkGetPhysicalDeviceProperties)             \
    X(vkGetPhysicalDeviceFeatures2)              \
    X(vkGetPhysicalDeviceQueueFamilyProperties)  \
    X(vkGetPhysicalDeviceMemoryProperties)           X(vkEnumerateDeviceExtensionProperties)      \
    X(vkCreateDevice)                            \
    X(vkGetDeviceProcAddr)                       \
    SURFACE_FUNCS(X)                             \
    X(vkDestroySurfaceKHR)                       \
    X(vkGetPhysicalDeviceSurfaceSupportKHR)      \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR)      \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR)

#define VK_DEVICE_FUNCS(X)              \
    X(vkGetDeviceQueue)                 \
    X(vkCreateSwapchainKHR)             \
    X(vkDestroySwapchainKHR)            \
    X(vkGetSwapchainImagesKHR)          \
    X(vkAcquireNextImageKHR)            \
    X(vkQueuePresentKHR)                \
    X(vkQueueSubmit)                    \
    X(vkDeviceWaitIdle)                 \
    X(vkCreateCommandPool)              \
    X(vkResetCommandPool)               \
    X(vkAllocateCommandBuffers)         \
    X(vkBeginCommandBuffer)             \
    X(vkEndCommandBuffer)               \
    X(vkCreateFence)                    \
    X(vkWaitForFences)                  \
    X(vkResetFences)                    \
    X(vkCreateSemaphore)                \
    X(vkDestroySemaphore)               \
    X(vkCreateImage)                    \
    X(vkDestroyImage)                   \
    X(vkCreateImageView)                \
    X(vkDestroyImageView)               \
    X(vkGetImageMemoryRequirements)     \
    X(vkBindImageMemory)                \
    X(vkCreateBuffer)                   \
    X(vkDestroyBuffer)                  \
    X(vkGetBufferMemoryRequirements)    \
    X(vkBindBufferMemory)               \
    X(vkAllocateMemory)                 \
    X(vkMapMemory)                      \
    X(vkFreeMemory)                     \
    X(vkCreateSampler)                  \
    X(vkCreateDescriptorSetLayout)      \
    X(vkCreatePipelineLayout)           \
    X(vkCreateDescriptorPool)           \
    X(vkAllocateDescriptorSets)         \
    X(vkUpdateDescriptorSets)           \
    X(vkCreateShaderModule)             \
    X(vkCreateGraphicsPipelines)            X(vkCreatePipelineCache)                X(vkGetPipelineCacheData)           \
    X(vkCmdBeginRendering)              \
    X(vkCmdEndRendering)                \
    X(vkCmdBindPipeline)                \
    X(vkCmdBindDescriptorSets)          \
    X(vkCmdBindVertexBuffers)           \
    X(vkCmdBindIndexBuffer)             \
    X(vkCmdPushConstants)               \
    X(vkCmdDraw)                        \
    X(vkCmdDrawIndexed)                 \
    X(vkCmdSetViewport)                 \
    X(vkCmdSetScissor)                  \
    X(vkCmdSetCullMode)                 \
    X(vkCmdSetFrontFace)                \
    X(vkCmdSetDepthTestEnable)          \
    X(vkCmdSetDepthWriteEnable)         \
    X(vkCmdSetDepthCompareOp)           \
    X(vkCmdPipelineBarrier2)            \
    X(vkCmdCopyBufferToImage)           \
    X(vkCmdCopyBuffer)                      X(vkCreateQueryPool)                    X(vkCmdResetQueryPool)                  X(vkCmdWriteTimestamp)                  X(vkGetQueryPoolResults)            \
    X(vkCmdBlitImage)                   \
    X(vkCmdCopyImageToBuffer)           \
    X(vkCmdClearColorImage)             \
    X(vkCmdClearDepthStencilImage)

#define DECLARE(name) static PFN_##name name;
VK_GLOBAL_FUNCS(DECLARE)
VK_INSTANCE_FUNCS(DECLARE)
VK_DEVICE_FUNCS(DECLARE)
#undef DECLARE
static PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;

static VkInstance sInstance;
static VkPhysicalDevice sGpu;
static VkDevice sDev;
static VkQueue sQueue;
static uint32_t sQueueFamily;
static VkSurfaceKHR sSurface;
static VkPhysicalDeviceMemoryProperties sMemProps;
static VkPhysicalDeviceLimits sLimits;
/* With RECOMP_FPS=1, the GPU's time for each frame, from timestamps at
 * the start and end of its batch (two queries a slot). */
static VkQueryPool sTimes;
static double sTimeTotal, sTimeWorst;
static unsigned sTimeFrames, sTimeSlow;
#define CHECK(call)                                                                                 \
    do {                                                                                            \
        VkResult r_ = (call);                                                                       \
        if (r_ != VK_SUCCESS) {                                                                     \
            fprintf(stderr, "vulkan: %s failed (%d) at line %d\n", #call, (int)r_, __LINE__);       \
            exit(1);                                                                                \
        }                                                                                           \
    } while (0)

/* ------------------------------------------------------------------------ */
/* Memory: blocks of device memory handed out in pieces */

typedef struct Alloc {
    int block;
    VkDeviceSize off, size;
} Alloc;

typedef struct Range {
    VkDeviceSize off, size;
} Range;

typedef struct Block {
    VkDeviceMemory mem;
    uint32_t type;
    int linear; /* buffers (1) and images (0) keep to blocks of their own (bufferImageGranularity) */
    int own;    /* one allocation's alone (a large one), freed with it */
    VkDeviceSize size;
    uint8_t* map;
    Range* free; /* by offset */
    int nfree, capfree;
} Block;

#define BLOCK_SIZE ((VkDeviceSize)64 << 20) /* images */
#define LINEAR_BLOCK_SIZE ((VkDeviceSize)16 << 20) /* buffers */
static Block* sBlocks;
static int sBlockCount, sBlockCap;

/* The first type with what is wanted; memory the CPU writes is kept out
 * of video memory unless that is wanted too. */
static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want) {
    uint32_t i, pass;
    for (pass = 0; pass < 2; pass++) {
        for (i = 0; i < sMemProps.memoryTypeCount; i++) {
            VkMemoryPropertyFlags f = sMemProps.memoryTypes[i].propertyFlags;
            if (!(bits & (1u << i)) || (f & want) != want) continue;
            if (!pass && (f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) && !(want & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
                continue;
            return i;
        }
    }
    fprintf(stderr, "vulkan: no memory type %08X of %08X\n", (unsigned)want, bits);
    exit(1);
}

static void free_insert(Block* b, int at, VkDeviceSize off, VkDeviceSize size) {
    if (b->nfree == b->capfree) {
        b->capfree = b->capfree ? b->capfree * 2 : 16;
        b->free = (Range*)realloc(b->free, (size_t)b->capfree * sizeof(Range));
    }
    memmove(&b->free[at + 1], &b->free[at], (size_t)(b->nfree - at) * sizeof(Range));
    b->free[at].off = off;
    b->free[at].size = size;
    b->nfree++;
}

static void free_remove(Block* b, int at) {
    memmove(&b->free[at], &b->free[at + 1], (size_t)(b->nfree - at - 1) * sizeof(Range));
    b->nfree--;
}

static int carve(int bi, VkMemoryRequirements* req, Alloc* out) {
    Block* b = &sBlocks[bi];
    int i;
    for (i = 0; i < b->nfree; i++) {
        Range r = b->free[i];
        VkDeviceSize at = (r.off + req->alignment - 1) / req->alignment * req->alignment;
        if (at + req->size > r.off + r.size) continue;
        free_remove(b, i);
        if (at + req->size < r.off + r.size) free_insert(b, i, at + req->size, r.off + r.size - (at + req->size));
        if (at > r.off) free_insert(b, i, r.off, at - r.off);
        out->block = bi;
        out->off = at;
        out->size = req->size;
        return 1;
    }
    return 0;
}

static Alloc mem_alloc(VkMemoryRequirements req, VkMemoryPropertyFlags want, int linear) {
    uint32_t type = mem_type(req.memoryTypeBits, want);
    Alloc a;
    Block* b;
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    int i;
    VkDeviceSize block = linear ? LINEAR_BLOCK_SIZE : BLOCK_SIZE;
    int own = req.size >= block / 4, at = sBlockCount; /* large: an allocation of its own */
    for (i = 0; i < sBlockCount; i++) {
        if (!sBlocks[i].mem) {
            at = i;
            continue;
        }
        if (!own && !sBlocks[i].own && sBlocks[i].type == type && sBlocks[i].linear == linear && carve(i, &req, &a))
            return a;
    }
    if (at == sBlockCount && sBlockCount == sBlockCap) {
        sBlockCap = sBlockCap ? sBlockCap * 2 : 16;
        sBlocks = (Block*)realloc(sBlocks, (size_t)sBlockCap * sizeof(Block));
    }
    b = &sBlocks[at];
    if (at == sBlockCount) memset(b, 0, sizeof(*b));
    b->type = type;
    b->linear = linear;
    b->own = own;
    b->size = own ? req.size : block;
    b->nfree = 0;
    ai.allocationSize = b->size;
    ai.memoryTypeIndex = type;
    CHECK(vkAllocateMemory(sDev, &ai, NULL, &b->mem));
    if (sMemProps.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
        CHECK(vkMapMemory(sDev, b->mem, 0, VK_WHOLE_SIZE, 0, (void**)&b->map));
    free_insert(b, 0, 0, b->size);
    if (at == sBlockCount) sBlockCount++;
    if (!carve(at, &req, &a)) {
        fprintf(stderr, "vulkan: allocation of %llu failed\n", (unsigned long long)req.size);
        exit(1);
    }
    return a;
}

static void mem_free(Alloc a) {
    Block* b = &sBlocks[a.block];
    int i = 0;
    if (b->own) {
        vkFreeMemory(sDev, b->mem, NULL);
        b->mem = VK_NULL_HANDLE;
        b->map = NULL;
        return;
    }
    while (i < b->nfree && b->free[i].off < a.off) i++;
    free_insert(b, i, a.off, a.size);
    if (i + 1 < b->nfree && b->free[i].off + b->free[i].size == b->free[i + 1].off) {
        b->free[i].size += b->free[i + 1].size;
        free_remove(b, i + 1);
    }
    if (i > 0 && b->free[i - 1].off + b->free[i - 1].size == b->free[i].off) {
        b->free[i - 1].size += b->free[i].size;
        free_remove(b, i);
    }
}

/* ------------------------------------------------------------------------ */
/* Buffers, images and what is freed once the GPU is done with it */

typedef struct Buffer {
    VkBuffer buf;
    Alloc mem;
    VkDeviceSize size;
    uint8_t* map; /* host-visible ones */
} Buffer;

typedef struct Image {
    VkImage img;
    VkImageView view;
    Alloc mem;
    VkImageLayout layout;
    VkFormat fmt;
    VkImageAspectFlags aspect;
    int w, h, levels;
} Image;

/* Frames are numbered as they are recorded (sSerial, the one being
 * recorded); sDone is the newest the GPU has finished. */
static uint64_t sSerial = 1, sDone;

enum { DEAD_IMAGE, DEAD_BUFFER, DEAD_HANDLE };
typedef struct Dead {
    uint64_t serial;
    int kind;
    VkImage img;
    VkImageView view;
    VkBuffer buf;
    Alloc mem;
    uint32_t handle;
} Dead;
static Dead* sDead;
static int sDeadCount, sDeadCap;

static Dead* dead(int kind) {
    Dead* d;
    if (sDeadCount == sDeadCap) {
        sDeadCap = sDeadCap ? sDeadCap * 2 : 256;
        sDead = (Dead*)realloc(sDead, (size_t)sDeadCap * sizeof(Dead));
    }
    d = &sDead[sDeadCount++];
    memset(d, 0, sizeof(*d));
    d->serial = sSerial;
    d->kind = kind;
    return d;
}

static Buffer make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props) {
    Buffer b;
    VkBufferCreateInfo ci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    VkMemoryRequirements req;
    memset(&b, 0, sizeof(b));
    ci.size = size;
    ci.usage = usage;
    CHECK(vkCreateBuffer(sDev, &ci, NULL, &b.buf));
    vkGetBufferMemoryRequirements(sDev, b.buf, &req);
    b.mem = mem_alloc(req, props, 1);
    CHECK(vkBindBufferMemory(sDev, b.buf, sBlocks[b.mem.block].mem, b.mem.off));
    b.size = size;
    if (sBlocks[b.mem.block].map) b.map = sBlocks[b.mem.block].map + b.mem.off;
    return b;
}

static void drop_buffer(Buffer* b) {
    Dead* d;
    if (!b->buf) return;
    d = dead(DEAD_BUFFER);
    d->buf = b->buf;
    d->mem = b->mem;
    memset(b, 0, sizeof(*b));
}

/* An image whose view reads its channels as map says. */
static Image make_image_mapped(int w, int h, int levels, VkFormat fmt, VkImageUsageFlags usage,
                               VkSampleCountFlagBits samples, VkComponentMapping map) {
    Image im;
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    VkMemoryRequirements req;
    memset(&im, 0, sizeof(im));
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = fmt;
    ci.extent.width = (uint32_t)(w > 0 ? w : 1);
    ci.extent.height = (uint32_t)(h > 0 ? h : 1);
    ci.extent.depth = 1;
    ci.mipLevels = (uint32_t)levels;
    ci.arrayLayers = 1;
    ci.samples = samples;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    CHECK(vkCreateImage(sDev, &ci, NULL, &im.img));
    vkGetImageMemoryRequirements(sDev, im.img, &req);
    im.mem = mem_alloc(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    CHECK(vkBindImageMemory(sDev, im.img, sBlocks[im.mem.block].mem, im.mem.off));
    im.aspect = fmt == VK_FORMAT_D32_SFLOAT ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    vi.image = im.img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.components = map;
    vi.subresourceRange.aspectMask = im.aspect;
    vi.subresourceRange.levelCount = (uint32_t)levels;
    vi.subresourceRange.layerCount = 1;
    CHECK(vkCreateImageView(sDev, &vi, NULL, &im.view));
    im.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    im.fmt = fmt;
    im.w = (int)ci.extent.width;
    im.h = (int)ci.extent.height;
    im.levels = levels;
    return im;
}

static Image make_image(int w, int h, int levels, VkFormat fmt, VkImageUsageFlags usage, VkSampleCountFlagBits samples) {
    VkComponentMapping same = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                               VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
    return make_image_mapped(w, h, levels, fmt, usage, samples, same);
}

static void drop_image(Image* im) {
    Dead* d;
    if (!im->img) return;
    d = dead(DEAD_IMAGE);
    d->img = im->img;
    d->view = im->view;
    d->mem = im->mem;
    memset(im, 0, sizeof(*im));
}

/* Moves an image to a layout. Every transition waits for everything
 * before it: simple, and the few per frame don't cost much. */
static void transition(VkCommandBuffer cb, Image* im, VkImageLayout to) {
    VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    VkDependencyInfo d = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.oldLayout = im->layout;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = im->img;
    b.subresourceRange.aspectMask = im->aspect;
    b.subresourceRange.levelCount = (uint32_t)im->levels;
    b.subresourceRange.layerCount = 1;
    d.imageMemoryBarrierCount = 1;
    d.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cb, &d);
    im->layout = to;
}

/* Everything written before (copies into buffers) visible to everything after. */
static void memory_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier2 b = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    VkDependencyInfo d = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    d.memoryBarrierCount = 1;
    d.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cb, &d);
}

/* ------------------------------------------------------------------------ */
/* Textures: numbered places in the shaders' array */

#define MAX_TEXTURES 16384 /* as the shaders declare */
enum { TEX_WHITE, TEX_EFB, TEX_MENU, TEX_MAP, TEX_LIST, TEX_FIRST };

typedef struct Tex {
    Image im;
    int pix; /* the layout of its pixels (GX_PIX_...) */
    VkSampler sampler;
    uint64_t used; /* the frame that last drew with it */
} Tex;
static Tex sTex[MAX_TEXTURES];
static uint32_t sFreeHandles[MAX_TEXTURES];
static int sFreeCount;

static VkDescriptorSetLayout sTexLayout, sDataLayout;
static VkDescriptorPool sPool;
static VkDescriptorSet sTexSet;
static VkPipelineLayout sLayout;

static void set_texture(uint32_t handle) {
    VkDescriptorImageInfo ii;
    VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    ii.sampler = sTex[handle].sampler;
    ii.imageView = sTex[handle].im.view;
    ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    w.dstSet = sTexSet;
    w.dstBinding = 0;
    w.dstArrayElement = handle;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(sDev, 1, &w, 0, NULL);
}

static uint32_t new_handle(void) {
    if (!sFreeCount) {
        fprintf(stderr, "vulkan: more than %d textures\n", MAX_TEXTURES);
        exit(1);
    }
    return sFreeHandles[--sFreeCount];
}

/* Samplers, one for each way of sampling. */
typedef struct SamplerKey {
    uint32_t mode0;
    int levels1, aniso;
} SamplerKey;
static struct {
    SamplerKey key;
    VkSampler s;
} sSamplers[256];
static int sSamplerCount;
static float sMaxAniso;
static VkBool32 sBc; /* whether BC1 textures can be made (vk_texture_pixels) */

static VkSamplerAddressMode wrap_mode(int w) {
    return w == 0 ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE
                  : (w == 1 ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT);
}

/* Sampled as the game set it, as gl_backend.c's textures are: TX_SETMODE0's
 * wrap, filters and LOD bias, mipmaps of levels, anisotropic filtering as
 * the setting says on what is filtered linearly. */
static VkSampler sampler(uint32_t mode0, int levels) {
    VkSamplerCreateInfo ci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    SamplerKey k;
    int mip = (mode0 >> 5) & 3, min_linear = (mode0 >> 7) & 1, i;
    if (mip > 2) mip = 2;
    if (levels <= 1) mip = 0;
    memset(&k, 0, sizeof(k));
    k.mode0 = mode0 & (mip ? 0x1FFFF : 0x9F); /* LOD bias only with mipmaps */
    k.levels1 = mip ? levels : 1;
    k.aniso = sMaxAniso > 1 && g_settings.aniso > 1 && min_linear ? g_settings.aniso : 0;
    for (i = 0; i < sSamplerCount; i++) {
        if (memcmp(&sSamplers[i].key, &k, sizeof(k)) == 0) return sSamplers[i].s;
    }
    ci.magFilter = (mode0 >> 4) & 1 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.minFilter = min_linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.mipmapMode = mip == 2 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.addressModeU = wrap_mode(mode0 & 3);
    ci.addressModeV = wrap_mode((mode0 >> 2) & 3);
    ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ci.mipLodBias = mip ? (float)(int8_t)((mode0 >> 9) & 0xFF) / 32.0f : 0.0f;
    ci.minLod = 0;
    ci.maxLod = mip ? (float)(levels - 1) : 0.0f;
    if (k.aniso) {
        ci.anisotropyEnable = VK_TRUE;
        ci.maxAnisotropy = (float)k.aniso < sMaxAniso ? (float)k.aniso : sMaxAniso;
    }
    if (sSamplerCount == 256) {
        fprintf(stderr, "vulkan: too many samplers\n");
        exit(1);
    }
    sSamplers[sSamplerCount].key = k;
    CHECK(vkCreateSampler(sDev, &ci, NULL, &sSamplers[sSamplerCount].s));
    return sSamplers[sSamplerCount++].s;
}

static VkSampler sLinearClamp;

/* ------------------------------------------------------------------------ */
/* Frames in flight */

/* On Android two frames are let be in flight, not one: with one, the CPU
 * and GPU took turns, the GPU never looked busy enough for its clock to be
 * raised, and the game's thread waited for it a fifth of the time at 120
 * fps (90 fps where 120 now holds, on an Adreno 830). */
#ifdef __ANDROID__
#define SLOTS 3
#else
#define SLOTS 2
#endif
#define STAGING_SIZE ((VkDeviceSize)4 << 20)
typedef struct Slot {
    VkCommandPool pool;
    VkCommandBuffer up, cb, out; /* uploads, then the frame, then its copy to the swapchain's image */
    VkFence fence;
    VkSemaphore acquired, acquired2; /* the swapchains' images, the main one's and the second screen's */
    uint64_t serial; /* the frame last submitted from here */
    /* Rings, filled from the start each time the slot is used: draw data
     * (XF and TEV state, read by the shaders), vertices and indices of
     * draws as they come, and uploads. */
    Buffer data, verts, idx, staging;
    VkDeviceSize data_used, verts_used, idx_used, staging_used;
    VkDeviceSize staging_peak; /* the most staging a frame used lately */
    unsigned frames;
    VkDescriptorSet set; /* the data buffer, for the shaders */
} Slot;
static Slot sSlots[SLOTS];
static Slot* S; /* the one being recorded */
static int sSlot;

enum { RING_DATA, RING_VERTS, RING_IDX, RING_STAGING };

/* What the frame's commands have set, so a draw sets only what differs. */
static struct {
    VkPipeline pipe;
    int vp_valid, sc_valid, cull, dtest, dwrite, dfunc;
    VkViewport vp;
    VkRect2D sc;
    VkBuffer vb, ib;
    uint32_t push[12];
    int push_valid;
} sCmd;

static void forget(void) {
    memset(&sCmd, 0, sizeof(sCmd));
    sCmd.cull = sCmd.dtest = sCmd.dwrite = sCmd.dfunc = -1;
}

/* The XF state as the vertex shader reads it (gx.vert): memory 0x000-0x0FF,
 * 0x400-0x47F, 0x500-0x67F, the registers, then the texture coordinates'
 * setup as texgen_program makes it. */
#define XF_PACKED (0x360 + 8 + 8 * 12)
/* The TEV state as the fragment shader reads it (gx.frag). */
#define TEV_WORDS (160 + 24 * 16)
/* Where uniform buffers may start (the device's limit). */
static VkDeviceSize sUboAlign = 256;

/* The frame's data buffer, as the shaders read it: whole, and from each
 * draw's XF and TEV states on, as uniform buffers: the shaders read those
 * for every vertex and pixel, which a phone's GPU does far faster from a
 * uniform buffer (kept in its constant registers) than from a storage one. */
static void data_set(void) {
    VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    VkDescriptorBufferInfo bi, ui, ti;
    VkWriteDescriptorSet w[3] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET},
                                 {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
    ai.descriptorPool = sPool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &sDataLayout;
    CHECK(vkAllocateDescriptorSets(sDev, &ai, &S->set));
    bi.buffer = ui.buffer = ti.buffer = S->data.buf;
    bi.offset = ui.offset = ti.offset = 0;
    bi.range = VK_WHOLE_SIZE;
    ui.range = XF_PACKED * 4;
    ti.range = TEV_WORDS * 4;
    w[0].dstSet = w[1].dstSet = w[2].dstSet = S->set;
    w[0].descriptorCount = w[1].descriptorCount = w[2].descriptorCount = 1;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w[0].pBufferInfo = &bi;
    w[1].dstBinding = 1;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    w[1].pBufferInfo = &ui;
    w[2].dstBinding = 2;
    w[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    w[2].pBufferInfo = &ti;
    vkUpdateDescriptorSets(sDev, 3, w, 0, NULL);
}

static int64_t sXfAt = -1, sTevAt = -1; /* where the last draw's XF and TEV state were written, in words */
static VkDeviceSize sBound[2]; /* the XF and TEV states' offsets the data set is bound with, in bytes */

static void bind_sets(void) {
    VkDescriptorSet sets[2];
    uint32_t off[2] = {(uint32_t)sBound[0], (uint32_t)sBound[1]};
    sets[0] = sTexSet;
    sets[1] = S->set;
    vkCmdBindDescriptorSets(S->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, sLayout, 0, 2, sets, 2, off);
}

/* The data set bound with the XF and TEV states at byte offsets xf and tev. */
static void bind_data(VkDeviceSize xf, VkDeviceSize tev) {
    uint32_t o[2] = {(uint32_t)xf, (uint32_t)tev};
    if (xf == sBound[0] && tev == sBound[1]) return;
    sBound[0] = xf;
    sBound[1] = tev;
    vkCmdBindDescriptorSets(S->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, sLayout, 1, 1, &S->set, 2, o);
}

static void* ring(int which, VkDeviceSize bytes, VkDeviceSize align, VkDeviceSize* off) {
    static const VkBufferUsageFlags kUsage[4] = {
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
    Buffer* b = which == RING_DATA ? &S->data : which == RING_VERTS ? &S->verts : which == RING_IDX ? &S->idx : &S->staging;
    VkDeviceSize* used = which == RING_DATA     ? &S->data_used
                         : which == RING_VERTS  ? &S->verts_used
                         : which == RING_IDX    ? &S->idx_used
                                                : &S->staging_used;
    VkDeviceSize at = (*used + align - 1) / align * align;
    if (at + bytes > b->size) {
        /* Full: a bigger one from here on (the old one is freed when the
         * frame is done). */
        VkDeviceSize size = b->size * 2;
        while (size < bytes * 2) size *= 2;
        drop_buffer(b);
        *b = make_buffer(size, kUsage[which],
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        at = 0;
        if (which == RING_DATA) {
            data_set();
            sBound[0] = sBound[1] = 0;
            bind_sets();
            sXfAt = sTevAt = -1;
        } else if (which == RING_VERTS) sCmd.vb = VK_NULL_HANDLE;
        else if (which == RING_IDX) sCmd.ib = VK_NULL_HANDLE;
    }
    *used = at + bytes;
    *off = at;
    return b->map + at;
}

/* Room for an upload: in the staging ring, or, for a burst more than it
 * holds (loading), a buffer of its own for this frame, so the ring stays
 * the size frames usually need. */
static void* staging(VkDeviceSize bytes, VkBuffer* buf, VkDeviceSize* off) {
    VkDeviceSize at = (S->staging_used + 15) / 16 * 16;
    if (at + bytes > S->staging.size && bytes > S->staging.size / 4) {
        Buffer once = make_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        uint8_t* map = once.map;
        *buf = once.buf;
        *off = 0;
        drop_buffer(&once); /* freed once the frame is done */
        return map;
    }
    {
        void* p = ring(RING_STAGING, bytes, 16, off);
        *buf = S->staging.buf;
        return p;
    }
}

/* ------------------------------------------------------------------------ */
/* Pipelines */

/* gx.vert and gx.frag by TEX_IO. */
static VkShaderModule sGxVert[9], sGxFrag[9], sQuadVert, sFillFrag, sCopyFrag, sBlitFrag;
static VkPipelineCache sCache = VK_NULL_HANDLE;

enum { PK_GX, PK_FILL, PK_COPY, PK_BLIT };
static int sMsaa = 1, sMaxSamples = 1;

typedef struct Pipe {
    int kind, samples;
    uint32_t key;
    VkPipeline p;
} Pipe;
static Pipe* sPipes;
static int sPipeCount, sPipeCap;

/* A TEV setup as gx.frag's SPEC_ constants: stages, swap tables, then
 * three words a stage (spec_words). */
#define SPEC_WORDS (2 + 3 * 16)
/* A transform setup as gx.vert's VS_ constants (vs_words): whether it is
 * one, the four lighting channel controls, which matrix indices the
 * vertices carry and whether texgen_program's setup holds; then the
 * texture coordinates passed to gx.frag (its and gx.vert's TEX_IO, of
 * the shaders' builds). */
#define VS_WORDS 8
#define VS_TEX_IO 7
#define VS_CONST 64 /* gx.vert's first */
static volatile int sCacheDirty; /* pipelines made since vk_pipelines.bin was written */

/* The push constants of quads (clears, copies, presenting). */
typedef struct Quad {
    float src[4];
    float color[4];
    int32_t mode[4];
    float depth;
    uint32_t tex;
} Quad;

static VkShaderModule shader(const uint32_t* code, size_t bytes) {
    VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    VkShaderModule m;
    ci.codeSize = bytes;
    ci.pCode = code;
    CHECK(vkCreateShaderModule(sDev, &ci, NULL, &m));
    return m;
}

/* GX keys: color mask (bits 0-3, R G B A), blending (4), source and
 * destination factors (5-9, 10-14), reverse subtract (15). Fill keys:
 * the color mask. Blit keys: blending (alpha over). */
/* Makes a pipeline; spec, for PK_GX, a TEV setup's words (spec_words),
 * or NULL for the general shader. Called on the pipeline thread too. */
static VkPipeline make_pipeline(int kind, uint32_t key, int samples, const uint32_t* spec, const uint32_t* vs) {
    static const VkDynamicState kDynamic[] = {
        VK_DYNAMIC_STATE_VIEWPORT,          VK_DYNAMIC_STATE_SCISSOR,           VK_DYNAMIC_STATE_CULL_MODE,
        VK_DYNAMIC_STATE_FRONT_FACE,        VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_COMPARE_OP};
    int i;
    VkPipelineShaderStageCreateInfo st[2];
    VkVertexInputBindingDescription bind;
    VkVertexInputAttributeDescription attr[11];
    VkPipelineVertexInputStateCreateInfo vin = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState cba;
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    VkPipelineDynamicStateCreateInfo dyn = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    VkFormat color = VK_FORMAT_R8G8B8A8_UNORM;
    /* gx.frag's constants: ALPHA_TEST, then the setup's (SPEC_STAGES on). */
    uint32_t consts[1 + SPEC_WORDS];
    VkSpecializationMapEntry entries[1 + SPEC_WORDS];
    VkSpecializationInfo si = {1, entries, sizeof(uint32_t), consts};
    VkSpecializationMapEntry ventries[VS_WORDS];
    VkSpecializationInfo vsi = {VS_TEX_IO, ventries, VS_TEX_IO * sizeof(uint32_t), vs};
    /* All the coordinates for the general shaders. */
    int io = spec && vs ? (int)vs[VS_TEX_IO] : 8;
    VkPipeline out;
    consts[0] = !(key & (1u << 16));
    if (spec) {
        memcpy(consts + 1, spec, SPEC_WORDS * sizeof(uint32_t));
        si.mapEntryCount = 3 + 3 * spec[0];
        si.dataSize = si.mapEntryCount * sizeof(uint32_t);
    }
    for (i = 0; i < (int)si.mapEntryCount; i++) {
        entries[i].constantID = (uint32_t)i;
        entries[i].offset = (uint32_t)(i * sizeof(uint32_t));
        entries[i].size = sizeof(uint32_t);
    }
    memset(st, 0, sizeof(st));
    for (i = 0; i < 2; i++) {
        st[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        st[i].pName = "main";
    }
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    st[0].module = kind == PK_GX ? sGxVert[io] : sQuadVert;
    st[1].module = kind == PK_GX ? sGxFrag[io] : kind == PK_FILL ? sFillFrag : kind == PK_COPY ? sCopyFrag : sBlitFrag;
    if (kind == PK_GX) {
        st[1].pSpecializationInfo = &si;
        if (vs) {
            for (i = 0; i < VS_TEX_IO; i++) {
                ventries[i].constantID = (uint32_t)(VS_CONST + i);
                ventries[i].offset = (uint32_t)(i * sizeof(uint32_t));
                ventries[i].size = sizeof(uint32_t);
            }
            st[0].pSpecializationInfo = &vsi;
        }
        bind.binding = 0;
        bind.stride = sizeof(GXVertex);
        bind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        memset(attr, 0, sizeof(attr));
        for (i = 0; i < 11; i++) attr[i].location = (uint32_t)i;
        for (i = 0; i < 3; i++) {
            attr[i].format = VK_FORMAT_R8G8B8A8_UINT;
            attr[i].offset = (uint32_t)(offsetof(GXVertex, mtx) + i * 4);
        }
        attr[3].format = attr[4].format = VK_FORMAT_R32G32B32_SFLOAT;
        attr[3].offset = offsetof(GXVertex, pos);
        attr[4].offset = offsetof(GXVertex, nrm);
        attr[5].format = attr[6].format = VK_FORMAT_R8G8B8A8_UNORM;
        attr[5].offset = offsetof(GXVertex, color[0]);
        attr[6].offset = offsetof(GXVertex, color[1]);
        for (i = 0; i < 4; i++) {
            attr[7 + i].format = VK_FORMAT_R32G32B32A32_SFLOAT;
            attr[7 + i].offset = (uint32_t)(offsetof(GXVertex, tex) + i * 4 * sizeof(float));
        }
        vin.vertexBindingDescriptionCount = 1;
        vin.pVertexBindingDescriptions = &bind;
        vin.vertexAttributeDescriptionCount = 11;
        vin.pVertexAttributeDescriptions = attr;
    }
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    vp.viewportCount = vp.scissorCount = 1;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.lineWidth = 1;
    ms.rasterizationSamples = (VkSampleCountFlagBits)samples;
    ds.depthCompareOp = VK_COMPARE_OP_ALWAYS;
    memset(&cba, 0, sizeof(cba));
    cba.colorWriteMask = kind == PK_GX || kind == PK_FILL ? key & 15 : 15;
    if (kind == PK_GX && (key & 16)) {
        cba.blendEnable = VK_TRUE;
        cba.srcColorBlendFactor = cba.srcAlphaBlendFactor = (VkBlendFactor)((key >> 5) & 31);
        cba.dstColorBlendFactor = cba.dstAlphaBlendFactor = (VkBlendFactor)((key >> 10) & 31);
        cba.colorBlendOp = cba.alphaBlendOp = (key >> 15) & 1 ? VK_BLEND_OP_REVERSE_SUBTRACT : VK_BLEND_OP_ADD;
    } else if (kind == PK_BLIT && key) {
        cba.blendEnable = VK_TRUE;
        cba.srcColorBlendFactor = cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        cba.dstColorBlendFactor = cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cba.colorBlendOp = cba.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;
    dyn.dynamicStateCount = sizeof(kDynamic) / sizeof(kDynamic[0]);
    dyn.pDynamicStates = kDynamic;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachmentFormats = &color;
    ri.depthAttachmentFormat = kind == PK_GX || kind == PK_FILL ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_UNDEFINED;
    ci.pNext = &ri;
    ci.stageCount = 2;
    ci.pStages = st;
    ci.pVertexInputState = &vin;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pDepthStencilState = &ds;
    ci.pColorBlendState = &cb;
    ci.pDynamicState = &dyn;
    ci.layout = sLayout;
    CHECK(vkCreateGraphicsPipelines(sDev, sCache, 1, &ci, NULL, &out));
    return out;
}

static VkPipeline pipeline(int kind, uint32_t key) {
    int samples = kind == PK_GX || kind == PK_FILL ? sMsaa : 1, i;
    Pipe* p;
    for (i = 0; i < sPipeCount; i++) {
        if (sPipes[i].kind == kind && sPipes[i].key == key && sPipes[i].samples == samples) return sPipes[i].p;
    }
    if (sPipeCount == sPipeCap) {
        sPipeCap = sPipeCap ? sPipeCap * 2 : 64;
        sPipes = (Pipe*)realloc(sPipes, (size_t)sPipeCap * sizeof(Pipe));
    }
    p = &sPipes[sPipeCount++];
    p->kind = kind;
    p->key = key;
    p->samples = samples;
    p->p = make_pipeline(kind, key, samples, NULL, NULL);
    sCacheDirty = 1;
    return p->p;
}

static void use_pipeline(VkPipeline p) {
    if (sCmd.pipe != p) {
        vkCmdBindPipeline(S->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
        sCmd.pipe = p;
    }
}

static void set_viewport(float x, float y, float w, float h, float zmin, float zmax) {
    VkViewport v;
    v.x = x;
    v.y = y;
    v.width = w;
    v.height = h;
    v.minDepth = zmin;
    v.maxDepth = zmax;
    if (!sCmd.vp_valid || memcmp(&v, &sCmd.vp, sizeof(v)) != 0) {
        vkCmdSetViewport(S->cb, 0, 1, &v);
        sCmd.vp = v;
        sCmd.vp_valid = 1;
    }
}

static void set_scissor(int x, int y, int w, int h) {
    VkRect2D r;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    r.offset.x = x;
    r.offset.y = y;
    r.extent.width = (uint32_t)(w > 0 ? w : 0);
    r.extent.height = (uint32_t)(h > 0 ? h : 0);
    if (!sCmd.sc_valid || memcmp(&r, &sCmd.sc, sizeof(r)) != 0) {
        vkCmdSetScissor(S->cb, 0, 1, &r);
        sCmd.sc = r;
        sCmd.sc_valid = 1;
    }
}

static void set_depth(int test, int write, int func) {
    if (sCmd.dtest != test) vkCmdSetDepthTestEnable(S->cb, (VkBool32)test);
    if (sCmd.dwrite != write) vkCmdSetDepthWriteEnable(S->cb, (VkBool32)write);
    if (sCmd.dfunc != func) vkCmdSetDepthCompareOp(S->cb, (VkCompareOp)func);
    sCmd.dtest = test;
    sCmd.dwrite = write;
    sCmd.dfunc = func;
}

static void set_cull(int cull) {
    if (sCmd.cull != cull) vkCmdSetCullMode(S->cb, (VkCullModeFlags)cull);
    sCmd.cull = cull;
}

static void push(const void* data, uint32_t bytes) {
    if (bytes == sizeof(sCmd.push) && sCmd.push_valid && memcmp(sCmd.push, data, bytes) == 0) return;
    vkCmdPushConstants(S->cb, sLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, bytes, data);
    if (bytes == sizeof(sCmd.push)) {
        memcpy(sCmd.push, data, bytes);
        sCmd.push_valid = 1;
    } else sCmd.push_valid = 0;
}

static void quad(const Quad* q) {
    push(q, sizeof(*q));
    vkCmdDraw(S->cb, 3, 1, 0, 0);
}

/* ------------------------------------------------------------------------ */
/* The EFB */

static Image sEfb, sEfbDepth, sMsColor, sMsDepth; /* the multisampled ones with anti-aliasing */
static double sScaleX = 2, sScaleY = 2, sPixelAspect = 1;
static int sScale, sRendering;
/* The canvas's margins (gx_canvas), as the EFB was made for: positions
 * on it are in EFB pixels from its top left corner (gx_canvas_draw,
 * gx_canvas_rect). */
static int sMX, sMY;
#define VX(x) ((int)floor((x) * sScaleX + 0.5))
#define VY(y) ((int)floor((y) * sScaleY + 0.5))

/* Clears not made yet (vk_clear): made as the next drawing starts, as its
 * tiles are set up, rather than drawn or loaded from memory and drawn over
 * (with anti-aliasing several times the picture's size to read); and on
 * the resolved picture alone (efb_settle) for a copy or presenting before
 * that. */
static int sClearColor, sClearDepth;
static VkClearColorValue sClearColorTo;
static float sClearDepthTo;
static int sClearShown; /* sEfb shows the color clear */

/* Whether the drawing has the depth buffer: a drawing started after a copy
 * leaves it in memory, untouched, until a draw tests depth (most drawn
 * there, as a rule, are the picture's 2D parts); reading it in costs as
 * much as a fifth of the frame on a phone. Without VK_EXT_load_store_op_none
 * it is always read in. */
static int sDepthIn, sLoadNone;

static void begin_pass(int depth_used) {
    VkRenderingAttachmentInfo ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkRenderingAttachmentInfo da = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    if (sEfb.layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
        transition(S->cb, &sEfb, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    ca.loadOp = da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    ca.storeOp = da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    if (sClearColor) {
        ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        ca.clearValue.color = sClearColorTo;
    }
    if (sClearDepth) {
        da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        da.clearValue.depthStencil.depth = sClearDepthTo;
    } else if (!depth_used && sLoadNone) {
        da.loadOp = VK_ATTACHMENT_LOAD_OP_NONE_EXT;
        da.storeOp = VK_ATTACHMENT_STORE_OP_NONE;
    }
    sDepthIn = da.loadOp != VK_ATTACHMENT_LOAD_OP_NONE_EXT;
    sClearColor = sClearDepth = sClearShown = 0;
    if (sMsaa > 1) {
        /* Resolved into sEfb as the drawing ends, for copies and presenting. */
        ca.imageView = sMsColor.view;
        ca.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
        ca.resolveImageView = sEfb.view;
        ca.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        da.imageView = sMsDepth.view;
    } else {
        ca.imageView = sEfb.view;
        da.imageView = sEfbDepth.view;
    }
    ri.renderArea.extent.width = (uint32_t)sEfb.w;
    ri.renderArea.extent.height = (uint32_t)sEfb.h;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    ri.pDepthAttachment = &da;
    vkCmdBeginRendering(S->cb, &ri);
    sRendering = 1;
}

static void end_efb(void);

/* Draws to the EFB from here on, with its depth buffer if depth_used. */
static void begin_efb(int depth_used) {
    if (sRendering) {
        if (!depth_used || sDepthIn) return;
        end_efb(); /* and again, with it */
    }
    begin_pass(depth_used);
}

/* The resolved picture (sEfb) as the EFB is, with a color clear not made
 * yet: for copying or presenting it. Outside drawing. */
static void efb_settle(void) {
    VkImageSubresourceRange r;
    if (!sClearColor || sClearShown) return;
    memset(&r, 0, sizeof(r));
    r.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    r.levelCount = 1;
    r.layerCount = 1;
    transition(S->cb, &sEfb, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdClearColorImage(S->cb, sEfb.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &sClearColorTo, 1, &r);
    sClearShown = 1;
}

static void end_efb(void) {
    if (!sRendering) return;
    vkCmdEndRendering(S->cb);
    sRendering = 0;
}

/* Clears an image as it is made: black, depth at the far plane (0, reversed). */
static void clear_image(Image* im) {
    VkImageSubresourceRange r;
    memset(&r, 0, sizeof(r));
    r.aspectMask = im->aspect;
    r.levelCount = 1;
    r.layerCount = 1;
    transition(S->cb, im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    if (im->aspect == VK_IMAGE_ASPECT_DEPTH_BIT) {
        VkClearDepthStencilValue d = {0.0f, 0};
        vkCmdClearDepthStencilImage(S->cb, im->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &d, 1, &r);
        transition(S->cb, im, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
    } else {
        VkClearColorValue c;
        memset(&c, 0, sizeof(c));
        vkCmdClearColorImage(S->cb, im->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &r);
        transition(S->cb, im, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    }
}

/* EFB copies' textures, by address and size. */
typedef struct CopyTarget {
    uint32_t addr;
    int w, h;
    uint32_t handle;
} CopyTarget;
static CopyTarget sTargets[64];
static int sTargetCount;

static void make_target_image(CopyTarget* t) {
    Tex* x = &sTex[t->handle];
    int cx = 0, cy = 0, cw = t->w, ch = t->h; /* a copy of the whole picture has the canvas's margins */
    gx_canvas_rect(sMX, sMY, &cx, &cy, &cw, &ch);
    x->im = make_image(VX(cw), VY(ch), 1, VK_FORMAT_R8G8B8A8_UNORM,
                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                       VK_SAMPLE_COUNT_1_BIT);
    x->sampler = sLinearClamp;
    transition(S->cb, &x->im, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    set_texture(t->handle);
}

/* (Re)makes the EFB at the current scale and sample count, and the copy
 * textures at the matching scale. What they held is lost; the game
 * redraws it. Nothing in flight uses them (the caller waits). */
static void alloc_efb(void) {
    int w = VX(EFB_W + 2 * sMX), h = VY(EFB_H + 2 * sMY), i;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                              VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    end_efb();
    sClearColor = sClearDepth = 0;
    drop_image(&sEfb);
    drop_image(&sEfbDepth);
    drop_image(&sMsColor);
    drop_image(&sMsDepth);
    sEfb = make_image(w, h, 1, VK_FORMAT_R8G8B8A8_UNORM, usage, VK_SAMPLE_COUNT_1_BIT);
    clear_image(&sEfb);
    if (sMsaa > 1) {
        sMsColor = make_image(w, h, 1, VK_FORMAT_R8G8B8A8_UNORM,
                              VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                              (VkSampleCountFlagBits)sMsaa);
        sMsDepth = make_image(w, h, 1, VK_FORMAT_D32_SFLOAT,
                              VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                              (VkSampleCountFlagBits)sMsaa);
        clear_image(&sMsColor);
        clear_image(&sMsDepth);
    } else {
        sEfbDepth = make_image(w, h, 1, VK_FORMAT_D32_SFLOAT,
                               VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                               VK_SAMPLE_COUNT_1_BIT);
        clear_image(&sEfbDepth);
    }
    sTex[TEX_EFB].im = sEfb; /* a copy of the handles: sEfb's layout is the one kept */
    sTex[TEX_EFB].sampler = sLinearClamp;
    set_texture(TEX_EFB);
    for (i = 0; i < sTargetCount; i++) {
        drop_image(&sTex[sTargets[i].handle].im);
        make_target_image(&sTargets[i]);
    }
}

/* ------------------------------------------------------------------------ */
/* The window: the picture is drawn into sWin, at the client area's size,
 * then copied to the swapchain's image */

static Image sWin;
static VkSwapchainKHR sChain;
static VkImage sChainImages[8];
static VkSemaphore sRendered[8];
static uint32_t sChainCount;
static int sChainW, sChainH, sChainStale;
/* The second screen's (Android): its window's surface and swapchain, and
 * the image of it copied to this frame, if one was (sAcquired2). */
static VkSurfaceKHR sSurface2;
static VkSwapchainKHR sChain2;
static VkImage sChain2Images[8];
static VkSemaphore sRendered2[8];
static uint32_t sChain2Count, sImage2;
static int sChain2W, sChain2H, sChain2Stale, sAcquired2;
/* Drawn for it (draw_bottom): its picture, and the minimap's. */
static int sBottomW, sBottomH; /* sBottom's size; 0 without a second screen */
static int sMapSamples, sBottomDumped;
static Image sBottom, sMap, sMapMs, sMapDepth;
static Buffer sReadback2; /* sBottom, for RECOMP_GX_DUMP */

/* On Android the swapchain is made unrotated and the compositor turns it
 * for a landscape window (make_swapchain), and every acquire and present
 * then says VK_SUBOPTIMAL_KHR: the picture isn't pre-rotated. Remaking
 * the swapchain for that, as elsewhere, remade it every frame (a wait for
 * the GPU to go idle and calls to the compositor each time), a third of
 * the game's thread at 120 fps; there it's remade only when out of date
 * or the window's size changes. */
#ifdef __ANDROID__
#define SUBOPTIMAL_IS_EXPECTED 1
#else
#define SUBOPTIMAL_IS_EXPECTED 0
#endif

/* On Android vkQueuePresentKHR waits a while in the driver (2 to 3 ms a
 * frame on an Adreno 830, a fifth of the game's thread at 120 fps), so
 * there it's called on a thread of its own. The queue and the swapchain
 * are used by one thread at a time: the game's waits for the last present
 * to be done (present_done) before it uses either again, which by then
 * it nearly always is. */
#ifdef __ANDROID__
static pthread_mutex_t sPresentLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sPresentWake = PTHREAD_COND_INITIALIZER;
static int sPresentStarted, sPresentPending;
static uint32_t sPresentImage, sPresentImage2; /* the second ~0u if none */

static void* present_thread(void* arg) {
    (void)arg;
    pthread_mutex_lock(&sPresentLock);
    for (;;) {
        VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        VkResult r;
        while (!sPresentPending) pthread_cond_wait(&sPresentWake, &sPresentLock);
        pthread_mutex_unlock(&sPresentLock);
        VkSwapchainKHR chains[2];
        VkSemaphore waits[2];
        uint32_t images[2];
        VkResult results[2] = {VK_SUCCESS, VK_SUCCESS};
        chains[0] = sChain;
        waits[0] = sRendered[sPresentImage];
        images[0] = sPresentImage;
        pi.swapchainCount = 1;
        if (sPresentImage2 != ~0u) {
            chains[1] = sChain2;
            waits[1] = sRendered2[sPresentImage2];
            images[1] = sPresentImage2;
            pi.swapchainCount = 2;
        }
        pi.waitSemaphoreCount = pi.swapchainCount;
        pi.pWaitSemaphores = waits;
        pi.pSwapchains = chains;
        pi.pImageIndices = images;
        pi.pResults = results;
        r = vkQueuePresentKHR(sQueue, &pi);
        (void)r;
        pthread_mutex_lock(&sPresentLock);
        if (results[0] == VK_ERROR_OUT_OF_DATE_KHR) sChainStale = 1;
        if (pi.swapchainCount == 2 && results[1] < 0) sChain2Stale = 1;
        sPresentPending = 0;
        pthread_cond_broadcast(&sPresentWake);
    }
    return NULL;
}

static void present_done(void) {
    pthread_mutex_lock(&sPresentLock);
    while (sPresentPending) pthread_cond_wait(&sPresentWake, &sPresentLock);
    pthread_mutex_unlock(&sPresentLock);
}

static void present_later(uint32_t image, uint32_t image2) {
    if (!sPresentStarted) {
        pthread_t t;
        pthread_create(&t, NULL, present_thread, NULL);
        pthread_detach(t);
        sPresentStarted = 1;
    }
    pthread_mutex_lock(&sPresentLock);
    sPresentImage = image;
    sPresentImage2 = image2;
    sPresentPending = 1;
    pthread_cond_broadcast(&sPresentWake);
    pthread_mutex_unlock(&sPresentLock);
}
#else
static void present_done(void) {}
#endif

static void wait_idle(void) {
    present_done();
    vkDeviceWaitIdle(sDev);
}
static VkPresentModeKHR sPresentMode;
static Buffer sReadback; /* RECOMP_GX_DUMP */
static const char* sDumpDir;
static int sDumpEvery = 30;
static unsigned sDumpFrom, sPresented;

static void make_swapchain(int w, int h) {
    VkSurfaceCapabilitiesKHR caps;
    VkSurfaceFormatKHR formats[64];
    uint32_t nf = 64, i, count;
    VkSwapchainCreateInfoKHR ci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    VkSwapchainKHR old = sChain;
    VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    wait_idle();
    CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(sGpu, sSurface, &caps));
    vkGetPhysicalDeviceSurfaceFormatsKHR(sGpu, sSurface, &nf, formats);
    ci.surface = sSurface;
    count = caps.minImageCount < 3 ? 3 : caps.minImageCount;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
    if (count > 8) count = 8;
    ci.minImageCount = count;
    ci.imageFormat = formats[0].format;
    ci.imageColorSpace = formats[0].colorSpace;
    for (i = 0; i < nf; i++) {
        if (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM || formats[i].format == VK_FORMAT_R8G8B8A8_UNORM) {
            ci.imageFormat = formats[i].format;
            ci.imageColorSpace = formats[i].colorSpace;
            break;
        }
    }
#ifdef __ANDROID__
    /* Android gives the display's own (unrotated) size; at the window's,
     * unrotated, the compositor turns the picture for a landscape window. */
    ci.imageExtent.width = (uint32_t)w;
    ci.imageExtent.height = (uint32_t)h;
#else
    ci.imageExtent.width = caps.currentExtent.width != 0xFFFFFFFFu ? caps.currentExtent.width : (uint32_t)w;
    ci.imageExtent.height = caps.currentExtent.height != 0xFFFFFFFFu ? caps.currentExtent.height : (uint32_t)h;
#endif
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
#ifdef __ANDROID__
    ci.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
                                                                                       : caps.currentTransform;
#else
    ci.preTransform = caps.currentTransform;
#endif
    /* Opaque where it can be (Android's surfaces often take only inherit). */
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)) {
        uint32_t a;
        for (a = VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR; a < VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR; a <<= 1) {
            if (caps.supportedCompositeAlpha & a) break;
        }
        ci.compositeAlpha = (VkCompositeAlphaFlagBitsKHR)a;
    }
    ci.presentMode = sPresentMode;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = old;
    sChain = VK_NULL_HANDLE;
    sChainW = sChainH = 0;
    if (ci.imageExtent.width && ci.imageExtent.height) {
        if (vkCreateSwapchainKHR(sDev, &ci, NULL, &sChain) != VK_SUCCESS) sChain = VK_NULL_HANDLE;
    }
    if (old) vkDestroySwapchainKHR(sDev, old, NULL);
    for (i = 0; i < sChainCount; i++) vkDestroySemaphore(sDev, sRendered[i], NULL);
    sChainCount = 0;
    sChainStale = 0;
    if (!sChain) return;
    sChainCount = 8;
    vkGetSwapchainImagesKHR(sDev, sChain, &sChainCount, sChainImages);
    for (i = 0; i < sChainCount; i++) CHECK(vkCreateSemaphore(sDev, &si, NULL, &sRendered[i]));
    sChainW = (int)ci.imageExtent.width;
    sChainH = (int)ci.imageExtent.height;
}

static void write_bmp(const char* path, const uint8_t* rgba, int w, int h) {
    FILE* f = fopen(path, "wb");
    uint8_t hdr[54] = {'B', 'M'};
    uint8_t* row;
    int x, y;
    if (!f) return;
    *(uint32_t*)(hdr + 2) = (uint32_t)(54 + w * h * 4);
    hdr[10] = 54;
    hdr[14] = 40;
    *(int32_t*)(hdr + 18) = w;
    *(int32_t*)(hdr + 22) = -h; /* top row first */
    hdr[26] = 1;
    hdr[28] = 32;
    fwrite(hdr, 1, 54, f);
    row = (uint8_t*)malloc((size_t)w * 4);
    for (y = 0; y < h; y++) {
        const uint8_t* px = rgba + (size_t)y * w * 4;
        for (x = 0; x < w; x++) {
            row[x * 4] = px[x * 4 + 2];
            row[x * 4 + 1] = px[x * 4 + 1];
            row[x * 4 + 2] = px[x * 4];
            row[x * 4 + 3] = 255;
        }
        fwrite(row, 4, (size_t)w, f);
    }
    free(row);
    fclose(f);
}

/* ------------------------------------------------------------------------ */
/* Frames */

static void begin_slot(void) {
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    int i;
    if (S->serial) {
        CHECK(vkWaitForFences(sDev, 1, &S->fence, VK_TRUE, UINT64_MAX));
        CHECK(vkResetFences(sDev, 1, &S->fence));
        if (S->serial > sDone) sDone = S->serial;
        if (sTimes) {
            uint64_t t[2];
            if (vkGetQueryPoolResults(sDev, sTimes, (uint32_t)sSlot * 2, 2, sizeof(t), t, sizeof(uint64_t),
                                      VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
                double ms = (double)(t[1] - t[0]) * sLimits.timestampPeriod / 1e6;
                sTimeTotal += ms;
                if (ms > sTimeWorst) sTimeWorst = ms;
                if (ms > 1000.0 / 90) sTimeSlow++;
                if (++sTimeFrames == 120) {
                    fprintf(stderr, "gpu: %.2f ms a frame, slowest %.1f ms, %u of 120 over 11 ms\n", sTimeTotal / sTimeFrames,
                            sTimeWorst, sTimeSlow);
                    sTimeTotal = sTimeWorst = 0;
                    sTimeFrames = sTimeSlow = 0;
                }
            }
        }
    }
    /* What the GPU is done with. */
    for (i = 0; i < sDeadCount;) {
        Dead* d = &sDead[i];
        if (d->serial > sDone) {
            i++;
            continue;
        }
        if (d->kind == DEAD_IMAGE) {
            vkDestroyImageView(sDev, d->view, NULL);
            vkDestroyImage(sDev, d->img, NULL);
            mem_free(d->mem);
        } else if (d->kind == DEAD_BUFFER) {
            vkDestroyBuffer(sDev, d->buf, NULL);
            mem_free(d->mem);
        } else {
            sFreeHandles[sFreeCount++] = d->handle;
        }
        *d = sDead[--sDeadCount];
    }
    CHECK(vkResetCommandPool(sDev, S->pool, 0));
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    CHECK(vkBeginCommandBuffer(S->up, &bi));
    CHECK(vkBeginCommandBuffer(S->cb, &bi));
    if (sTimes) {
        vkCmdResetQueryPool(S->up, sTimes, (uint32_t)sSlot * 2, 2);
        vkCmdWriteTimestamp(S->up, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, sTimes, (uint32_t)sSlot * 2);
    }
    /* The staging ring grows for bursts of uploads (loading); halved again
     * when frames have long needed far less. */
    if (S->staging_used > S->staging_peak) S->staging_peak = S->staging_used;
    if (++S->frames % 512 == 0) {
        if (S->staging.size > STAGING_SIZE && S->staging_peak < S->staging.size / 4) {
            VkDeviceSize size = S->staging.size / 2;
            drop_buffer(&S->staging);
            S->staging = make_buffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        }
        S->staging_peak = 0;
    }
    S->data_used = S->verts_used = S->idx_used = S->staging_used = 0;
    forget();
    sXfAt = sTevAt = -1;
    sBound[0] = sBound[1] = 0;
    bind_sets();
    vkCmdSetFrontFace(S->cb, VK_FRONT_FACE_COUNTER_CLOCKWISE);
}

/* Copies into the vertex arena are recorded this frame (vk_arena_write):
 * the frame's draws wait for them. */
static int sArenaCopied;

/* Submits the frame (and presents swapchain image `image`, if acquired),
 * and starts the next. */
static void end_slot(int acquired, uint32_t image, int dump, int dw, int dh) {
    VkSubmitInfo si[2] = {{VK_STRUCTURE_TYPE_SUBMIT_INFO}, {VK_STRUCTURE_TYPE_SUBMIT_INFO}};
    VkCommandBuffer cbs[2];
    VkPipelineStageFlags wait[2] = {VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT};
    VkSemaphore waits[2], signals[2];
    end_efb();
    if (sArenaCopied) {
        memory_barrier(S->up);
        sArenaCopied = 0;
    }
    if (sTimes) vkCmdWriteTimestamp(S->cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, sTimes, (uint32_t)sSlot * 2 + 1);
    CHECK(vkEndCommandBuffer(S->up));
    CHECK(vkEndCommandBuffer(S->cb));
    cbs[0] = S->up;
    cbs[1] = S->cb;
    si[0].commandBufferCount = 2;
    si[0].pCommandBuffers = cbs;
    /* The copy to the swapchain's image is a batch of its own, so waiting
     * for the image holds up only that and not the frame: on Android the
     * compositor gives an image back well after it's acquired. */
    if (acquired) {
        CHECK(vkEndCommandBuffer(S->out));
        si[1].commandBufferCount = 1;
        si[1].pCommandBuffers = &S->out;
        waits[0] = S->acquired;
        signals[0] = sRendered[image];
        waits[1] = S->acquired2;
        signals[1] = sAcquired2 ? sRendered2[sImage2] : VK_NULL_HANDLE;
        si[1].waitSemaphoreCount = si[1].signalSemaphoreCount = sAcquired2 ? 2 : 1;
        si[1].pWaitSemaphores = waits;
        si[1].pWaitDstStageMask = wait;
        si[1].pSignalSemaphores = signals;
    }
    present_done();
    CHECK(vkQueueSubmit(sQueue, acquired ? 2 : 1, si, S->fence));
    S->serial = sSerial++;
#ifdef __ANDROID__
    if (acquired) present_later(image, sAcquired2 ? sImage2 : ~0u);
#else
    if (acquired) {
        VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        VkResult r;
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &sRendered[image];
        pi.swapchainCount = 1;
        pi.pSwapchains = &sChain;
        pi.pImageIndices = &image;
        r = vkQueuePresentKHR(sQueue, &pi);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) sChainStale = 1;
    }
#endif
    if (dump) {
        char path[512];
        CHECK(vkWaitForFences(sDev, 1, &S->fence, VK_TRUE, UINT64_MAX));
        snprintf(path, sizeof(path), "%s/frame_%05u.bmp", sDumpDir, sPresented);
        write_bmp(path, sReadback.map, dw, dh);
        if (sBottomDumped) {
            snprintf(path, sizeof(path), "%s/second_%05u.bmp", sDumpDir, sPresented);
            write_bmp(path, sReadback2.map, sBottom.w, sBottom.h);
        }
    }
    sAcquired2 = sBottomDumped = 0;
    sSlot = (sSlot + 1) % SLOTS;
    S = &sSlots[sSlot];
    begin_slot();
}

/* ------------------------------------------------------------------------ */
/* The device's calls */

static uint32_t vk_texture(int pix, int width, int height, int levels, const uint8_t* pixels, uint32_t mode0);
static void upload(VkCommandBuffer cb, Tex* t, int width, int height, int levels, const uint8_t* pixels);

static int pick_gpu(void) {
    VkPhysicalDevice gpus[16];
    uint32_t n = 16, i, best = ~0u, bestScore = 0;
    vkEnumeratePhysicalDevices(sInstance, &n, gpus);
    for (i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        VkQueueFamilyProperties qf[32];
        uint32_t nq = 32, q;
        VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        VkPhysicalDeviceFeatures2 f = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        uint32_t score;
        vkGetPhysicalDeviceProperties(gpus[i], &p);
        if (p.apiVersion < VK_API_VERSION_1_3) continue;
        f.pNext = &f12;
        f12.pNext = &f13;
        vkGetPhysicalDeviceFeatures2(gpus[i], &f);
        if (!f13.dynamicRendering || !f13.synchronization2 || !f13.shaderDemoteToHelperInvocation ||
            !f12.descriptorBindingPartiallyBound ||
            !f12.descriptorBindingSampledImageUpdateAfterBind || !f12.descriptorBindingUpdateUnusedWhilePending)
            continue;
        vkGetPhysicalDeviceQueueFamilyProperties(gpus[i], &nq, qf);
        for (q = 0; q < nq; q++) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(gpus[i], q, sSurface, &present);
            if ((qf[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) break;
        }
        if (q == nq) continue;
        score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3 : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
        if (best == ~0u || score > bestScore) {
            best = i;
            bestScore = score;
            sGpu = gpus[i];
            sQueueFamily = q;
            sLimits = p.limits;
            sMaxAniso = f.features.samplerAnisotropy ? p.limits.maxSamplerAnisotropy : 0;
            sBc = f.features.textureCompressionBC;
        }
    }
    if (best == ~0u) return 0;
    {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(sGpu, &p);
        if (p.limits.minUniformBufferOffsetAlignment > sUboAlign) sUboAlign = p.limits.minUniformBufferOffsetAlignment;
        fprintf(stderr, "vulkan: %s, Vulkan %u.%u.%u\n", p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion),
                VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion));
    }
    return 1;
}

static VKAPI_ATTR VkBool32 VKAPI_CALL debug_message(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                   VkDebugUtilsMessageTypeFlagsEXT type,
                                                   const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) {
    static unsigned count;
    (void)type;
    (void)user;
    if (severity < VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) return VK_FALSE;
    if (++count <= 200) fprintf(stderr, "vulkan validation: %s\n", data->pMessage);
    return VK_FALSE;
}

/* The window's surface: made at the start, and again on Android when the
 * app comes back with a new window. */
static int make_surface(void) {
#ifdef _WIN32
    VkWin32SurfaceCreateInfoKHR sci = {VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    sci.hinstance = GetModuleHandleA(NULL);
    sci.hwnd = (HWND)win_handle();
    return vkCreateWin32SurfaceKHR(sInstance, &sci, NULL, &sSurface) == VK_SUCCESS;
#else
    VkAndroidSurfaceCreateInfoKHR sci = {VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
    sci.window = (ANativeWindow*)win_handle();
    return vkCreateAndroidSurfaceKHR(sInstance, &sci, NULL, &sSurface) == VK_SUCCESS;
#endif
}

static int make_instance(void) {
#ifdef _WIN32
    HMODULE lib = LoadLibraryA("vulkan-1.dll");
#define LIB_SYMBOL GetProcAddress
#else
    void* lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
#define LIB_SYMBOL dlsym
#endif
    const char* exts[3] = {VK_KHR_SURFACE_EXTENSION_NAME, SURFACE_EXTENSION, VK_EXT_DEBUG_UTILS_EXTENSION_NAME};
    const char* layer = "VK_LAYER_KHRONOS_validation";
    const char* v = getenv("RECOMP_VK_VALIDATE");
    int validate = v && *v && *v != '0';
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    VkInstanceCreateInfo ci = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    if (!lib) {
        fprintf(stderr, "vulkan: no Vulkan library\n");
        return 0;
    }
    vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)(void*)LIB_SYMBOL(lib, "vkGetInstanceProcAddr");
    if (!vkGetInstanceProcAddr) return 0;
#define LOAD(name) name = (PFN_##name)(void*)vkGetInstanceProcAddr(NULL, #name);
    VK_GLOBAL_FUNCS(LOAD)
#undef LOAD
    app.pApplicationName = "PokePark (recompiled)";
    app.apiVersion = VK_API_VERSION_1_3;
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = validate ? 3 : 2;
    ci.ppEnabledExtensionNames = exts;
    if (validate) {
        ci.enabledLayerCount = 1;
        ci.ppEnabledLayerNames = &layer;
    }
    if (vkCreateInstance(&ci, NULL, &sInstance) != VK_SUCCESS) {
        if (!validate) {
            fprintf(stderr, "vulkan: no instance\n");
            return 0;
        }
        fprintf(stderr, "vulkan: no validation layers; without them\n");
        ci.enabledExtensionCount = 2;
        ci.enabledLayerCount = 0;
        validate = 0;
        if (vkCreateInstance(&ci, NULL, &sInstance) != VK_SUCCESS) return 0;
    }
#define LOAD(name)                                                                                  \
    if (!(name = (PFN_##name)(void*)vkGetInstanceProcAddr(sInstance, #name))) {                     \
        fprintf(stderr, "vulkan: missing %s\n", #name);                                             \
        return 0;                                                                                   \
    }
    VK_INSTANCE_FUNCS(LOAD)
#undef LOAD
    if (validate) {
        PFN_vkCreateDebugUtilsMessengerEXT create =
            (PFN_vkCreateDebugUtilsMessengerEXT)(void*)vkGetInstanceProcAddr(sInstance, "vkCreateDebugUtilsMessengerEXT");
        VkDebugUtilsMessengerCreateInfoEXT di = {VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        VkDebugUtilsMessengerEXT m;
        di.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        di.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                         VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        di.pfnUserCallback = debug_message;
        if (create) create(sInstance, &di, NULL, &m);
        fprintf(stderr, "vulkan: validation layers on\n");
    }
    return make_surface();
}

static int make_device(void) {
    float prio = 1;
    const char* exts[2] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_EXT_LOAD_STORE_OP_NONE_EXTENSION_NAME};
    VkDeviceQueueCreateInfo qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceFeatures2 f = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkDeviceCreateInfo ci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    VkPresentModeKHR modes[16];
    uint32_t nm = 16, i;
    qi.queueFamilyIndex = sQueueFamily;
    qi.queueCount = 1;
    qi.pQueuePriorities = &prio;
    {
        uint32_t n = 0, k;
        VkExtensionProperties* e;
        vkEnumerateDeviceExtensionProperties(sGpu, NULL, &n, NULL);
        e = (VkExtensionProperties*)calloc(n ? n : 1, sizeof(*e));
        vkEnumerateDeviceExtensionProperties(sGpu, NULL, &n, e);
        for (k = 0; k < n; k++) {
            if (strcmp(e[k].extensionName, VK_EXT_LOAD_STORE_OP_NONE_EXTENSION_NAME) == 0) sLoadNone = 1;
        }
        free(e);
    }
    f.pNext = &f12;
    f12.pNext = &f13;
    f12.descriptorBindingPartiallyBound = VK_TRUE;
    f12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    f12.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
    f13.dynamicRendering = VK_TRUE;
    f13.synchronization2 = VK_TRUE;
    f13.shaderDemoteToHelperInvocation = VK_TRUE; /* discard (the alpha test) compiles to it */
    f.features.samplerAnisotropy = sMaxAniso > 0 ? VK_TRUE : VK_FALSE;
    ci.pNext = &f;
    ci.queueCreateInfoCount = 1;
    ci.pQueueCreateInfos = &qi;
    ci.enabledExtensionCount = sLoadNone ? 2 : 1;
    ci.ppEnabledExtensionNames = exts;
    if (vkCreateDevice(sGpu, &ci, NULL, &sDev) != VK_SUCCESS) {
        fprintf(stderr, "vulkan: no device\n");
        return 0;
    }
#define LOAD(name)                                                                                  \
    if (!(name = (PFN_##name)(void*)vkGetDeviceProcAddr(sDev, #name))) {                            \
        fprintf(stderr, "vulkan: missing %s\n", #name);                                             \
        return 0;                                                                                   \
    }
    VK_DEVICE_FUNCS(LOAD)
#undef LOAD
    vkGetDeviceQueue(sDev, sQueueFamily, 0, &sQueue);
    vkGetPhysicalDeviceMemoryProperties(sGpu, &sMemProps);
    /* Presenting never waits for the display (as gl_backend.c's): the
     * game paces itself. */
    vkGetPhysicalDeviceSurfacePresentModesKHR(sGpu, sSurface, &nm, modes);
    sPresentMode = VK_PRESENT_MODE_FIFO_KHR;
    for (i = 0; i < nm; i++) {
        if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) sPresentMode = modes[i];
        else if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR && sPresentMode == VK_PRESENT_MODE_FIFO_KHR) sPresentMode = modes[i];
    }
    {
        VkSampleCountFlags c = sLimits.framebufferColorSampleCounts & sLimits.framebufferDepthSampleCounts;
        for (sMaxSamples = 8; sMaxSamples > 1 && !(c & (VkSampleCountFlags)sMaxSamples); sMaxSamples /= 2) {}
    }
    return 1;
}

static void make_layouts(void) {
    VkDescriptorSetLayoutBinding tb = {0}, db[3] = {{0}, {0}, {0}};
    VkDescriptorBindingFlags tflags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
                                      VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
    VkDescriptorSetLayoutBindingFlagsCreateInfo fi = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    VkDescriptorPoolSize sizes[3];
    VkDescriptorPoolCreateInfo pi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    VkPushConstantRange pc;
    VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    VkDescriptorSetLayout layouts[2];
    tb.binding = 0;
    tb.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    tb.descriptorCount = MAX_TEXTURES;
    tb.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    fi.bindingCount = 1;
    fi.pBindingFlags = &tflags;
    li.pNext = &fi;
    li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    li.bindingCount = 1;
    li.pBindings = &tb;
    CHECK(vkCreateDescriptorSetLayout(sDev, &li, NULL, &sTexLayout));
    db[0].binding = 0;
    db[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    db[0].descriptorCount = 1;
    db[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    db[1].binding = 1;
    db[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    db[1].descriptorCount = 1;
    db[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    db[2].binding = 2;
    db[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    db[2].descriptorCount = 1;
    db[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    li.pNext = NULL;
    li.flags = 0;
    li.bindingCount = 3;
    li.pBindings = db;
    CHECK(vkCreateDescriptorSetLayout(sDev, &li, NULL, &sDataLayout));
    sizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    sizes[0].descriptorCount = MAX_TEXTURES;
    sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    sizes[1].descriptorCount = 256;
    sizes[2].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    sizes[2].descriptorCount = 512;
    pi.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    pi.maxSets = 257;
    pi.poolSizeCount = 3;
    pi.pPoolSizes = sizes;
    CHECK(vkCreateDescriptorPool(sDev, &pi, NULL, &sPool));
    ai.descriptorPool = sPool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &sTexLayout;
    CHECK(vkAllocateDescriptorSets(sDev, &ai, &sTexSet));
    pc.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pc.offset = 0;
    pc.size = sizeof(Quad);
    layouts[0] = sTexLayout;
    layouts[1] = sDataLayout;
    pli.setLayoutCount = 2;
    pli.pSetLayouts = layouts;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pc;
    CHECK(vkCreatePipelineLayout(sDev, &pli, NULL, &sLayout));
}

static void vk_apply_settings(void);
static void find_rebar(void);
static void spec_start(void);

static int vk_init(void) {
    VkCommandPoolCreateInfo cpi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkSemaphoreCreateInfo sci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSamplerCreateInfo smp = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    const VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    uint32_t white = 0xFFFFFFFF;
    const char* v;
    int i;
    if (!make_instance() || !pick_gpu() || !make_device()) return 0;
    sPixelAspect = win_pixel_aspect();
    sDumpDir = getenv("RECOMP_GX_DUMP");
    if ((v = getenv("RECOMP_GX_DUMP_EVERY")) && atoi(v) > 0) sDumpEvery = atoi(v);
    if ((v = getenv("RECOMP_GX_DUMP_FROM")) && atoi(v) > 0) sDumpFrom = (unsigned)atoi(v);
    make_layouts();
    find_rebar();
    for (i = 0; i < 9; i++) {
        sGxVert[i] = shader(kSpv_gx_vert[i], kSpv_gx_vert_size[i]);
        sGxFrag[i] = shader(kSpv_gx_frag[i], kSpv_gx_frag_size[i]);
    }
    sQuadVert = shader(kSpv_quad_vert, sizeof(kSpv_quad_vert));
    sFillFrag = shader(kSpv_fill_frag, sizeof(kSpv_fill_frag));
    sCopyFrag = shader(kSpv_copy_frag, sizeof(kSpv_copy_frag));
    sBlitFrag = shader(kSpv_blit_frag, sizeof(kSpv_blit_frag));
    spec_start();
    smp.magFilter = smp.minFilter = VK_FILTER_LINEAR;
    smp.addressModeU = smp.addressModeV = smp.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    CHECK(vkCreateSampler(sDev, &smp, NULL, &sLinearClamp));
    for (i = MAX_TEXTURES - 1; i >= TEX_FIRST; i--) sFreeHandles[sFreeCount++] = (uint32_t)i;
    {
        const char* e = getenv("RECOMP_FPS");
        if (e && *e == '1' && sLimits.timestampComputeAndGraphics) {
            VkQueryPoolCreateInfo qi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
            qi.queryCount = SLOTS * 2;
            if (vkCreateQueryPool(sDev, &qi, NULL, &sTimes) != VK_SUCCESS) sTimes = VK_NULL_HANDLE;
        }
    }
    cpi.queueFamilyIndex = sQueueFamily;
    for (i = 0; i < SLOTS; i++) {
        Slot* s = &sSlots[i];
        VkCommandBuffer cbs[3];
        CHECK(vkCreateCommandPool(sDev, &cpi, NULL, &s->pool));
        cai.commandPool = s->pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 3;
        CHECK(vkAllocateCommandBuffers(sDev, &cai, cbs));
        s->up = cbs[0];
        s->cb = cbs[1];
        s->out = cbs[2];
        CHECK(vkCreateFence(sDev, &fci, NULL, &s->fence));
        CHECK(vkCreateSemaphore(sDev, &sci, NULL, &s->acquired));
        CHECK(vkCreateSemaphore(sDev, &sci, NULL, &s->acquired2));
        s->data = make_buffer((VkDeviceSize)2 << 20, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                              host);
        s->verts = make_buffer((VkDeviceSize)4 << 20, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, host);
        s->idx = make_buffer((VkDeviceSize)1 << 20, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, host);
        s->staging = make_buffer(STAGING_SIZE, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, host);
        S = s;
        data_set();
    }
    S = &sSlots[0];
    sSlot = 0;
    begin_slot();
    /* Texture 0: white, for stages without one. */
    sTex[TEX_WHITE].im = make_image(1, 1, 1, VK_FORMAT_R8G8B8A8_UNORM,
                                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SAMPLE_COUNT_1_BIT);
    sTex[TEX_WHITE].sampler = sLinearClamp;
    upload(S->up, &sTex[TEX_WHITE], 1, 1, 1, (const uint8_t*)&white);
    set_texture(TEX_WHITE);
    vk_apply_settings();
    fprintf(stderr, "vulkan: presenting %s\n", sPresentMode == VK_PRESENT_MODE_IMMEDIATE_KHR ? "immediately (tearing allowed)"
                                               : sPresentMode == VK_PRESENT_MODE_MAILBOX_KHR ? "by mailbox" : "in time with the display");
    return 1;
}

static int vk_scale(void) { return sScale; }
static int vk_msaa(void) {
    int m;
    for (m = g_settings.msaa; m > sMaxSamples; m /= 2) {}
    return m;
}

static void vk_apply_settings(void) {
    int w, h, s, m, mx, my;
    win_size(&w, &h);
    if (h <= 0) return; /* minimized */
    s = gx_wanted_scale(h);
    for (m = g_settings.msaa; m > sMaxSamples; m /= 2) {}
    gx_canvas(&mx, &my);
    if (s == sScale && m == sMsaa && mx == sMX && my == sMY && sEfb.img) return;
    /* Nothing in flight may still use what is remade. */
    wait_idle();
    if (m != sMsaa) {
        /* The EFB's pipelines are for the old sample count; they stay
         * made, as switching back is likely. */
        sCmd.pipe = VK_NULL_HANDLE;
    }
    sScale = s;
    sMsaa = m;
    sMX = mx;
    sMY = my;
    sScaleY = s;
    sScaleX = s * sPixelAspect;
    alloc_efb();
    fprintf(stderr, "vulkan: internal resolution %dx (%d x %d), anti-aliasing %s\n", s, VX(EFB_W + 2 * sMX),
            VY(EFB_H + 2 * sMY),
            m > 1 ? (m == 2 ? "2x" : m == 4 ? "4x" : "8x") : "off");
}

/* Copies the levels of pixels (each half the last's size, laid out as
 * t->pix says) into t's image, in cb, with the copy in the slot's staging
 * ring. */
static void upload(VkCommandBuffer cb, Tex* t, int width, int height, int levels, const uint8_t* pixels) {
    VkBufferImageCopy regions[16];
    VkDeviceSize bytes = 0, off;
    int l, w = width, h = height;
    uint8_t* dst;
    VkBuffer src;
    for (l = 0; l < levels; l++) {
        bytes += gx_pix_size(t->pix, w, h);
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
    dst = (uint8_t*)staging(bytes, &src, &off);
    memcpy(dst, pixels, (size_t)bytes);
    w = width;
    h = height;
    memset(regions, 0, sizeof(regions));
    for (l = 0; l < levels && l < 16; l++) {
        regions[l].bufferOffset = off;
        regions[l].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        regions[l].imageSubresource.mipLevel = (uint32_t)l;
        regions[l].imageSubresource.layerCount = 1;
        regions[l].imageExtent.width = (uint32_t)w;
        regions[l].imageExtent.height = (uint32_t)h;
        regions[l].imageExtent.depth = 1;
        off += gx_pix_size(t->pix, w, h);
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
    transition(cb, &t->im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdCopyBufferToImage(cb, src, t->im.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, (uint32_t)l, regions);
    transition(cb, &t->im, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

/* Copies part r (x, y, w, h) of a w-wide image of 4-byte pixels into the
 * same part of t's image. */
static void upload_part(VkCommandBuffer cb, Tex* t, int w, const int* r, const uint32_t* pixels) {
    VkBufferImageCopy region;
    VkDeviceSize off, bytes = (VkDeviceSize)r[2] * r[3] * 4;
    VkBuffer src;
    uint32_t* dst = (uint32_t*)staging(bytes, &src, &off);
    int y;
    for (y = 0; y < r[3]; y++) memcpy(dst + (size_t)y * r[2], pixels + (size_t)(r[1] + y) * w + r[0], (size_t)r[2] * 4);
    memset(&region, 0, sizeof(region));
    region.bufferOffset = off;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageOffset.x = r[0];
    region.imageOffset.y = r[1];
    region.imageExtent.width = (uint32_t)r[2];
    region.imageExtent.height = (uint32_t)r[3];
    region.imageExtent.depth = 1;
    transition(cb, &t->im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdCopyBufferToImage(cb, src, t->im.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    transition(cb, &t->im, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

static unsigned vk_texture_pixels(void) {
    return (1u << GX_PIX_I8) | (1u << GX_PIX_IA8) | (sBc ? 1u << GX_PIX_BC1 : 0u);
}

static uint32_t vk_texture(int pix, int width, int height, int levels, const uint8_t* pixels, uint32_t mode0) {
    static const VkFormat kFormat[] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM,
                                       VK_FORMAT_BC1_RGB_UNORM_BLOCK};
    const VkComponentSwizzle I = VK_COMPONENT_SWIZZLE_R, A = VK_COMPONENT_SWIZZLE_G;
    VkComponentMapping map = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                              VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
    uint32_t h = new_handle();
    Tex* t = &sTex[h];
    if (levels > 16) levels = 16;
    if (pix == GX_PIX_I8) map.r = map.g = map.b = map.a = I;
    if (pix == GX_PIX_IA8) {
        map.r = map.g = map.b = I;
        map.a = A;
    }
    t->pix = pix;
    t->im = make_image_mapped(width, height, levels, kFormat[pix], VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                              VK_SAMPLE_COUNT_1_BIT, map);
    t->sampler = sampler(mode0, levels);
    t->used = 0;
    upload(S->up, t, width, height, levels, pixels);
    set_texture(h);
    return h;
}

static void vk_texture_update(uint32_t handle, int pix, int width, int height, int levels, const uint8_t* pixels) {
    Tex* t = &sTex[handle];
    (void)pix; /* the one it was made with */
    if (levels > 16) levels = 16;
    if (t->used == sSerial) {
        /* A draw of this frame used it: changed in the frame's order. */
        end_efb();
        upload(S->cb, t, width, height, levels, pixels);
        forget();
        bind_sets();
        vkCmdSetFrontFace(S->cb, VK_FRONT_FACE_COUNTER_CLOCKWISE);
    } else {
        upload(S->up, t, width, height, levels, pixels);
    }
}

static void vk_texture_delete(const uint32_t* handles, int n) {
    int i;
    for (i = 0; i < n; i++) {
        Dead* d;
        drop_image(&sTex[handles[i]].im);
        d = dead(DEAD_HANDLE);
        d->handle = handles[i];
    }
}

/* ------------------------------------------------------------------------ */
/* Drawing */

static uint32_t sXfLast[XF_PACKED];

/* The texture coordinates' setup, for gx.vert: each coordinate as one
 * matrix (3 x 4) times one of the vertex's vectors (with 1 for w), so the
 * shader does no more for it than that. Into out[i]: coordinate i's vector
 * (0 the position, 1 the normal, 2 none, (0, 0, 1, 1), 4 + n texture
 * coordinates n); out[8 + 12 i]: its matrix,
 * the texture matrix XF's MATINDEX registers give it (or, unlike XF's,
 * rows passing the vector's x and y through, and 1, for a texgen other
 * than a transform), times the post-transform matrix with dual texturing.
 * Not such (and left to the shader's XF, vs_words says): a post-transform
 * of normalized coordinates. */
static void texgen_program(const uint32_t* xf, uint32_t* out) {
    const uint32_t* r = xf + GX_XF_MEM_WORDS;
    float* m = (float*)(out + 8);
    int n = (int)(r[0x3F] & 15), dual = (int)(r[0x12] & 1), i, j, k;
    memset(out, 0, 8 * sizeof(uint32_t));
    if (n > 8) n = 8;
    for (i = 0; i < n; i++, m += 12) {
        uint32_t info = r[0x40 + i], pt = r[0x50 + i];
        int row = (int)((info >> 7) & 31), type = (int)((info >> 4) & 7);
        float t[4][4], *p;
        out[i] = row == 0 ? 0 : row == 1 ? 1 : row >= 5 && row <= 12 ? (uint32_t)(4 + row - 5) : 2;
        memset(t, 0, sizeof(t));
        if (type == 0) {
            uint32_t idx = i < 4 ? (r[0x18] >> (6 + 6 * i)) & 0x3F : (r[0x19] >> (6 * (i - 4))) & 0x3F;
            memcpy(t, xf + idx * 4, ((info >> 1) & 1 ? 12 : 8) * sizeof(float));
            if (!((info >> 1) & 1)) t[2][3] = 1;
        } else {
            t[0][0] = t[1][1] = t[2][3] = 1;
        }
        t[3][3] = 1;
        if (!dual) {
            memcpy(m, t, 12 * sizeof(float));
            continue;
        }
        if ((pt >> 8) & 1) return;
        p = (float*)(xf + 0x500 + (pt & 63) * 4);
        for (j = 0; j < 3; j++) {
            for (k = 0; k < 4; k++)
                m[j * 4 + k] = p[j * 4] * t[0][k] + p[j * 4 + 1] * t[1][k] + p[j * 4 + 2] * t[2][k] + p[j * 4 + 3] * t[3][k];
        }
    }
}

static uint32_t write_xf(const uint32_t* xf) {
    uint32_t packed[XF_PACKED];
    VkDeviceSize off;
    if (!xf) {
        if (sXfAt >= 0) return (uint32_t)sXfAt;
        memset(packed, 0, sizeof(packed));
    } else {
        memcpy(packed, xf, 0x100 * 4);
        memcpy(packed + 0x100, xf + 0x400, 0x80 * 4);
        memcpy(packed + 0x180, xf + 0x500, 0x180 * 4);
        memcpy(packed + 0x300, xf + GX_XF_MEM_WORDS, GX_XF_REG_WORDS * 4);
        texgen_program(xf, packed + 0x360);
    }
    if (sXfAt >= 0 && memcmp(packed, sXfLast, sizeof(packed)) == 0) return (uint32_t)sXfAt;
    memcpy(ring(RING_DATA, sizeof(packed), sUboAlign, &off), packed, sizeof(packed));
    memcpy(sXfLast, packed, sizeof(packed));
    sXfAt = (int64_t)(off / 4);
    return (uint32_t)sXfAt;
}

static uint32_t sTevLast[TEV_WORDS];
static int sTevLastWords;

static uint32_t write_tev(const GXDrawState* st) {
    uint32_t w[TEV_WORDS];
    int32_t* iw = (int32_t*)w;
    float* fw = (float*)w;
    int i, k, n = 160 + 24 * st->num_stages;
    VkDeviceSize off;
    memset(w, 0, (size_t)n * 4);
    iw[0] = st->num_stages;
    iw[1] = st->alpha_func[0];
    iw[2] = st->alpha_func[1];
    iw[3] = st->alpha_logic;
    fw[4] = (float)st->alpha_ref[0];
    fw[5] = (float)st->alpha_ref[1];
    for (i = 0; i < 4; i++) {
        for (k = 0; k < 4; k++) {
            fw[8 + 4 * i + k] = st->tev_reg[i][k];
            fw[24 + 4 * i + k] = st->konst[i][k];
            iw[40 + 4 * i + k] = st->swap_table[i][k];
        }
    }
    iw[56] = st->fog_type;
    iw[57] = st->fog_ortho;
    iw[58] = st->fog_b_mag;
    iw[59] = st->fog_b_shift;
    fw[60] = st->fog_a;
    fw[61] = st->fog_c;
    iw[62] = (int32_t)((uint32_t)(st->fog_color[0] * 255.0f + 0.5f) | (uint32_t)(st->fog_color[1] * 255.0f + 0.5f) << 8 |
                       (uint32_t)(st->fog_color[2] * 255.0f + 0.5f) << 16);
    if (st->fog_range) {
        /* The pixel's offset from the center, in EFB pixels: x a + b. */
        fw[144] = (float)(1.0 / sScaleX);
        fw[145] = -st->fog_center - (float)sMX;
        iw[146] = 1;
        for (i = 0; i < 10; i++) fw[148 + i] = st->fog_k[i];
    }
    if (st->ind_any) {
        for (i = 0; i < st->num_stages; i++) iw[64 + i] = (int32_t)st->ind_cmd[i];
        for (i = 0; i < 4; i++) {
            iw[80 + 4 * i] = st->ind_map[i];
            iw[81 + 4 * i] = st->ind_coord[i];
            iw[82 + 4 * i] = st->ind_scale[i][0];
            iw[83 + 4 * i] = st->ind_scale[i][1];
        }
        for (i = 0; i < 3; i++) {
            for (k = 0; k < 3; k++) {
                iw[96 + 8 * i + k] = st->ind_mtx[i][0][k];
                iw[100 + 8 * i + k] = st->ind_mtx[i][1][k];
            }
            iw[99 + 8 * i] = iw[103 + 8 * i] = 17 - st->ind_mtx_scale[i];
        }
        iw[120] = st->ind_stages;
        iw[121] = 1;
        for (i = 0; i < 8; i++) {
            iw[128 + 2 * i] = st->tc_size[i][0];
            iw[129 + 2 * i] = st->tc_size[i][1];
        }
    }
    for (i = 0; i < st->num_stages; i++) {
        const GXTevStage* s = &st->stage[i];
        int32_t* o = iw + 160 + 24 * i;
        for (k = 0; k < 4; k++) {
            o[k] = s->color_in[k];
            o[4 + k] = s->alpha_in[k];
        }
        o[8] = s->color_op;
        o[9] = s->color_bias;
        o[10] = s->color_scale;
        o[11] = s->color_clamp;
        o[12] = s->alpha_op;
        o[13] = s->alpha_bias;
        o[14] = s->alpha_scale;
        o[15] = s->alpha_clamp;
        o[16] = s->texmap;
        o[17] = s->texcoord;
        o[18] = s->tex_enable;
        /* (The bump alphas, 5 and 6, with an indirect stage to give them.) */
        o[19] = s->channel == 7 || (st->ind_any && (s->channel == 5 || s->channel == 6)) ? s->channel : (s->channel & 1);
        o[20] = s->color_dest;
        o[21] = s->alpha_dest;
        o[22] = s->kcolor_sel | (s->kalpha_sel << 5);
        o[23] = s->tex_swap | (s->ras_swap << 2);
    }
    if (sTevAt >= 0 && n == sTevLastWords && memcmp(w, sTevLast, (size_t)n * 4) == 0) return (uint32_t)sTevAt;
    /* The shader may read all 16 stages' worth: room for them, written or not. */
    memcpy(ring(RING_DATA, TEV_WORDS * 4, sUboAlign, &off), w, (size_t)n * 4);
    memcpy(sTevLast, w, (size_t)n * 4);
    sTevLastWords = n;
    sTevAt = (int64_t)(off / 4);
    return (uint32_t)sTevAt;
}

/* Whether the alpha compare passes whatever the alpha (gx.frag's
 * ALPHA_TEST): GX_NEVER 0, GX_ALWAYS 7; logic AND, OR, XOR, XNOR. */
static int alpha_always(const GXDrawState* st) {
    int f0 = st->alpha_func[0], f1 = st->alpha_func[1];
    switch (st->alpha_logic) {
    case 0: return f0 == 7 && f1 == 7;
    case 1: return f0 == 7 || f1 == 7;
    case 2: return (f0 == 7 && f1 == 0) || (f0 == 0 && f1 == 7);
    default: return f0 == f1 && (f0 == 0 || f0 == 7);
    }
}

/* The pipeline's: color and alpha writes (bits 0-3), blending (4-15),
 * and no alpha test (16). */
static uint32_t gx_key(const GXDrawState* st) {
    static const VkBlendFactor kSrc[8] = {
        VK_BLEND_FACTOR_ZERO,      VK_BLEND_FACTOR_ONE,       VK_BLEND_FACTOR_DST_COLOR,
        VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA};
    static const VkBlendFactor kDst[8] = {
        VK_BLEND_FACTOR_ZERO,      VK_BLEND_FACTOR_ONE,       VK_BLEND_FACTOR_SRC_COLOR,
        VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA};
    uint32_t key = (st->color_update ? 7u : 0u) | (st->alpha_update && st->efb_alpha ? 8u : 0u);
    if (st->blend_enable) {
        VkBlendFactor src = kSrc[st->blend_src & 7], dst = kDst[st->blend_dst & 7];
        /* Without an alpha channel the EFB's alpha reads as 1 (gl_backend.c). */
        if (!st->efb_alpha) {
            if (src == VK_BLEND_FACTOR_DST_ALPHA) src = VK_BLEND_FACTOR_ONE;
            if (src == VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA) src = VK_BLEND_FACTOR_ZERO;
            if (dst == VK_BLEND_FACTOR_DST_ALPHA) dst = VK_BLEND_FACTOR_ONE;
            if (dst == VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA) dst = VK_BLEND_FACTOR_ZERO;
        }
        if (st->blend_subtract) src = dst = VK_BLEND_FACTOR_ONE;
        key |= 16u | ((uint32_t)src << 5) | ((uint32_t)dst << 10) | (st->blend_subtract ? 1u << 15 : 0u);
    }
    if (alpha_always(st)) key |= 1u << 16;
    return key;
}

/* Mirrored for the reversed depth (gl_backend.c). */
static const VkCompareOp kDepthFunc[8] = {VK_COMPARE_OP_NEVER, VK_COMPARE_OP_GREATER, VK_COMPARE_OP_EQUAL,
                                          VK_COMPARE_OP_GREATER_OR_EQUAL, VK_COMPARE_OP_LESS, VK_COMPARE_OP_NOT_EQUAL,
                                          VK_COMPARE_OP_LESS_OR_EQUAL, VK_COMPARE_OP_ALWAYS};
/* GX_CULL_NONE, FRONT, BACK, ALL; GX's front faces wind counter-clockwise
 * as gl_backend.c sees them, and the flipped viewport keeps that. */
static const VkCullModeFlags kCull[4] = {VK_CULL_MODE_NONE, VK_CULL_MODE_FRONT_BIT, VK_CULL_MODE_BACK_BIT,
                                         VK_CULL_MODE_FRONT_AND_BACK};

/* ------------------------------------------------------------------------ */
/* Pipelines for TEV setups */

/* The general shader runs every stage's every choice for every pixel: a
 * phone's GPU takes several times as long as with gx.frag specialized for
 * the setup, which a draw uses once the pipeline thread has made it. */
typedef struct Spec {
    uint32_t words[SPEC_WORDS];
    uint32_t vs[VS_WORDS];
    uint32_t key;
    int samples, used;
    VkPipeline p; /* set by the pipeline thread once made */
} Spec;
#define SPEC_SLOTS 8192 /* a power of two; filled to three quarters at most */
static Spec* sSpecs;
static int sSpecCount;
static int sMakeQueue[SPEC_SLOTS], sMakeHead, sMakeTail;
static char sCachePath[1024];

#ifdef _WIN32
static CRITICAL_SECTION sMakeLock;
static CONDITION_VARIABLE sMakeWake;
#define MAKE_LOCK() EnterCriticalSection(&sMakeLock)
#define MAKE_UNLOCK() LeaveCriticalSection(&sMakeLock)
#define MAKE_WAIT() SleepConditionVariableCS(&sMakeWake, &sMakeLock, INFINITE)
#define MAKE_WAKE() WakeConditionVariable(&sMakeWake)
#else
static pthread_mutex_t sMakeLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sMakeWake = PTHREAD_COND_INITIALIZER;
#define MAKE_LOCK() pthread_mutex_lock(&sMakeLock)
#define MAKE_UNLOCK() pthread_mutex_unlock(&sMakeLock)
#define MAKE_WAIT() pthread_cond_wait(&sMakeWake, &sMakeLock)
#define MAKE_WAKE() pthread_cond_signal(&sMakeWake)
#endif
#ifdef _MSC_VER
#define PIPE_LOAD(x) (*(volatile VkPipeline*)&(x))
#define PIPE_STORE(x, v) (*(volatile VkPipeline*)&(x) = (v))
#define SAVE_BEGIN() (InterlockedExchange(&sSaving, 1) == 0)
#define SAVE_END() InterlockedExchange(&sSaving, 0)
#else
#define PIPE_LOAD(x) __atomic_load_n(&(x), __ATOMIC_ACQUIRE)
#define PIPE_STORE(x, v) __atomic_store_n(&(x), (v), __ATOMIC_RELEASE)
#define SAVE_BEGIN() (__atomic_exchange_n(&sSaving, 1, __ATOMIC_ACQUIRE) == 0)
#define SAVE_END() __atomic_store_n(&sSaving, 0, __ATOMIC_RELEASE)
#endif
static volatile long sSaving; /* save_cache is running (on either thread) */

/* The setup's words, as gx.frag's run unpacks them. */
static void spec_words(const GXDrawState* st, uint32_t* w) {
    int i, k, n = st->num_stages > 16 ? 16 : st->num_stages;
    memset(w, 0, SPEC_WORDS * sizeof(uint32_t));
    w[0] = (uint32_t)n;
    for (i = 0; i < 4; i++) {
        for (k = 0; k < 4; k++) w[1] |= (uint32_t)(st->swap_table[i][k] & 3) << (8 * i + 2 * k);
    }
    for (i = 0; i < n; i++) {
        const GXTevStage* s = &st->stage[i];
        uint32_t* o = w + 2 + 3 * i;
        for (k = 0; k < 4; k++) {
            o[0] |= (uint32_t)(s->color_in[k] & 15) << (4 * k);
            o[0] |= (uint32_t)(s->alpha_in[k] & 7) << (16 + 3 * k);
        }
        o[1] = (uint32_t)(s->color_op & 15) | (uint32_t)(s->color_bias & 15) << 4 | (uint32_t)(s->color_scale & 15) << 8 |
               (uint32_t)(s->color_clamp & 15) << 12 | (uint32_t)(s->alpha_op & 15) << 16 |
               (uint32_t)(s->alpha_bias & 15) << 20 | (uint32_t)(s->alpha_scale & 15) << 24 |
               (uint32_t)(s->alpha_clamp & 15) << 28;
        o[2] = (uint32_t)(s->texmap & 15) | (uint32_t)(s->texcoord & 15) << 4 | (s->tex_enable ? 1u : 0u) << 8 |
               (uint32_t)(s->channel == 7 ? 7 : (s->channel & 1)) << 9 | (uint32_t)(s->color_dest & 3) << 13 |
               (uint32_t)(s->alpha_dest & 3) << 15 |
               (uint32_t)((s->kcolor_sel & 31) | (s->kalpha_sel & 31) << 5) << 17 |
               (uint32_t)((s->tex_swap & 3) | (s->ras_swap & 3) << 2) << 27;
    }
}

/* The transform setup's words, as gx.vert's VS_ constants; none (zeros)
 * without the XF state. */
static void vs_words(const GXDrawState* st, const uint32_t* xf, uint32_t* w) {
    int i;
    memset(w, 0, VS_WORDS * sizeof(uint32_t));
    /* As many coordinates as the stages read: past the last read, none. */
    for (i = 0; i < st->num_stages && i < 16; i++) {
        const GXTevStage* s = &st->stage[i];
        if (s->tex_enable && (uint32_t)(s->texcoord & 7) + 1 > w[VS_TEX_IO]) w[VS_TEX_IO] = (uint32_t)(s->texcoord & 7) + 1;
    }
    if (!xf) return;
    w[0] = 1;
    for (i = 0; i < 4; i++) w[1 + i] = xf[GX_XF_MEM_WORDS + 0x0E + i];
    w[5] = (uint32_t)st->mtx_per_vertex;
#ifdef __ANDROID__
    /* The matrix indices read from the vertices, which hold XF's when the
     * vertices carry none: read from XF's registers instead, on an Adreno
     * 830, Ponyta's flames and some trees' leaves were stretched far out. */
    w[5] = 3;
#endif
    /* Whether texgen_program's setup holds (no normalized coordinates
     * post-transformed), with no texture matrix indices in the vertices. */
    w[6] = !(st->mtx_per_vertex & 2);
    if (xf[GX_XF_MEM_WORDS + 0x12] & 1) {
        int n = (int)(xf[GX_XF_MEM_WORDS + 0x3F] & 15);
        for (i = 0; i < n && i < 8; i++) {
            if ((xf[GX_XF_MEM_WORDS + 0x50 + i] >> 8) & 1) w[6] = 0;
        }
    }
}

static void save_cache(void) {
    size_t n = 0;
    void* data;
    char tmp[1040];
    FILE* f;
    if (!sCache || !sCachePath[0] || !SAVE_BEGIN()) return;
    sCacheDirty = 0;
    if (vkGetPipelineCacheData(sDev, sCache, &n, NULL) != VK_SUCCESS || !n || !(data = malloc(n))) {
        SAVE_END();
        return;
    }
    if (vkGetPipelineCacheData(sDev, sCache, &n, data) >= 0) {
        snprintf(tmp, sizeof(tmp), "%s.new", sCachePath);
        if ((f = fopen(tmp, "wb"))) {
            int ok = fwrite(data, 1, n, f) == n;
            ok &= fclose(f) == 0;
#ifdef _WIN32
            if (ok) MoveFileExA(tmp, sCachePath, MOVEFILE_REPLACE_EXISTING);
#else
            if (ok) rename(tmp, sCachePath);
#endif
        }
    }
    free(data);
    SAVE_END();
}

#ifdef _WIN32
static DWORD WINAPI spec_thread(LPVOID arg) {
#else
static void* spec_thread(void* arg) {
#endif
    (void)arg;
#ifdef __ANDROID__
    /* Below the game thread, which is kept to the fastest core: at the same
     * priority, a pipeline being made there held it up by 15 ms. */
    setpriority(PRIO_PROCESS, 0, 10);
#endif
    for (;;) {
        Spec* s;
        MAKE_LOCK();
        while (sMakeHead == sMakeTail) {
            if (sCacheDirty) {
                /* Made all there was to: kept for the next start. */
                MAKE_UNLOCK();
                save_cache();
                MAKE_LOCK();
                continue;
            }
            MAKE_WAIT();
        }
        s = &sSpecs[sMakeQueue[sMakeHead]];
        sMakeHead = (sMakeHead + 1) % SPEC_SLOTS;
        MAKE_UNLOCK();
        PIPE_STORE(s->p, make_pipeline(PK_GX, s->key, s->samples, s->words, s->vs));
        sCacheDirty = 1;
    }
#ifndef _WIN32
    return NULL;
#endif
}

/* The pipeline for the draw's setup, or VK_NULL_HANDLE while it's being
 * made (and then asked for). */
static VkPipeline spec_pipeline(const GXDrawState* st, const uint32_t* xf, uint32_t key) {
    uint32_t w[SPEC_WORDS], v[VS_WORDS], h = 2166136261u;
    int i, n;
    Spec* s;
    /* Indirect texturing is in the general shader only (it's rare). */
    if (!sSpecs || st->ind_any) return VK_NULL_HANDLE;
    spec_words(st, w);
    vs_words(st, xf, v);
    n = 2 + 3 * (int)w[0];
    for (i = 0; i < n; i++) h = (h ^ w[i]) * 16777619u;
    for (i = 0; i < VS_WORDS; i++) h = (h ^ v[i]) * 16777619u;
    h = ((h ^ key) * 16777619u ^ (uint32_t)sMsaa) * 16777619u;
    for (i = (int)(h & (SPEC_SLOTS - 1));; i = (i + 1) & (SPEC_SLOTS - 1)) {
        s = &sSpecs[i];
        if (!s->used) break;
        if (s->key == key && s->samples == sMsaa && memcmp(s->words, w, sizeof(w)) == 0 &&
            memcmp(s->vs, v, sizeof(v)) == 0)
            return PIPE_LOAD(s->p);
    }
    if (sSpecCount >= SPEC_SLOTS / 4 * 3) return VK_NULL_HANDLE;
    memcpy(s->words, w, sizeof(w));
    memcpy(s->vs, v, sizeof(v));
    s->key = key;
    s->samples = sMsaa;
    s->used = 1;
    sSpecCount++;
    MAKE_LOCK();
    sMakeQueue[sMakeTail] = i;
    sMakeTail = (sMakeTail + 1) % SPEC_SLOTS;
    MAKE_WAKE();
    MAKE_UNLOCK();
    return VK_NULL_HANDLE;
}

/* The pipeline cache, from vk_pipelines.bin beside settings.ini (a
 * driver's own check leaves out what another GPU or driver made), and the
 * thread making the setups' pipelines. */
static void spec_start(void) {
    VkPipelineCacheCreateInfo ci = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    const char* e = getenv("RECOMP_SETTINGS");
    char *slash, *back;
    void* data = NULL;
    FILE* f;
    if (e && *e) snprintf(sCachePath, sizeof(sCachePath) - 32, "%s", e);
#ifdef _WIN32
    else GetModuleFileNameA(NULL, sCachePath, sizeof(sCachePath) - 32);
#endif
    slash = strrchr(sCachePath, '/');
    back = strrchr(sCachePath, '\\');
    if (back > slash) slash = back;
    strcpy(slash ? slash + 1 : sCachePath, "vk_pipelines.bin");
    if ((f = fopen(sCachePath, "rb"))) {
        long n;
        fseek(f, 0, SEEK_END);
        n = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (n > 0 && (data = malloc((size_t)n)) && fread(data, 1, (size_t)n, f) == (size_t)n) {
            ci.initialDataSize = (size_t)n;
            ci.pInitialData = data;
        }
        fclose(f);
    }
    if (vkCreatePipelineCache(sDev, &ci, NULL, &sCache) != VK_SUCCESS) {
        ci.initialDataSize = 0;
        ci.pInitialData = NULL;
        if (vkCreatePipelineCache(sDev, &ci, NULL, &sCache) != VK_SUCCESS) sCache = VK_NULL_HANDLE;
    }
    free(data);
    if ((e = getenv("RECOMP_VK_GENERAL")) && *e == '1') return;
    sSpecs = (Spec*)calloc(SPEC_SLOTS, sizeof(Spec));
    if (!sSpecs) return;
#ifdef _WIN32
    InitializeCriticalSection(&sMakeLock);
    InitializeConditionVariable(&sMakeWake);
    CloseHandle(CreateThread(NULL, 0, spec_thread, NULL, 0, NULL));
#else
    {
        pthread_t t;
        pthread_create(&t, NULL, spec_thread, NULL);
        pthread_detach(t);
    }
#endif
}

static void draw_indexed(VkBuffer vb, VkBuffer ib, int32_t base_vertex, uint32_t first_index, uint32_t count) {
    if (sCmd.vb != vb) {
        VkDeviceSize zero = 0;
        vkCmdBindVertexBuffers(S->cb, 0, 1, &vb, &zero);
        sCmd.vb = vb;
    }
    if (sCmd.ib != ib) {
        vkCmdBindIndexBuffer(S->cb, ib, 0, VK_INDEX_TYPE_UINT32);
        sCmd.ib = ib;
    }
    vkCmdDrawIndexed(S->cb, count, 1, first_index, base_vertex, 0);
}

/* ------------------------------------------------------------------------ */
/* The second screen (the AYN Thor's lower one, on Android, whose window
 * second_screen_android.c gives): the minimap, drawn there as well. Its
 * draws (GXDrawState.screen 1) are kept as they come, and drawn again as
 * the frame is presented, into sMap, with the minimap's part of the EFB
 * (kGxMinimapRect) stretched over it; drawn into the EFB too only if the setting
 * says so. sMap is drawn into sBottom, the screen's picture, which is
 * copied to its swapchain's image with the main one's. On Windows,
 * RECOMP_GX_SECOND=<w>x<h> draws sBottom at that size without a screen,
 * for RECOMP_GX_DUMP (second_*.bmp). */
typedef struct MapDraw {
    VkPipeline p;
    VkBuffer vb, ib, data;
    uint32_t pc[12], first, count;
    int32_t base;
    float vp[6];
    int sc[4], cull, dtest, dwrite, dfunc;
} MapDraw;
static MapDraw* sMapDraws;
static int sMapCount, sMapCap;
#define kMapRect kGxMinimapRect
static const VkClearColorValue kBottomColor = {{0.05f, 0.06f, 0.09f, 1}};

static void map_keep(VkPipeline p, const uint32_t* pc, const GXDrawState* st, VkBuffer vb, VkBuffer ib, int32_t base,
                     uint32_t first, uint32_t count) {
    MapDraw* d;
    if (sMapCount == sMapCap) {
        sMapCap = sMapCap ? sMapCap * 2 : 64;
        sMapDraws = (MapDraw*)realloc(sMapDraws, (size_t)sMapCap * sizeof(MapDraw));
    }
    d = &sMapDraws[sMapCount++];
    d->p = p;
    memcpy(d->pc, pc, sizeof(d->pc));
    d->vb = vb;
    d->ib = ib;
    d->data = S->data.buf; /* the offsets in pc[0..1] are into it */
    d->base = base;
    d->first = first;
    d->count = count;
    memcpy(d->vp, st->viewport, sizeof(d->vp));
    memcpy(d->sc, st->scissor, sizeof(d->sc));
    d->cull = (int)kCull[st->cull_mode & 3];
    d->dtest = st->z_enable;
    d->dwrite = st->z_enable && st->z_update;
    d->dfunc = (int)kDepthFunc[st->z_func & 7];
}

/* The minimap at its shape on the game's screen, as large as fits in
 * rectangle in (x, y, w, h) at k of its size, centred: r. */
static void fit_map(const int* in, double k, int* r) {
    double aspect = (kMapRect[2] - kMapRect[0]) * sPixelAspect / (kMapRect[3] - kMapRect[1]);
    double mh = in[3] * k, mw = mh * aspect;
    if (mw > in[2] * k) {
        mw = in[2] * k;
        mh = mw / aspect;
    }
    r[0] = in[0] + (int)((in[2] - mw) / 2);
    r[1] = in[1] + (int)((in[3] - mh) / 2);
    r[2] = (int)mw;
    r[3] = (int)mh;
}

/* The minimap's shape (width over height), for friends.c to make room. */
double second_map_aspect(void) { return (kMapRect[2] - kMapRect[0]) * sPixelAspect / (kMapRect[3] - kMapRect[1]); }

/* Where the minimap goes in sBottom (x, y, w, h) with no friends list:
 * as large as fits. */
static void bottom_layout(int w, int h, int* r) {
    int in[4];
    in[0] = in[1] = 0;
    in[2] = w;
    in[3] = h;
    fit_map(in, 0.92, r);
}

static void map_images(int w, int h) {
    if (sMap.w == w && sMap.h == h && sMapSamples == sMsaa) return;
    drop_image(&sMap);
    drop_image(&sMapMs);
    drop_image(&sMapDepth);
    sMap = make_image(w, h, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                      VK_SAMPLE_COUNT_1_BIT);
    if (sMsaa > 1)
        sMapMs = make_image(w, h, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                            (VkSampleCountFlagBits)sMsaa);
    sMapDepth = make_image(w, h, 1, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                           (VkSampleCountFlagBits)sMsaa);
    sMapSamples = sMsaa;
    sTex[TEX_MAP].im = sMap; /* a copy of the handles, as for TEX_EFB */
    sTex[TEX_MAP].sampler = sLinearClamp;
    set_texture(TEX_MAP);
}

/* The minimap's draws again, into sMap. */
static void draw_map(void) {
    VkRenderingAttachmentInfo ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkRenderingAttachmentInfo da = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    float sx = (float)sMap.w / (kMapRect[2] - kMapRect[0]), sy = (float)sMap.h / (kMapRect[3] - kMapRect[1]);
    int i;
    transition(S->cb, &sMap, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    if (sMapMs.img) transition(S->cb, &sMapMs, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    transition(S->cb, &sMapDepth, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ca.clearValue.color = kBottomColor;
    if (sMapMs.img) {
        ca.imageView = sMapMs.view;
        ca.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        ca.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
        ca.resolveImageView = sMap.view;
        ca.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    } else {
        ca.imageView = sMap.view;
    }
    da.imageView = sMapDepth.view;
    da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; /* to the far plane, 0 (reversed) */
    da.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    ri.renderArea.extent.width = (uint32_t)sMap.w;
    ri.renderArea.extent.height = (uint32_t)sMap.h;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    ri.pDepthAttachment = &da;
    vkCmdBeginRendering(S->cb, &ri);
    for (i = 0; i < sMapCount; i++) {
        const MapDraw* d = &sMapDraws[i];
        float x0 = (d->vp[0] - kMapRect[0]) * sx, x1 = (d->vp[0] + d->vp[2] - kMapRect[0]) * sx;
        float y0 = (d->vp[1] - kMapRect[1]) * sy, y1 = (d->vp[1] + d->vp[3] - kMapRect[1]) * sy;
        float zn = d->vp[4] < 0 ? 0 : d->vp[4], zf = d->vp[5] > 1 ? 1 : d->vp[5];
        int c0 = (int)floor((d->sc[0] - kMapRect[0]) * sx + 0.5f), c1 = (int)floor((d->sc[2] - kMapRect[0]) * sx + 0.5f);
        int r0 = (int)floor((d->sc[1] - kMapRect[1]) * sy + 0.5f), r1 = (int)floor((d->sc[3] - kMapRect[1]) * sy + 0.5f);
        if (d->data != S->data.buf) continue; /* the data ring was replaced since: its state is gone */
        if (c1 > sMap.w) c1 = sMap.w;
        if (r1 > sMap.h) r1 = sMap.h;
        use_pipeline(d->p);
        set_viewport(x0, y1, x1 - x0, y0 - y1, 1.0f - zf, 1.0f - zn);
        set_scissor(c0, r0, c1 - c0, r1 - r0);
        set_cull(d->cull);
        set_depth(d->dtest, d->dwrite, d->dfunc);
        bind_data((VkDeviceSize)d->pc[0] * 4, (VkDeviceSize)d->pc[1] * 4);
        push(d->pc, sizeof(d->pc));
        draw_indexed(d->vb, d->ib, d->base, d->first, d->count);
    }
    vkCmdEndRendering(S->cb);
    transition(S->cb, &sMap, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

#ifdef __ANDROID__
int second_touch_next(int* action, float* x, float* y); /* second_screen_android.c */

/* The second screen's touches, to the friends list. */
static void list_touches(void) {
    int action;
    float x, y;
    while (second_touch_next(&action, &x, &y)) friends_touch(action, x, y);
}
#else
static void list_touches(void) {}
#endif

/* The second screen's picture, into sBottom, for this frame. */
static void draw_bottom(void) {
    VkRenderingAttachmentInfo ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    int r[4], in[4], changed[5];
    const uint32_t* list;
    Quad q;
    if (sBottom.w != sBottomW || sBottom.h != sBottomH) {
        drop_image(&sBottom);
        sBottom = make_image(sBottomW, sBottomH, 1, VK_FORMAT_R8G8B8A8_UNORM,
                             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_SAMPLE_COUNT_1_BIT);
    }
    /* The Pad's pages with the friends list (friends.c), uploaded only when
     * they changed, and the map on them where they say. */
    list_touches();
    list = friends_draw(sBottomW, sBottomH, changed, in);
#ifdef __ANDROID__
    {
        /* With the setting off, the map alone on the pause menu's sky;
         * switched, all uploaded. */
        static int was = 1;
        int on = g_settings.friends_list;
        if (!on) {
            list = friends_sky(sBottomW, sBottomH, changed);
            bottom_layout(sBottomW, sBottomH, in);
        }
        if (on != was) changed[0] = 1;
        was = on;
    }
#endif
    if (list) {
        Tex* t = &sTex[TEX_LIST];
        if (t->im.w != sBottomW || t->im.h != sBottomH) {
            drop_image(&t->im);
            t->im = make_image(sBottomW, sBottomH, 1, VK_FORMAT_B8G8R8A8_UNORM,
                               VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SAMPLE_COUNT_1_BIT);
            t->sampler = sLinearClamp;
            set_texture(TEX_LIST);
            changed[0] = 1;
        }
        if (changed[0] == 1) {
            upload(S->cb, t, sBottomW, sBottomH, 1, (const uint8_t*)list);
        } else if (changed[0] == 2 && changed[1] >= 0 && changed[2] >= 0 && changed[3] > 0 && changed[4] > 0 &&
                   changed[1] + changed[3] <= sBottomW && changed[2] + changed[4] <= sBottomH) {
            upload_part(S->cb, t, sBottomW, changed + 1, list);
        }
        memcpy(r, in, sizeof(r));
    } else {
        bottom_layout(sBottomW, sBottomH, r);
    }
    if (sMapCount && r[2] > 0 && r[3] > 0) {
        map_images(r[2], r[3]);
        draw_map();
    }
    transition(S->cb, &sBottom, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    ca.imageView = sBottom.view;
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ca.clearValue.color = kBottomColor;
    ri.renderArea.extent.width = (uint32_t)sBottomW;
    ri.renderArea.extent.height = (uint32_t)sBottomH;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    vkCmdBeginRendering(S->cb, &ri);
    set_scissor(0, 0, sBottomW, sBottomH);
    set_cull(VK_CULL_MODE_NONE);
    set_depth(0, 0, VK_COMPARE_OP_ALWAYS);
    memset(&q, 0, sizeof(q));
    q.src[2] = q.src[3] = 1;
    if (list) {
        use_pipeline(pipeline(PK_BLIT, 0));
        set_viewport(0, 0, (float)sBottomW, (float)sBottomH, 0, 1);
        q.tex = TEX_LIST;
        quad(&q);
    }
    if (sMapCount && r[2] > 0 && r[3] > 0) {
        use_pipeline(pipeline(PK_BLIT, 0));
        set_viewport((float)r[0], (float)r[1], (float)r[2], (float)r[3], 0, 1);
        q.tex = TEX_MAP;
        quad(&q);
    }
    vkCmdEndRendering(S->cb);
    transition(S->cb, &sBottom, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
}

/* Copies a picture to a swapchain's image just acquired, recorded into
 * S->out, and readies it for presenting. */
static void copy_to_chain(const Image* from, VkImage to, int w, int h) {
    VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    VkDependencyInfo d = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    VkImageBlit blit;
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = to;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    d.imageMemoryBarrierCount = 1;
    d.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(S->out, &d);
    memset(&blit, 0, sizeof(blit));
    blit.srcSubresource.aspectMask = blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.srcSubresource.layerCount = blit.dstSubresource.layerCount = 1;
    blit.srcOffsets[1].x = from->w;
    blit.srcOffsets[1].y = from->h;
    blit.dstOffsets[1].x = w;
    blit.dstOffsets[1].y = h;
    blit.srcOffsets[1].z = blit.dstOffsets[1].z = 1;
    vkCmdBlitImage(S->out, from->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, to, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                   &blit, from->w == w && from->h == h ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
    b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstAccessMask = 0;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier2(S->out, &d);
}

#ifdef __ANDROID__
struct ANativeWindow* second_window_take(unsigned* gen); /* second_screen_android.c */
void second_window_drop(void);
unsigned second_window_gen(void);
void second_screen_apply(void);
static unsigned sSecondGen = ~0u; /* the window's, as last taken */

static void drop_chain2(void) {
    uint32_t i;
    if (sChain2) vkDestroySwapchainKHR(sDev, sChain2, NULL);
    for (i = 0; i < sChain2Count; i++) vkDestroySemaphore(sDev, sRendered2[i], NULL);
    sChain2 = VK_NULL_HANDLE;
    sChain2Count = 0;
    sBottomW = sBottomH = 0;
}

static void drop_surface2(void) {
    drop_chain2();
    if (sSurface2) vkDestroySurfaceKHR(sInstance, sSurface2, NULL);
    sSurface2 = VK_NULL_HANDLE;
    second_window_drop();
}

static void make_chain2(void) {
    VkSurfaceCapabilitiesKHR caps;
    VkSurfaceFormatKHR formats[64];
    uint32_t nf = 64, i, count;
    VkBool32 present = VK_FALSE;
    VkSwapchainCreateInfoKHR ci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    vkGetPhysicalDeviceSurfaceSupportKHR(sGpu, sQueueFamily, sSurface2, &present);
    if (!present || vkGetPhysicalDeviceSurfaceCapabilitiesKHR(sGpu, sSurface2, &caps) != VK_SUCCESS) return;
    vkGetPhysicalDeviceSurfaceFormatsKHR(sGpu, sSurface2, &nf, formats);
    if (!nf || caps.currentExtent.width == 0xFFFFFFFFu || !caps.currentExtent.width || !caps.currentExtent.height) return;
    ci.surface = sSurface2;
    count = caps.minImageCount < 3 ? 3 : caps.minImageCount;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
    if (count > 8) count = 8;
    ci.minImageCount = count;
    ci.imageFormat = formats[0].format;
    ci.imageColorSpace = formats[0].colorSpace;
    for (i = 0; i < nf; i++) {
        if (formats[i].format == VK_FORMAT_B8G8R8A8_UNORM || formats[i].format == VK_FORMAT_R8G8B8A8_UNORM) {
            ci.imageFormat = formats[i].format;
            ci.imageColorSpace = formats[i].colorSpace;
            break;
        }
    }
    ci.imageExtent = caps.currentExtent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
                                                                                       : caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)) {
        uint32_t a;
        for (a = VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR; a < VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR; a <<= 1) {
            if (caps.supportedCompositeAlpha & a) break;
        }
        ci.compositeAlpha = (VkCompositeAlphaFlagBitsKHR)a;
    }
    ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    ci.clipped = VK_TRUE;
    if (vkCreateSwapchainKHR(sDev, &ci, NULL, &sChain2) != VK_SUCCESS) {
        sChain2 = VK_NULL_HANDLE;
        return;
    }
    sChain2Count = 8;
    vkGetSwapchainImagesKHR(sDev, sChain2, &sChain2Count, sChain2Images);
    for (i = 0; i < sChain2Count; i++) CHECK(vkCreateSemaphore(sDev, &si, NULL, &sRendered2[i]));
    sChain2W = sBottomW = (int)ci.imageExtent.width;
    sChain2H = sBottomH = (int)ci.imageExtent.height;
    fprintf(stderr, "vulkan: second screen %dx%d, transform %u\n", sChain2W, sChain2H, (unsigned)caps.currentTransform);
}

/* The second screen's swapchain, made again when its window changed or
 * it's out of date. */
static void second_chain(void) {
    unsigned gen;
    second_screen_apply();
    gen = second_window_gen();
    if (gen == sSecondGen && !sChain2Stale) return;
    wait_idle();
    sChain2Stale = 0;
    if (gen != sSecondGen) {
        VkAndroidSurfaceCreateInfoKHR sci = {VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
        drop_surface2();
        sci.window = second_window_take(&sSecondGen);
        if (sci.window && vkCreateAndroidSurfaceKHR(sInstance, &sci, NULL, &sSurface2) != VK_SUCCESS)
            sSurface2 = VK_NULL_HANDLE;
        if (!sSurface2) second_window_drop();
    } else {
        drop_chain2();
    }
    if (sSurface2) make_chain2();
}

/* The second screen's image, acquired and drawn (from what: sBottom, or
 * the game's picture swapped), with the main one's. */
static void second_out(const Image* what) {
    VkResult r;
    if (!sChain2 || what->w != sChain2W || what->h != sChain2H) return;
    r = vkAcquireNextImageKHR(sDev, sChain2, 0, S->acquired2, VK_NULL_HANDLE, &sImage2);
    if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) {
        copy_to_chain(what, sChain2Images[sImage2], sChain2W, sChain2H);
        sAcquired2 = 1;
    } else if (r != VK_NOT_READY && r != VK_TIMEOUT) {
        sChain2Stale = 1;
    }
}
#else
static void second_chain(void) {
    static int checked;
    const char* e;
    if (checked) return;
    checked = 1;
    if ((e = getenv("RECOMP_GX_SECOND")) && sscanf(e, "%dx%d", &sBottomW, &sBottomH) != 2) sBottomW = sBottomH = 0;
}
static void second_out(const Image* what) { (void)what; }
#endif

static void draw_common(const GXDrawState* st, const uint32_t* xf, VkBuffer vb, VkBuffer ib, int32_t base_vertex,
                        uint32_t first_index, uint32_t count) {
    uint32_t pc[12];
    int i;
    uint32_t key = gx_key(st);
    VkPipeline sp = spec_pipeline(st, xf, key);
    /* The minimap, with a second screen: drawn there, and here too if
     * the setting says so. */
    int keep = st->screen == 1 && sBottomW, here = !keep || g_settings.minimap_top;
    /* Depth is used by a test other than "always" or by writes. */
    if (here) begin_efb(st->z_enable && ((st->z_func & 7) != 7 || st->z_update));
    if (!sp) sp = pipeline(PK_GX, key);

    use_pipeline(sp);
    {
        /* Edges are scaled and rounded, as gl_backend.c does; flipped,
         * so row 0 is the EFB's top. On the canvas, with its clip space
         * scaled (pc[10..11]). */
        float vp[4], k[2], x0, x1, y0, y1;
        int sc[4];
        float zn = st->viewport[4] < 0 ? 0 : st->viewport[4], zf = st->viewport[5] > 1 ? 1 : st->viewport[5];
        gx_canvas_draw(st, sMX, sMY, vp, sc, k);
        x0 = (float)VX(vp[0]);
        x1 = (float)VX(vp[0] + vp[2]);
        y0 = (float)VY(vp[1]);
        y1 = (float)VY(vp[1] + vp[3]);
        set_viewport(x0, y1, x1 - x0, y0 - y1, 1.0f - zf, 1.0f - zn);
        set_scissor(VX(sc[0]), VY(sc[1]), VX(sc[2]) - VX(sc[0]), VY(sc[3]) - VY(sc[1]));
        memcpy(&pc[10], k, sizeof(k));
    }
    set_cull((int)kCull[st->cull_mode & 3]);
    set_depth(st->z_enable, st->z_enable && st->z_update, (int)kDepthFunc[st->z_func & 7]);
    pc[0] = write_xf(xf);
    pc[1] = write_tev(st);
    if (sXfAt < 0) pc[0] = write_xf(xf); /* the data buffer filled up and was replaced */
    bind_data((VkDeviceSize)pc[0] * 4, (VkDeviceSize)pc[1] * 4);
    for (i = 0; i < 8; i++) {
        pc[2 + i] = st->texture[i];
        if (st->texture[i]) sTex[st->texture[i]].used = sSerial;
    }
    push(pc, sizeof(pc));
    if (keep) map_keep(sp, pc, st, vb, ib, base_vertex, first_index, count);
    if (here) draw_indexed(vb, ib, base_vertex, first_index, count);
}

/* Video memory the CPU can write (resizable BAR): the recorded frame is
 * written straight into it, rather than copied there from a staging
 * buffer. */
static int sRebar;

static void find_rebar(void) {
    uint32_t i;
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (i = 0; i < sMemProps.memoryTypeCount; i++) {
        if ((sMemProps.memoryTypes[i].propertyFlags & want) == want &&
            sMemProps.memoryHeaps[sMemProps.memoryTypes[i].heapIndex].size > ((VkDeviceSize)1 << 30))
            sRebar = 1;
    }
}

/* The vertex arena (gxb_arena): vertices kept in video memory from frame
 * to frame, in spans. The free spans are kept in address order, joined
 * where they touch; a freed span joins them once the GPU has finished the
 * frames that could draw from it. */
typedef struct Span {
    int first, n;
    uint64_t serial;
} Span;
static Buffer sArena;
static int sArenaVerts;
/* The frame the arena last grew in: until the GPU has finished it, writes
 * go by the staging ring, after the copy of the old arena, not into the
 * mapped arena where the copy could overwrite them. */
static uint64_t sArenaGrew;
static Span *sSpans, *sFreed;
static int sSpanCount, sSpanCap, sFreedCount, sFreedCap;

static void span_add(int first, int n) {
    int lo = 0, hi = sSpanCount;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (sSpans[mid].first < first) lo = mid + 1;
        else hi = mid;
    }
    if (lo > 0 && sSpans[lo - 1].first + sSpans[lo - 1].n == first) {
        sSpans[lo - 1].n += n;
        if (lo < sSpanCount && sSpans[lo - 1].first + sSpans[lo - 1].n == sSpans[lo].first) {
            sSpans[lo - 1].n += sSpans[lo].n;
            memmove(sSpans + lo, sSpans + lo + 1, (size_t)(sSpanCount - lo - 1) * sizeof(Span));
            sSpanCount--;
        }
        return;
    }
    if (lo < sSpanCount && first + n == sSpans[lo].first) {
        sSpans[lo].first = first;
        sSpans[lo].n += n;
        return;
    }
    if (sSpanCount == sSpanCap) {
        sSpanCap = sSpanCap ? sSpanCap * 2 : 256;
        sSpans = (Span*)realloc(sSpans, (size_t)sSpanCap * sizeof(Span));
    }
    memmove(sSpans + lo + 1, sSpans + lo, (size_t)(sSpanCount - lo) * sizeof(Span));
    sSpans[lo].first = first;
    sSpans[lo].n = n;
    sSpanCount++;
}

/* Room for at least `need` more vertices: a bigger arena, with the old
 * one's contents, copied by the GPU: copied by the CPU, out of a mapped
 * arena in video memory (Resizable BAR) it took 1.3 s at 64 MB, then
 * 2.6 s, on an RTX 5070. */
static void arena_grow(int need) {
    VkMemoryPropertyFlags props = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    /* What the park comes to: growing costs a frame 25 ms or more, most of
     * it the system clearing the new memory, so it's done at the start. */
    int verts = sArenaVerts ? sArenaVerts * 2 : 1 << 21;
    Buffer b;
    while (verts < sArenaVerts + need) verts *= 2;
    if (sRebar) props |= VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    b = make_buffer((VkDeviceSize)verts * sizeof(GXVertex),
                    VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                    props);
    if (sArena.buf) {
        VkBufferCopy c;
        c.srcOffset = c.dstOffset = 0;
        c.size = (VkDeviceSize)sArenaVerts * sizeof(GXVertex);
        vkCmdCopyBuffer(S->up, sArena.buf, b.buf, 1, &c);
        memory_barrier(S->up); /* before the writes recorded after it */
        sArenaCopied = 1;
        sArenaGrew = sSerial;
        drop_buffer(&sArena);
    }
    span_add(sArenaVerts, verts - sArenaVerts);
    sArena = b;
    sArenaVerts = verts;
}

static int vk_arena_alloc(int n) {
    static uint64_t reclaimed;
    int i, first;
    if (n <= 0) return 0;
    if (sDone != reclaimed) {
        reclaimed = sDone;
        for (i = 0; i < sFreedCount;) {
            if (sFreed[i].serial <= sDone) {
                span_add(sFreed[i].first, sFreed[i].n);
                sFreed[i] = sFreed[--sFreedCount];
            } else i++;
        }
    }
    for (;;) {
        for (i = 0; i < sSpanCount && sSpans[i].n < n; i++) {}
        if (i < sSpanCount) break;
        arena_grow(n);
    }
    first = sSpans[i].first;
    sSpans[i].first += n;
    sSpans[i].n -= n;
    if (!sSpans[i].n) {
        memmove(sSpans + i, sSpans + i + 1, (size_t)(sSpanCount - i - 1) * sizeof(Span));
        sSpanCount--;
    }
    return first;
}

static void vk_arena_write(int first, const GXVertex* v, int n) {
    VkDeviceSize bytes = (VkDeviceSize)n * sizeof(GXVertex), off;
    VkBufferCopy c;
    VkBuffer from;
    if (n <= 0) return;
    if (sArena.map && sDone >= sArenaGrew) {
        memcpy(sArena.map + (size_t)first * sizeof(GXVertex), v, (size_t)bytes);
        return;
    }
    memcpy(staging(bytes, &from, &off), v, (size_t)bytes);
    c.srcOffset = off;
    c.dstOffset = (VkDeviceSize)first * sizeof(GXVertex);
    c.size = bytes;
    vkCmdCopyBuffer(S->up, from, sArena.buf, 1, &c);
    sArenaCopied = 1;
}

static void vk_arena_free(int first, int n) {
    if (n <= 0) return;
    if (sFreedCount == sFreedCap) {
        sFreedCap = sFreedCap ? sFreedCap * 2 : 256;
        sFreed = (Span*)realloc(sFreed, (size_t)sFreedCap * sizeof(Span));
    }
    sFreed[sFreedCount].first = first;
    sFreed[sFreedCount].n = n;
    sFreed[sFreedCount].serial = sSerial;
    sFreedCount++;
}

static void vk_draw(const GXDrawState* st, const GXVertex* v, int first_vertex, int vertices, const uint32_t* index,
                    int first_index, int count) {
    VkDeviceSize voff, ioff;
    if (!v) {
        /* In the arena, by index from its start. */
        memcpy(ring(RING_IDX, (VkDeviceSize)count * 4, 4, &ioff), index + first_index, (size_t)count * 4);
        draw_common(st, st->xf, sArena.buf, S->idx.buf, 0, (uint32_t)(ioff / 4), (uint32_t)count);
        return;
    }
    memcpy(ring(RING_VERTS, (VkDeviceSize)vertices * sizeof(GXVertex), sizeof(GXVertex), &voff), v + first_vertex,
           (size_t)vertices * sizeof(GXVertex));
    memcpy(ring(RING_IDX, (VkDeviceSize)count * 4, 4, &ioff), index + first_index, (size_t)count * 4);
    draw_common(st, st->xf, S->verts.buf, S->idx.buf, (int32_t)(voff / sizeof(GXVertex)), (uint32_t)(ioff / 4),
                (uint32_t)count);
}

/* The recorded frame's vertices and indices, in device memory; a few
 * kept, as frames in flight may still draw from the last ones. */
typedef struct FrameBuf {
    Buffer vb, ib;
    uint64_t used;
    int arena; /* its indices are into the vertex arena */
} FrameBuf;
static FrameBuf sFrameBufs[SLOTS + 1];
static FrameBuf* sFrameBuf;
static void vk_frame_data(const GXVertex* v, int vertices, const uint32_t* index, int count) {
    VkDeviceSize vbytes = (VkDeviceSize)vertices * sizeof(GXVertex), ibytes = (VkDeviceSize)count * 4, off;
    VkBufferCopy c;
    FrameBuf* f = NULL;
    int i;
    for (i = 0; i < SLOTS + 1 && !f; i++) {
        if (&sFrameBufs[i] != sFrameBuf && sFrameBufs[i].used <= sDone) f = &sFrameBufs[i];
    }
    if (!f) {
        /* All in flight: wait for the GPU. */
        wait_idle();
        sDone = sSerial - 1;
        f = &sFrameBufs[0] != sFrameBuf ? &sFrameBufs[0] : &sFrameBufs[1];
    }
    if (f->vb.size < vbytes || f->ib.size < ibytes) {
        VkMemoryPropertyFlags props = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        if (sRebar) props |= VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        drop_buffer(&f->vb);
        drop_buffer(&f->ib);
        f->vb = make_buffer(vbytes + vbytes / 4 + 4096, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                            props);
        f->ib = make_buffer(ibytes + ibytes / 4 + 4096, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                            props);
    }
    f->arena = !v;
    if (f->vb.map) {
        /* No frame in flight draws from it (above). */
        if (vbytes) memcpy(f->vb.map, v, (size_t)vbytes);
        memcpy(f->ib.map, index, (size_t)ibytes);
        f->used = sSerial;
        sFrameBuf = f;
        return;
    }
    if (vbytes) {
        memcpy(ring(RING_STAGING, vbytes, 16, &off), v, (size_t)vbytes);
        c.srcOffset = off;
        c.dstOffset = 0;
        c.size = vbytes;
        vkCmdCopyBuffer(S->up, S->staging.buf, f->vb.buf, 1, &c);
    }
    if (ibytes) {
        memcpy(ring(RING_STAGING, ibytes, 16, &off), index, (size_t)ibytes);
        c.srcOffset = off;
        c.dstOffset = 0;
        c.size = ibytes;
        vkCmdCopyBuffer(S->up, S->staging.buf, f->ib.buf, 1, &c);
    }
    memory_barrier(S->up);
    f->used = sSerial;
    sFrameBuf = f;
}

static void vk_draw_recorded(const GXDrawState* st, const uint32_t* xf, int first_vertex, int first_index, int count) {
    VkBuffer vb;
    if (!sFrameBuf) return;
    vb = sFrameBuf->arena ? sArena.buf : sFrameBuf->vb.buf;
    if (!vb) return;
    sFrameBuf->used = sSerial;
    draw_common(st, xf, vb, sFrameBuf->ib.buf, first_vertex, (uint32_t)first_index, (uint32_t)count);
}

static void vk_clear(int color, int alpha, int depth, uint32_t rgba, uint32_t z) {
    Quad q;
    if (!color && !alpha && !depth) return;
    memset(&q, 0, sizeof(q));
    q.color[0] = ((rgba >> 24) & 0xFF) / 255.0f;
    q.color[1] = ((rgba >> 16) & 0xFF) / 255.0f;
    q.color[2] = ((rgba >> 8) & 0xFF) / 255.0f;
    q.color[3] = (rgba & 0xFF) / 255.0f;
    q.depth = (float)(1.0 - z / 16777215.0);
    if (!sRendering) {
        /* Outside drawing (after a copy, as a rule): what is cleared whole
         * is cleared as drawing starts. */
        if (color && alpha) {
            sClearColor = 1;
            sClearShown = 0;
            memcpy(sClearColorTo.float32, q.color, sizeof(sClearColorTo.float32));
            color = alpha = 0;
        }
        if (depth) {
            sClearDepth = 1;
            sClearDepthTo = q.depth;
            depth = 0;
        }
        if (!color && !alpha) return;
    }
    begin_efb(depth);
    use_pipeline(pipeline(PK_FILL, (color ? 7u : 0u) | (alpha ? 8u : 0u)));
    set_viewport(0, 0, (float)sEfb.w, (float)sEfb.h, 0, 1);
    set_scissor(0, 0, sEfb.w, sEfb.h);
    set_cull(VK_CULL_MODE_NONE);
    set_depth(depth, depth, VK_COMPARE_OP_ALWAYS);
    quad(&q);
}

static int vk_copy_target(uint32_t addr, int w, int h) {
    CopyTarget* t;
    int i;
    for (i = 0; i < sTargetCount; i++) {
        if (sTargets[i].addr == addr && sTargets[i].w == w && sTargets[i].h == h) return i;
    }
    if (sTargetCount == 64) return -1;
    t = &sTargets[sTargetCount];
    t->addr = addr;
    t->w = w;
    t->h = h;
    t->handle = new_handle();
    /* Made ready to sample in the frame's commands, where the EFB's
     * clear is too, as nothing has drawn into it yet. */
    end_efb();
    make_target_image(t);
    return sTargetCount++;
}

static uint32_t vk_copy_texture(int target) { return sTargets[target].handle; }

/* The copy's texture read back as the frame so far leaves it (a copy the
 * game made this frame, for its CPU to read): what the frame has
 * recorded is submitted with the read and waited for, and the frame goes
 * on in fresh command buffers. It's rare: gx_frames.c's write backs. */
static uint8_t* vk_copy_read(int target, int* w, int* h) {
    static VkFence fence;
    static Buffer buf;
    Image* im = &sTex[sTargets[target].handle].im;
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    VkCommandBuffer cbs[2];
    VkBufferImageCopy r;
    VkImageLayout was = im->layout;
    VkDeviceSize bytes = (VkDeviceSize)im->w * im->h * 4;
    uint8_t* out;
    if (!fence) {
        VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        CHECK(vkCreateFence(sDev, &fci, NULL, &fence));
    }
    if (buf.size < bytes) {
        drop_buffer(&buf);
        buf = make_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                              VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    }
    end_efb();
    transition(S->cb, im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    memset(&r, 0, sizeof(r));
    r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    r.imageSubresource.layerCount = 1;
    r.imageExtent.width = (uint32_t)im->w;
    r.imageExtent.height = (uint32_t)im->h;
    r.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(S->cb, im->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf.buf, 1, &r);
    transition(S->cb, im, was);
    if (sArenaCopied) {
        memory_barrier(S->up);
        sArenaCopied = 0;
    }
    CHECK(vkEndCommandBuffer(S->up));
    CHECK(vkEndCommandBuffer(S->cb));
    cbs[0] = S->up;
    cbs[1] = S->cb;
    si.commandBufferCount = 2;
    si.pCommandBuffers = cbs;
    present_done(); /* (Android presents from another thread: the queue is ours again) */
    CHECK(vkQueueSubmit(sQueue, 1, &si, fence));
    CHECK(vkWaitForFences(sDev, 1, &fence, VK_TRUE, UINT64_MAX));
    CHECK(vkResetFences(sDev, 1, &fence));
    /* The rest of the frame, as begin_slot starts one. */
    CHECK(vkResetCommandPool(sDev, S->pool, 0));
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    CHECK(vkBeginCommandBuffer(S->up, &bi));
    CHECK(vkBeginCommandBuffer(S->cb, &bi));
    forget();
    sXfAt = sTevAt = -1;
    sBound[0] = sBound[1] = 0;
    bind_sets();
    vkCmdSetFrontFace(S->cb, VK_FRONT_FACE_COUNTER_CLOCKWISE);
    out = (uint8_t*)malloc((size_t)bytes);
    if (out) memcpy(out, buf.map, (size_t)bytes);
    *w = im->w;
    *h = im->h;
    return out;
}

/* Draws the EFB rectangle into the copy's texture. */
static void vk_copy(int target, int x, int y, int w, int h, int fmt, int intensity, int efb_alpha) {
    Tex* t = &sTex[sTargets[target].handle];
    VkRenderingAttachmentInfo ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    Quad q;
    end_efb();
    efb_settle();
    transition(S->cb, &sEfb, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    transition(S->cb, &t->im, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    ca.imageView = t->im.view;
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ri.renderArea.extent.width = (uint32_t)t->im.w;
    ri.renderArea.extent.height = (uint32_t)t->im.h;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    vkCmdBeginRendering(S->cb, &ri);
    use_pipeline(pipeline(PK_COPY, 0));
    set_viewport(0, 0, (float)t->im.w, (float)t->im.h, 0, 1);
    set_scissor(0, 0, t->im.w, t->im.h);
    set_cull(VK_CULL_MODE_NONE);
    set_depth(0, 0, VK_COMPARE_OP_ALWAYS);
    memset(&q, 0, sizeof(q));
    /* Row 0 of the texture is the top row of the rectangle, as the EFB has it. */
    gx_canvas_rect(sMX, sMY, &x, &y, &w, &h);
    q.src[0] = (float)x / (EFB_W + 2 * sMX);
    q.src[1] = (float)y / (EFB_H + 2 * sMY);
    q.src[2] = (float)(x + w) / (EFB_W + 2 * sMX);
    q.src[3] = (float)(y + h) / (EFB_H + 2 * sMY);
    q.mode[0] = fmt;
    q.mode[1] = intensity;
    q.mode[2] = efb_alpha;
    q.tex = TEX_EFB;
    quad(&q);
    vkCmdEndRendering(S->cb);
    transition(S->cb, &t->im, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    t->used = sSerial;
}

static int vk_present(int x, int y, int cw, int ch, int may_drop) {
    int w, h, gw, gh, dw, dh, acquired = 0, dropped = 0, dump, swap = 0;
    uint32_t image = 0;
    double aspect;
    Quad q;
    win_size(&w, &h);
    end_efb();
    gw = w;
    gh = h;
    dump = sDumpDir && sPresented >= sDumpFrom && sPresented % (unsigned)sDumpEvery == 0 && w > 0 && h > 0;
    if (w > 0 && h > 0) {
        const uint32_t* menu;
        VkRenderingAttachmentInfo ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
        present_done(); /* which may have found the swapchain out of date */
        if (sChainStale || sChainW != w || sChainH != h) make_swapchain(w, h);
        second_chain();
#ifdef __ANDROID__
        /* The screens swapped: the game's picture (gw by gh) on the second
         * screen, the Pad's pages on the main one. */
        swap = sChain2 && second_screens_swapped();
        if (sChain2) {
            sBottomW = swap ? w : sChain2W;
            sBottomH = swap ? h : sChain2H;
        }
        if (swap) {
            gw = sChain2W;
            gh = sChain2H;
        }
#endif
        if (sWin.w != gw || sWin.h != gh) {
            drop_image(&sWin);
            sWin = make_image(gw, gh, 1, VK_FORMAT_R8G8B8A8_UNORM,
                              VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_SAMPLE_COUNT_1_BIT);
        }
        /* The settings menu, uploaded before the frame's commands run. */
        menu = menu_draw(gw, gh);
        if (menu) {
            Tex* m = &sTex[TEX_MENU];
#ifdef __ANDROID__
            /* Uploaded only when it changed: a phone's screen is a lot to
             * copy every frame. */
            static unsigned uploaded = ~0u;
            int same = menu_generation() == uploaded && m->im.w == gw && m->im.h == gh;
            uploaded = menu_generation();
#else
            int same = 0;
#endif
            if (m->im.w != gw || m->im.h != gh) {
                drop_image(&m->im);
                m->im = make_image(gw, gh, 1, VK_FORMAT_B8G8R8A8_UNORM,
                                   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SAMPLE_COUNT_1_BIT);
                m->sampler = sLinearClamp;
                set_texture(TEX_MENU);
            }
            if (!same) upload(S->up, m, gw, gh, 1, (const uint8_t*)menu);
        }
        /* The copied rectangle of the EFB (on the canvas), as large as
         * fits in the window at the shape it has on a TV, with black bars
         * left over. */
        gx_canvas_rect(sMX, sMY, &x, &y, &cw, &ch);
        aspect = ch > 0 ? cw * sPixelAspect / ch : 1;
        dw = gw;
        dh = (int)(gw / aspect + 0.5);
        if (dh > gh) {
            dh = gh;
            dw = (int)(gh * aspect + 0.5);
        }
        efb_settle();
        transition(S->cb, &sEfb, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        transition(S->cb, &sWin, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        ca.imageView = sWin.view;
        ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        ca.clearValue.color.float32[3] = 1;
        ri.renderArea.extent.width = (uint32_t)gw;
        ri.renderArea.extent.height = (uint32_t)gh;
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &ca;
        vkCmdBeginRendering(S->cb, &ri);
        use_pipeline(pipeline(PK_BLIT, 0));
        set_viewport((float)((gw - dw) / 2), (float)((gh - dh) / 2), (float)dw, (float)dh, 0, 1);
        set_scissor(0, 0, gw, gh);
        set_cull(VK_CULL_MODE_NONE);
        set_depth(0, 0, VK_COMPARE_OP_ALWAYS);
        memset(&q, 0, sizeof(q));
        q.src[0] = (float)x / (EFB_W + 2 * sMX);
        q.src[1] = (float)y / (EFB_H + 2 * sMY);
        q.src[2] = (float)(x + cw) / (EFB_W + 2 * sMX);
        q.src[3] = (float)(y + ch) / (EFB_H + 2 * sMY);
        q.tex = TEX_EFB;
        quad(&q);
        if (menu) {
            use_pipeline(pipeline(PK_BLIT, 1));
            set_viewport(0, 0, (float)gw, (float)gh, 0, 1);
            q.src[0] = q.src[1] = 0;
            q.src[2] = q.src[3] = 1;
            q.tex = TEX_MENU;
            quad(&q);
        }
        vkCmdEndRendering(S->cb);
        transition(S->cb, &sWin, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        if (sBottomW > 0 && sBottomH > 0) draw_bottom();
        if (dump && sBottom.img && sBottom.w == sBottomW && sBottom.h == sBottomH) {
            VkBufferImageCopy r;
            VkDeviceSize bytes = (VkDeviceSize)sBottom.w * sBottom.h * 4;
            if (sReadback2.size < bytes) {
                drop_buffer(&sReadback2);
                sReadback2 = make_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                             VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
            }
            memset(&r, 0, sizeof(r));
            r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            r.imageSubresource.layerCount = 1;
            r.imageExtent.width = (uint32_t)sBottom.w;
            r.imageExtent.height = (uint32_t)sBottom.h;
            r.imageExtent.depth = 1;
            vkCmdCopyImageToBuffer(S->cb, sBottom.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sReadback2.buf, 1, &r);
            sBottomDumped = 1;
        }
        if (dump) {
            VkBufferImageCopy r;
            VkDeviceSize bytes = (VkDeviceSize)gw * gh * 4;
            if (sReadback.size < bytes) {
                drop_buffer(&sReadback);
                sReadback = make_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                            VK_MEMORY_PROPERTY_HOST_CACHED_BIT); /* read by the CPU */
            }
            memset(&r, 0, sizeof(r));
            r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            r.imageSubresource.layerCount = 1;
            r.imageExtent.width = (uint32_t)gw;
            r.imageExtent.height = (uint32_t)gh;
            r.imageExtent.depth = 1;
            vkCmdCopyImageToBuffer(S->cb, sWin.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sReadback.buf, 1, &r);
        }
        if (sChain && sChainW == w && sChainH == h) {
            /* Above 60 fps a frame is never waited for (gx_frames.c):
             * left out if no image is free. */
            VkResult r;
            present_done();
            r = vkAcquireNextImageKHR(sDev, sChain, may_drop ? 0 : UINT64_MAX, S->acquired, VK_NULL_HANDLE, &image);
            if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) {
                VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
                acquired = 1;
                bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                CHECK(vkBeginCommandBuffer(S->out, &bi));
                if (r == VK_SUBOPTIMAL_KHR && !SUBOPTIMAL_IS_EXPECTED) sChainStale = 1;
                copy_to_chain(swap && sBottom.w == w && sBottom.h == h ? &sBottom : &sWin, sChainImages[image], w, h);
                second_out(swap ? &sWin : &sBottom);
            } else if (r == VK_NOT_READY || r == VK_TIMEOUT) {
                dropped = 1;
            } else {
                sChainStale = 1;
            }
        }
    }
    sMapCount = 0;
    end_slot(acquired, image, dump, gw, gh);
    sPresented++;
    return dropped;
}

#ifdef __ANDROID__
/* The app went to the background and its window is going: everything on
 * it is let go of, until vk_window_back. The pipelines made so far are
 * kept on disk now, as Android may end the app while it's away. */
void vk_window_lost(void) {
    uint32_t i;
    if (!sDev) return;
    wait_idle();
    if (sCacheDirty) save_cache();
    if (sChain) vkDestroySwapchainKHR(sDev, sChain, NULL);
    for (i = 0; i < sChainCount; i++) vkDestroySemaphore(sDev, sRendered[i], NULL);
    sChain = VK_NULL_HANDLE;
    sChainCount = 0;
    sChainW = sChainH = 0;
    drop_surface2();
    sSecondGen = ~0u; /* taken again when back */
    if (sSurface) vkDestroySurfaceKHR(sInstance, sSurface, NULL);
    sSurface = VK_NULL_HANDLE;
}

/* And back, with a new window. */
void vk_window_back(void) {
    if (!sDev) return;
    if (!make_surface()) {
        fprintf(stderr, "vulkan: no surface for the new window\n");
        exit(1);
    }
    sChainStale = 1;
}
#endif

const GXDevice g_vk_device = {
    "Vulkan",        vk_init,          vk_texture, vk_texture_update, vk_texture_delete, vk_draw,
    vk_frame_data,   vk_draw_recorded, vk_clear,   vk_copy_target,    vk_copy_texture,   vk_copy,
    vk_present,      vk_apply_settings, vk_scale,  vk_msaa,   vk_arena_alloc,    vk_arena_write,    vk_arena_free,
    vk_texture_pixels, vk_copy_read,
};

#endif
