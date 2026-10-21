// Vulkan layer that draws the Vox TTS menu onto Nightcap's frames.
//
// The same library is LD_PRELOADed (SDL hooks) and named in an implicit
// layer manifest, so the loader dlopen()s the already loaded copy and both
// halves share state. Before each present, the menu is drawn with Dear ImGui
// into the swapchain image, waiting on the game's semaphores and signalling
// our own, which the present then waits on.
//
// Debugging: VOX_TTS_DUMP (or legacy NIGHTCAP_AC_DUMP)=/path.ppm saves one composited
// frame (the 30th drawn, or VOX_TTS_DUMP_FRAME / NIGHTCAP_AC_DUMP_FRAME), and
// VOX_TTS_MENU_OPEN (or NIGHTCAP_AC_MENU_OPEN)=1 starts with the menu open.

#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "app.h"
#include "imgui.h"
#include "imgui_impl_vulkan.h"
#include "overlay.h"
#include "ui.h"

#define EXPORT extern "C" __attribute__((visibility("default")))

namespace nac::layer {
namespace {

void* Key(const void* dispatchable) { return *reinterpret_cast<void* const*>(dispatchable); }

struct InstanceData {
  VkInstance instance = VK_NULL_HANDLE;
  PFN_vkGetInstanceProcAddr gipa = nullptr;
  PFN_vkDestroyInstance DestroyInstance = nullptr;
  PFN_vkGetPhysicalDeviceQueueFamilyProperties GetQueueFamilyProperties = nullptr;
  PFN_vkGetPhysicalDeviceMemoryProperties GetMemoryProperties = nullptr;
  uint32_t api_version = VK_API_VERSION_1_0;
};

struct SwapchainData;

struct DeviceData {
  VkDevice device = VK_NULL_HANDLE;
  VkPhysicalDevice physical = VK_NULL_HANDLE;
  InstanceData* instance = nullptr;
  PFN_vkGetDeviceProcAddr gdpa = nullptr;
  PFN_vkSetDeviceLoaderData set_loader_data = nullptr;
  std::vector<VkQueueFamilyProperties> families;
  std::map<VkQueue, uint32_t> queue_family;

#define DEVICE_FN(name) PFN_vk##name name = nullptr;
#include "layer_device_fns.inc"
#undef DEVICE_FN
};

struct Frame {
  VkImage image = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  VkFramebuffer framebuffer = VK_NULL_HANDLE;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  VkSemaphore done = VK_NULL_HANDLE;
  bool submitted = false;
};

struct SwapchainData {
  DeviceData* device = nullptr;
  VkSwapchainKHR swapchain = VK_NULL_HANDLE;
  VkFormat format = VK_FORMAT_UNDEFINED;
  VkExtent2D extent{};
  uint32_t min_images = 2;
  bool can_copy = false;
  VkRenderPass render_pass = VK_NULL_HANDLE;
  VkCommandPool pool = VK_NULL_HANDLE;
  uint32_t pool_family = 0;
  std::vector<Frame> frames;
};

// Recursive: the loader can call back into us (proc-address lookups) while
// we hold it during a present.
std::recursive_mutex g_mutex;
std::map<void*, std::unique_ptr<InstanceData>> g_instances;  // by dispatch key
std::map<void*, std::unique_ptr<DeviceData>> g_devices;      // by dispatch key
std::map<VkSwapchainKHR, std::unique_ptr<SwapchainData>> g_swapchains;

// The ImGui renderer lives on one device at a time.
struct Renderer {
  DeviceData* device = nullptr;
  ImGuiContext* context = nullptr;
  VkFormat format = VK_FORMAT_UNDEFINED;
  VkRenderPass render_pass = VK_NULL_HANDLE;  // the pass the pipeline was built for
  VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
  bool ready = false;
  int dumped = 0;
} g_renderer;

InstanceData* FindInstance(const void* dispatchable) {
  auto it = g_instances.find(Key(dispatchable));
  return it == g_instances.end() ? nullptr : it->second.get();
}

DeviceData* FindDevice(const void* dispatchable) {
  auto it = g_devices.find(Key(dispatchable));
  return it == g_devices.end() ? nullptr : it->second.get();
}

bool IsSrgb(VkFormat f) {
  switch (f) {
    case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
      return true;
    default:
      return false;
  }
}

// --- ImGui backend plumbing ------------------------------------------------------

DeviceData* g_loading_device = nullptr;

// Command buffers made inside a layer need the loader's dispatch pointer, or
// layers below us can't look them up.
VKAPI_ATTR VkResult VKAPI_CALL AllocateCommandBuffersForImGui(
    VkDevice device, const VkCommandBufferAllocateInfo* info, VkCommandBuffer* out) {
  DeviceData* d = g_renderer.device;
  const VkResult r = d->AllocateCommandBuffers(device, info, out);
  if (r == VK_SUCCESS && d->set_loader_data != nullptr) {
    for (uint32_t i = 0; i < info->commandBufferCount; ++i) d->set_loader_data(device, out[i]);
  }
  return r;
}

PFN_vkVoidFunction LoadForImGui(const char* name, void*) {
  DeviceData* d = g_loading_device;
  if (std::strcmp(name, "vkAllocateCommandBuffers") == 0) {
    return reinterpret_cast<PFN_vkVoidFunction>(&AllocateCommandBuffersForImGui);
  }
  if (PFN_vkVoidFunction fn = d->gdpa(d->device, name)) return fn;
  return d->instance->gipa(d->instance->instance, name);
}

void ShutdownRenderer() {
  if (!g_renderer.ready) return;
  DeviceData* d = g_renderer.device;
  d->DeviceWaitIdle(d->device);
  ImGui::SetCurrentContext(g_renderer.context);
  ImGui_ImplVulkan_Shutdown();
  d->DestroyDescriptorPool(d->device, g_renderer.descriptor_pool, nullptr);
  g_renderer.ready = false;
  g_renderer.device = nullptr;
  g_renderer.render_pass = VK_NULL_HANDLE;
}

bool InitRenderer(SwapchainData* sc, VkQueue queue, uint32_t family) {
  if (g_renderer.ready && (g_renderer.device != sc->device || g_renderer.format != sc->format)) {
    ShutdownRenderer();
  }
  if (g_renderer.ready) return true;
  DeviceData* d = sc->device;
  if (g_renderer.context == nullptr) {
    IMGUI_CHECKVERSION();
    g_renderer.context = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().LogFilename = nullptr;
  }
  ImGui::SetCurrentContext(g_renderer.context);

  VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16}};
  VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
  pool_info.maxSets = 16;
  pool_info.poolSizeCount = 1;
  pool_info.pPoolSizes = sizes;
  if (d->CreateDescriptorPool(d->device, &pool_info, nullptr, &g_renderer.descriptor_pool) != VK_SUCCESS) {
    return false;
  }
  g_renderer.device = d;
  g_loading_device = d;
  if (!ImGui_ImplVulkan_LoadFunctions(d->instance->api_version, LoadForImGui)) {
    Log("overlay: could not load Vulkan functions");
    return false;
  }
  ImGui_ImplVulkan_InitInfo init{};
  init.ApiVersion = d->instance->api_version;
  init.Instance = d->instance->instance;
  init.PhysicalDevice = d->physical;
  init.Device = d->device;
  init.QueueFamily = family;
  init.Queue = queue;
  init.DescriptorPool = g_renderer.descriptor_pool;
  init.RenderPass = sc->render_pass;
  init.MinImageCount = std::max<uint32_t>(2, sc->min_images);
  init.ImageCount = std::max<uint32_t>(init.MinImageCount, static_cast<uint32_t>(sc->frames.size()));
  init.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
  if (!ImGui_ImplVulkan_Init(&init)) {
    Log("overlay: ImGui Vulkan init failed");
    return false;
  }
  g_renderer.format = sc->format;
  g_renderer.render_pass = sc->render_pass;
  g_renderer.ready = true;
  ui::ResetFonts();  // built (and uploaded) on the first frame
  Log("overlay ready (%ux%u, format %d)", sc->extent.width, sc->extent.height, sc->format);
  return true;
}

// --- swapchain resources ----------------------------------------------------------

void DestroyFrames(SwapchainData* sc) {
  DeviceData* d = sc->device;
  for (Frame& f : sc->frames) {
    if (f.fence) {
      if (f.submitted) d->WaitForFences(d->device, 1, &f.fence, VK_TRUE, UINT64_MAX);
      d->DestroyFence(d->device, f.fence, nullptr);
    }
    if (f.done) d->DestroySemaphore(d->device, f.done, nullptr);
    if (f.framebuffer) d->DestroyFramebuffer(d->device, f.framebuffer, nullptr);
    if (f.view) d->DestroyImageView(d->device, f.view, nullptr);
  }
  sc->frames.clear();
  if (sc->pool) d->DestroyCommandPool(d->device, sc->pool, nullptr);
  sc->pool = VK_NULL_HANDLE;
  if (sc->render_pass && sc->render_pass != g_renderer.render_pass) {
    d->DestroyRenderPass(d->device, sc->render_pass, nullptr);
  }
  sc->render_pass = VK_NULL_HANDLE;
}

bool CreateFrames(SwapchainData* sc, uint32_t family) {
  DeviceData* d = sc->device;
  uint32_t count = 0;
  d->GetSwapchainImagesKHR(d->device, sc->swapchain, &count, nullptr);
  std::vector<VkImage> images(count);
  d->GetSwapchainImagesKHR(d->device, sc->swapchain, &count, images.data());

  VkAttachmentDescription color{};
  color.format = sc->format;
  color.samples = VK_SAMPLE_COUNT_1_BIT;
  color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  color.initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription subpass{};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = 1;
  subpass.pColorAttachments = &ref;
  VkSubpassDependency dep{};
  dep.srcSubpass = VK_SUBPASS_EXTERNAL;
  dep.dstSubpass = 0;
  dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  rp.attachmentCount = 1;
  rp.pAttachments = &color;
  rp.subpassCount = 1;
  rp.pSubpasses = &subpass;
  rp.dependencyCount = 1;
  rp.pDependencies = &dep;
  if (d->CreateRenderPass(d->device, &rp, nullptr, &sc->render_pass) != VK_SUCCESS) return false;

  VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pool.queueFamilyIndex = family;
  if (d->CreateCommandPool(d->device, &pool, nullptr, &sc->pool) != VK_SUCCESS) return false;
  sc->pool_family = family;

  sc->frames.resize(count);
  for (uint32_t i = 0; i < count; ++i) {
    Frame& f = sc->frames[i];
    f.image = images[i];
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = f.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = sc->format;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (d->CreateImageView(d->device, &view, nullptr, &f.view) != VK_SUCCESS) return false;
    VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fb.renderPass = sc->render_pass;
    fb.attachmentCount = 1;
    fb.pAttachments = &f.view;
    fb.width = sc->extent.width;
    fb.height = sc->extent.height;
    fb.layers = 1;
    if (d->CreateFramebuffer(d->device, &fb, nullptr, &f.framebuffer) != VK_SUCCESS) return false;
    VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc.commandPool = sc->pool;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    if (d->AllocateCommandBuffers(d->device, &alloc, &f.cmd) != VK_SUCCESS) return false;
    if (d->set_loader_data) d->set_loader_data(d->device, f.cmd);
    VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (d->CreateFence(d->device, &fence, nullptr, &f.fence) != VK_SUCCESS) return false;
    VkSemaphoreCreateInfo sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    if (d->CreateSemaphore(d->device, &sem, nullptr, &f.done) != VK_SUCCESS) return false;
  }
  return true;
}

// --- frame dump (debugging) -------------------------------------------------------

void DumpFrame(DeviceData* d, SwapchainData* sc, Frame& f, VkQueue queue, const char* path) {
  const VkDeviceSize size = static_cast<VkDeviceSize>(sc->extent.width) * sc->extent.height * 4;
  VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bi.size = size;
  bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  VkBuffer buffer;
  if (d->CreateBuffer(d->device, &bi, nullptr, &buffer) != VK_SUCCESS) return;
  VkMemoryRequirements req;
  d->GetBufferMemoryRequirements(d->device, buffer, &req);
  VkPhysicalDeviceMemoryProperties props;
  d->instance->GetMemoryProperties(d->physical, &props);
  uint32_t type = UINT32_MAX;
  for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
    const auto want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    if ((req.memoryTypeBits & (1u << i)) && (props.memoryTypes[i].propertyFlags & want) == want) {
      type = i;
      break;
    }
  }
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.allocationSize = req.size;
  ai.memoryTypeIndex = type;
  VkDeviceMemory memory;
  if (type == UINT32_MAX || d->AllocateMemory(d->device, &ai, nullptr, &memory) != VK_SUCCESS) {
    d->DestroyBuffer(d->device, buffer, nullptr);
    return;
  }
  d->BindBufferMemory(d->device, buffer, memory, 0);

  VkCommandBuffer cmd = f.cmd;
  d->WaitForFences(d->device, 1, &f.fence, VK_TRUE, UINT64_MAX);
  d->ResetFences(d->device, 1, &f.fence);
  d->ResetCommandBuffer(cmd, 0);
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  d->BeginCommandBuffer(cmd, &begin);
  VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  barrier.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = f.image;
  barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  d->CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
  VkBufferImageCopy copy{};
  copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  copy.imageExtent = {sc->extent.width, sc->extent.height, 1};
  d->CmdCopyImageToBuffer(cmd, f.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  barrier.dstAccessMask = 0;
  barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  d->CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                        0, 0, nullptr, 0, nullptr, 1, &barrier);
  d->EndCommandBuffer(cmd);
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &cmd;
  d->QueueSubmit(queue, 1, &submit, f.fence);
  d->WaitForFences(d->device, 1, &f.fence, VK_TRUE, UINT64_MAX);

  void* pixels = nullptr;
  d->MapMemory(d->device, memory, 0, size, 0, &pixels);
  if (FILE* out = std::fopen(path, "wb")) {
    std::fprintf(out, "P6\n%u %u\n255\n", sc->extent.width, sc->extent.height);
    const bool bgr = sc->format == VK_FORMAT_B8G8R8A8_UNORM || sc->format == VK_FORMAT_B8G8R8A8_SRGB;
    const auto* p = static_cast<const unsigned char*>(pixels);
    for (VkDeviceSize i = 0; i < size; i += 4) {
      const unsigned char rgb[3] = {p[i + (bgr ? 2 : 0)], p[i + 1], p[i + (bgr ? 0 : 2)]};
      std::fwrite(rgb, 1, 3, out);
    }
    std::fclose(out);
    Log("overlay: saved frame to %s", path);
  }
  d->UnmapMemory(d->device, memory);
  d->FreeMemory(d->device, memory, nullptr);
  d->DestroyBuffer(d->device, buffer, nullptr);
}

// --- drawing ------------------------------------------------------------------------

// Draws the menu into the image being presented. Returns the semaphore the
// present must wait on, or VK_NULL_HANDLE when nothing was drawn.
VkSemaphore DrawOverlay(SwapchainData* sc, VkQueue queue, uint32_t image_index,
                        uint32_t wait_count, const VkSemaphore* waits) {
  DeviceData* d = sc->device;
  auto family_it = d->queue_family.find(queue);
  if (family_it == d->queue_family.end()) return VK_NULL_HANDLE;
  const uint32_t family = family_it->second;
  if (family >= d->families.size() || !(d->families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
    return VK_NULL_HANDLE;
  }
  if (image_index >= sc->frames.size() && !sc->frames.empty()) return VK_NULL_HANDLE;
  if (sc->frames.empty() || sc->pool_family != family) {
    DestroyFrames(sc);
    if (!CreateFrames(sc, family)) {
      DestroyFrames(sc);
      return VK_NULL_HANDLE;
    }
  }
  if (!InitRenderer(sc, queue, family)) return VK_NULL_HANDLE;

  ImGui::SetCurrentContext(g_renderer.context);
  const overlay::Metrics m = overlay::GetMetrics();
  const float px = m.window_w > 0 ? static_cast<float>(sc->extent.width) / m.window_w : 1.0f;
  if (ui::PrepareFonts(px)) {
    ImGui_ImplVulkan_DestroyFontsTexture();
    ImGui_ImplVulkan_CreateFontsTexture();
  }
  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2(static_cast<float>(sc->extent.width), static_cast<float>(sc->extent.height));
  ui::FeedInput(overlay::TakeInput(), px);
  ImGui_ImplVulkan_NewFrame();
  ImGui::NewFrame();
  const bool visible = ui::Draw(px, IsSrgb(sc->format));
  ImGui::Render();
  ImDrawData* draw = ImGui::GetDrawData();
  if (!visible || draw == nullptr || draw->TotalVtxCount == 0) return VK_NULL_HANDLE;

  Frame& f = sc->frames[image_index];
  if (f.submitted) {
    d->WaitForFences(d->device, 1, &f.fence, VK_TRUE, UINT64_MAX);
  }
  d->ResetFences(d->device, 1, &f.fence);
  d->ResetCommandBuffer(f.cmd, 0);
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  d->BeginCommandBuffer(f.cmd, &begin);
  VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  rp.renderPass = sc->render_pass;
  rp.framebuffer = f.framebuffer;
  rp.renderArea.extent = sc->extent;
  d->CmdBeginRenderPass(f.cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
  ImGui_ImplVulkan_RenderDrawData(draw, f.cmd);
  d->CmdEndRenderPass(f.cmd);
  d->EndCommandBuffer(f.cmd);

  std::vector<VkPipelineStageFlags> stages(wait_count, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.waitSemaphoreCount = wait_count;
  submit.pWaitSemaphores = waits;
  submit.pWaitDstStageMask = stages.data();
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &f.cmd;
  submit.signalSemaphoreCount = 1;
  submit.pSignalSemaphores = &f.done;
  if (d->QueueSubmit(queue, 1, &submit, f.fence) != VK_SUCCESS) return VK_NULL_HANDLE;
  f.submitted = true;

  const char* dump = std::getenv("VOX_TTS_DUMP");
  if (!dump) dump = std::getenv("NIGHTCAP_AC_DUMP");
  if (dump) {
    // Let a few frames settle (fonts, layout) before saving one.
    // Copying waits for our draw to finish; f.done stays signalled for
    // the present, so the handoff is unchanged.
    const char* at = std::getenv("VOX_TTS_DUMP_FRAME");
    if (!at) at = std::getenv("NIGHTCAP_AC_DUMP_FRAME");
    if (sc->can_copy && ++g_renderer.dumped == (at ? std::atoi(at) : 30)) DumpFrame(d, sc, f, queue, dump);
  }
  return f.done;
}

// --- hooked entry points ------------------------------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL CreateInstance(const VkInstanceCreateInfo* info,
                                              const VkAllocationCallbacks* alloc, VkInstance* out) {
  auto* link = const_cast<VkLayerInstanceCreateInfo*>(static_cast<const VkLayerInstanceCreateInfo*>(info->pNext));
  while (link && !(link->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && link->function == VK_LAYER_LINK_INFO)) {
    link = const_cast<VkLayerInstanceCreateInfo*>(static_cast<const VkLayerInstanceCreateInfo*>(link->pNext));
  }
  if (link == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
  PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
  link->u.pLayerInfo = link->u.pLayerInfo->pNext;
  auto create = reinterpret_cast<PFN_vkCreateInstance>(gipa(VK_NULL_HANDLE, "vkCreateInstance"));
  const VkResult r = create(info, alloc, out);
  if (r != VK_SUCCESS) return r;
  auto data = std::make_unique<InstanceData>();
  data->instance = *out;
  data->gipa = gipa;
  data->DestroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(gipa(*out, "vkDestroyInstance"));
  data->GetQueueFamilyProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
      gipa(*out, "vkGetPhysicalDeviceQueueFamilyProperties"));
  data->GetMemoryProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
      gipa(*out, "vkGetPhysicalDeviceMemoryProperties"));
  if (info->pApplicationInfo && info->pApplicationInfo->apiVersion) {
    data->api_version = info->pApplicationInfo->apiVersion;
  }
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  g_instances[Key(*out)] = std::move(data);
  return r;
}

VKAPI_ATTR void VKAPI_CALL DestroyInstance(VkInstance instance, const VkAllocationCallbacks* alloc) {
  PFN_vkDestroyInstance destroy;
  {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    InstanceData* data = FindInstance(instance);
    if (data == nullptr) return;
    destroy = data->DestroyInstance;
    g_instances.erase(Key(instance));
  }
  destroy(instance, alloc);
}

VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo* info,
                                            const VkAllocationCallbacks* alloc, VkDevice* out) {
  auto* chain = static_cast<const VkLayerDeviceCreateInfo*>(info->pNext);
  VkLayerDeviceCreateInfo* link = nullptr;
  PFN_vkSetDeviceLoaderData set_loader_data = nullptr;
  for (auto* p = chain; p; p = static_cast<const VkLayerDeviceCreateInfo*>(p->pNext)) {
    if (p->sType != VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO) continue;
    if (p->function == VK_LAYER_LINK_INFO && link == nullptr) link = const_cast<VkLayerDeviceCreateInfo*>(p);
    if (p->function == VK_LOADER_DATA_CALLBACK) set_loader_data = p->u.pfnSetDeviceLoaderData;
  }
  if (link == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
  PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
  PFN_vkGetDeviceProcAddr gdpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
  link->u.pLayerInfo = link->u.pLayerInfo->pNext;

  InstanceData* inst;
  {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    inst = FindInstance(physical);
  }
  if (inst == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
  auto create = reinterpret_cast<PFN_vkCreateDevice>(gipa(inst->instance, "vkCreateDevice"));
  const VkResult r = create(physical, info, alloc, out);
  if (r != VK_SUCCESS) return r;

  auto data = std::make_unique<DeviceData>();
  data->device = *out;
  data->physical = physical;
  data->instance = inst;
  data->gdpa = gdpa;
  data->set_loader_data = set_loader_data;
#define DEVICE_FN(name) data->name = reinterpret_cast<PFN_vk##name>(gdpa(*out, "vk" #name));
#include "layer_device_fns.inc"
#undef DEVICE_FN
  uint32_t count = 0;
  inst->GetQueueFamilyProperties(physical, &count, nullptr);
  data->families.resize(count);
  inst->GetQueueFamilyProperties(physical, &count, data->families.data());
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  g_devices[Key(*out)] = std::move(data);
  return r;
}

VKAPI_ATTR void VKAPI_CALL DestroyDevice(VkDevice device, const VkAllocationCallbacks* alloc) {
  PFN_vkDestroyDevice destroy;
  {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    DeviceData* d = FindDevice(device);
    if (d == nullptr) return;
    for (auto it = g_swapchains.begin(); it != g_swapchains.end();) {
      if (it->second->device == d) {
        DestroyFrames(it->second.get());
        it = g_swapchains.erase(it);
      } else {
        ++it;
      }
    }
    if (g_renderer.device == d) ShutdownRenderer();
    destroy = d->DestroyDevice;
    g_devices.erase(Key(device));
  }
  destroy(device, alloc);
}

VKAPI_ATTR void VKAPI_CALL GetDeviceQueue(VkDevice device, uint32_t family, uint32_t index, VkQueue* out) {
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  DeviceData* d = FindDevice(device);
  d->GetDeviceQueue(device, family, index, out);
  if (*out) d->queue_family[*out] = family;
}

VKAPI_ATTR void VKAPI_CALL GetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2* info, VkQueue* out) {
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  DeviceData* d = FindDevice(device);
  d->GetDeviceQueue2(device, info, out);
  if (*out) d->queue_family[*out] = info->queueFamilyIndex;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR* info,
                                                  const VkAllocationCallbacks* alloc, VkSwapchainKHR* out) {
  DeviceData* d;
  {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    d = FindDevice(device);
  }
  VkSwapchainCreateInfoKHR copy = *info;
  // We render into the images, and copy one out when debugging.
  copy.imageUsage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  const bool dump = (std::getenv("VOX_TTS_DUMP") != nullptr || std::getenv("NIGHTCAP_AC_DUMP") != nullptr);
  if (dump) copy.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  VkResult r = d->CreateSwapchainKHR(device, &copy, alloc, out);
  if (r != VK_SUCCESS) {
    // Fall back to exactly what the game asked for (and draw nothing).
    return d->CreateSwapchainKHR(device, info, alloc, out);
  }
  auto sc = std::make_unique<SwapchainData>();
  sc->device = d;
  sc->swapchain = *out;
  sc->format = info->imageFormat;
  sc->extent = info->imageExtent;
  sc->min_images = info->minImageCount;
  sc->can_copy = dump;
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  g_swapchains[*out] = std::move(sc);
  return r;
}

VKAPI_ATTR void VKAPI_CALL DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                                               const VkAllocationCallbacks* alloc) {
  PFN_vkDestroySwapchainKHR destroy;
  {
    std::lock_guard<std::recursive_mutex> lock(g_mutex);
    DeviceData* d = FindDevice(device);
    auto it = g_swapchains.find(swapchain);
    if (it != g_swapchains.end()) {
      DestroyFrames(it->second.get());
      g_swapchains.erase(it);
    }
    destroy = d->DestroySwapchainKHR;
  }
  destroy(device, swapchain, alloc);
}

VKAPI_ATTR VkResult VKAPI_CALL QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* info) {
  std::unique_lock<std::recursive_mutex> lock(g_mutex);
  DeviceData* d = FindDevice(queue);
  PFN_vkQueuePresentKHR present = d->QueuePresentKHR;
  VkSemaphore done = VK_NULL_HANDLE;
  if (info->swapchainCount > 0) {
    auto it = g_swapchains.find(info->pSwapchains[0]);
    if (it != g_swapchains.end()) {
      done = DrawOverlay(it->second.get(), queue, info->pImageIndices[0], info->waitSemaphoreCount,
                         info->pWaitSemaphores);
    }
  }
  lock.unlock();
  if (done == VK_NULL_HANDLE) return present(queue, info);
  VkPresentInfoKHR copy = *info;
  copy.waitSemaphoreCount = 1;
  copy.pWaitSemaphores = &done;
  return present(queue, &copy);
}

PFN_vkVoidFunction DeviceHook(const char* name);

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetDeviceProcAddr(VkDevice device, const char* name) {
  if (PFN_vkVoidFunction hook = DeviceHook(name)) return hook;
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  DeviceData* d = FindDevice(device);
  return d ? d->gdpa(device, name) : nullptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetInstanceProcAddr(VkInstance instance, const char* name) {
#define HOOK(fn) \
  if (std::strcmp(name, "vk" #fn) == 0) return reinterpret_cast<PFN_vkVoidFunction>(&fn);
  HOOK(GetInstanceProcAddr)
  HOOK(CreateInstance)
  HOOK(DestroyInstance)
  HOOK(CreateDevice)
  HOOK(GetDeviceProcAddr)
#undef HOOK
  if (PFN_vkVoidFunction hook = DeviceHook(name)) return hook;
  std::lock_guard<std::recursive_mutex> lock(g_mutex);
  InstanceData* inst = instance ? FindInstance(instance) : nullptr;
  return inst ? inst->gipa(instance, name) : nullptr;
}

PFN_vkVoidFunction DeviceHook(const char* name) {
#define HOOK(fn) \
  if (std::strcmp(name, "vk" #fn) == 0) return reinterpret_cast<PFN_vkVoidFunction>(&fn);
  HOOK(GetDeviceProcAddr)
  HOOK(DestroyDevice)
  HOOK(GetDeviceQueue)
  HOOK(GetDeviceQueue2)
  HOOK(CreateSwapchainKHR)
  HOOK(DestroySwapchainKHR)
  HOOK(QueuePresentKHR)
#undef HOOK
  return nullptr;
}

}  // namespace
}  // namespace nac::layer

EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* iface) {
  if (iface == nullptr || iface->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  if (iface->loaderLayerInterfaceVersion > 2) iface->loaderLayerInterfaceVersion = 2;
  iface->pfnGetInstanceProcAddr = &nac::layer::GetInstanceProcAddr;
  iface->pfnGetDeviceProcAddr = &nac::layer::GetDeviceProcAddr;
  iface->pfnGetPhysicalDeviceProcAddr = nullptr;
  if (std::getenv("VOX_TTS_MENU_OPEN") || std::getenv("NIGHTCAP_AC_MENU_OPEN")) {
    nac::overlay::SetMenuOpenFlag(true);
  }
  return VK_SUCCESS;
}
