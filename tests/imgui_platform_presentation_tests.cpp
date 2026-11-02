// CPU-only pressure against the actual bundled backend callbacks. All Vulkan
// calls dispatch to injected functions; no instance, device, window or GPU exists.
#define IMGUI_IMPL_VULKAN_NO_PROTOTYPES
#include "../third_party/imgui/backends/imgui_impl_vulkan.cpp"
#include <array>
#include <cstdint>
#include <iostream>
#include <map>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace {
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
template<class T> T handle(std::uintptr_t n) {
    if constexpr (std::is_pointer_v<T>) return reinterpret_cast<T>(n);
    else return static_cast<T>(n);
}
struct Trace {
    std::vector<std::uint32_t> images{0,1,0,1,1,0,2,0,0,1};
    std::array<bool,3> acquired{};
    std::map<VkSemaphore,std::uint32_t> pending_present;
    std::map<VkSemaphore,std::uint32_t> submitted;
    std::size_t next=0;
    int acquisitions=0, waits=0, submissions=0, presents=0, records=0, violations=0;
    VkResult acquire_result=VK_SUCCESS;
    bool defer_fence_once=false,creating=false;
    std::uint32_t allocated_commands=0;
    std::uint64_t acquire_budget=0,wait_budget=0;
};
Trace* trace=nullptr;
VKAPI_ATTR VkResult VKAPI_CALL acquire(VkDevice,VkSwapchainKHR,std::uint64_t budget,VkSemaphore,VkFence,std::uint32_t* image) {
    ++trace->acquisitions;trace->acquire_budget=budget;
    if(trace->acquire_result!=VK_SUCCESS)return trace->acquire_result;
    *image=trace->images.at(trace->next++);
    for(auto i=trace->pending_present.begin();i!=trace->pending_present.end();)
        if(i->second==*image)i=trace->pending_present.erase(i);else ++i;
    if(trace->acquired.at(*image))++trace->violations;
    trace->acquired.at(*image)=true;return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL wait(VkDevice,std::uint32_t,const VkFence*,VkBool32,std::uint64_t budget) {
    ++trace->waits;trace->wait_budget=budget;
    if(trace->defer_fence_once){trace->defer_fence_once=false;return VK_TIMEOUT;}
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL reset_pool(VkDevice,VkCommandPool,VkCommandPoolResetFlags){return VK_SUCCESS;}
VKAPI_ATTR VkResult VKAPI_CALL begin(VkCommandBuffer,const VkCommandBufferBeginInfo*){return VK_SUCCESS;}
VKAPI_ATTR void VKAPI_CALL begin_pass(VkCommandBuffer,const VkRenderPassBeginInfo* p,VkSubpassContents){
    ++trace->records;
    const auto image=static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(p->framebuffer)-100);
    if(!trace->acquired.at(image))++trace->violations;
}
VKAPI_ATTR void VKAPI_CALL end_pass(VkCommandBuffer){}
VKAPI_ATTR VkResult VKAPI_CALL end(VkCommandBuffer){return VK_SUCCESS;}
VKAPI_ATTR VkResult VKAPI_CALL reset_fences(VkDevice,std::uint32_t,const VkFence*){return VK_SUCCESS;}
VKAPI_ATTR VkResult VKAPI_CALL submit(VkQueue,std::uint32_t,const VkSubmitInfo* p,VkFence){
    ++trace->submissions;
    if(trace->creating){++trace->violations;return VK_SUCCESS;}
    const auto image=static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(*p->pCommandBuffers)-200);
    if(!trace->acquired.at(image)||trace->pending_present.contains(*p->pSignalSemaphores))++trace->violations;
    trace->submitted[*p->pSignalSemaphores]=image;return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL present(VkQueue,const VkPresentInfoKHR* p){
    ++trace->presents;const auto image=*p->pImageIndices;const auto semaphore=*p->pWaitSemaphores;
    if(!trace->acquired.at(image)||!trace->submitted.contains(semaphore)||trace->submitted[semaphore]!=image)++trace->violations;
    trace->acquired.at(image)=false;trace->pending_present[semaphore]=image;return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL bind_pipeline(VkCommandBuffer,VkPipelineBindPoint,VkPipeline){}
VKAPI_ATTR void VKAPI_CALL set_viewport(VkCommandBuffer,std::uint32_t,std::uint32_t,const VkViewport*){}
VKAPI_ATTR void VKAPI_CALL set_scissor(VkCommandBuffer,std::uint32_t,std::uint32_t,const VkRect2D*){}
VKAPI_ATTR void VKAPI_CALL push(VkCommandBuffer,VkPipelineLayout,VkShaderStageFlags,std::uint32_t,std::uint32_t,const void*){}
VKAPI_ATTR void VKAPI_CALL bind_sets(VkCommandBuffer,VkPipelineBindPoint,VkPipelineLayout,std::uint32_t,std::uint32_t,const VkDescriptorSet*,std::uint32_t,const std::uint32_t*){}
VKAPI_ATTR VkResult VKAPI_CALL idle(VkDevice){return VK_SUCCESS;}
VKAPI_ATTR VkResult VKAPI_CALL capabilities(VkPhysicalDevice,VkSurfaceKHR,VkSurfaceCapabilitiesKHR* p){
    *p={};p->minImageCount=2;p->maxImageCount=3;p->currentExtent={16,16};p->supportedTransforms=VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;p->currentTransform=VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;p->supportedCompositeAlpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL create_swap(VkDevice,const VkSwapchainCreateInfoKHR*,const VkAllocationCallbacks*,VkSwapchainKHR* p){*p=handle<VkSwapchainKHR>(600);return VK_SUCCESS;}
VKAPI_ATTR VkResult VKAPI_CALL images(VkDevice,VkSwapchainKHR,std::uint32_t* count,VkImage* p){*count=3;if(p)for(std::uint32_t i=0;i<3;++i)p[i]=handle<VkImage>(700+i);return VK_SUCCESS;}
VKAPI_ATTR VkResult VKAPI_CALL create_pass(VkDevice,const VkRenderPassCreateInfo*,const VkAllocationCallbacks*,VkRenderPass* p){*p=handle<VkRenderPass>(800);return VK_SUCCESS;}
VKAPI_ATTR VkResult VKAPI_CALL create_view(VkDevice,const VkImageViewCreateInfo*,const VkAllocationCallbacks*,VkImageView* p){*p=handle<VkImageView>(900);return VK_SUCCESS;}
VKAPI_ATTR VkResult VKAPI_CALL create_framebuffer(VkDevice,const VkFramebufferCreateInfo*,const VkAllocationCallbacks*,VkFramebuffer* p){*p=handle<VkFramebuffer>(100);return VK_SUCCESS;}
VKAPI_ATTR VkResult VKAPI_CALL create_pool(VkDevice,const VkCommandPoolCreateInfo*,const VkAllocationCallbacks*,VkCommandPool* p){*p=handle<VkCommandPool>(1000);return VK_SUCCESS;}
VKAPI_ATTR VkResult VKAPI_CALL allocate(VkDevice,const VkCommandBufferAllocateInfo*,VkCommandBuffer* p){*p=handle<VkCommandBuffer>(200+trace->allocated_commands++);return VK_SUCCESS;}
VKAPI_ATTR VkResult VKAPI_CALL create_fence(VkDevice,const VkFenceCreateInfo*,const VkAllocationCallbacks*,VkFence* p){*p=handle<VkFence>(1100);return VK_SUCCESS;}
VKAPI_ATTR VkResult VKAPI_CALL create_semaphore(VkDevice,const VkSemaphoreCreateInfo*,const VkAllocationCallbacks*,VkSemaphore* p){*p=handle<VkSemaphore>(1200);return VK_SUCCESS;}
VKAPI_ATTR void VKAPI_CALL queue(VkDevice,std::uint32_t,std::uint32_t,VkQueue* p){*p=handle<VkQueue>(1300);}
VKAPI_ATTR void VKAPI_CALL barriers(VkCommandBuffer,VkPipelineStageFlags,VkPipelineStageFlags,VkDependencyFlags,std::uint32_t,const VkMemoryBarrier*,std::uint32_t,const VkBufferMemoryBarrier*,std::uint32_t count,const VkImageMemoryBarrier*){trace->violations+=static_cast<int>(count);}
VKAPI_ATTR void VKAPI_CALL free_commands(VkDevice,VkCommandPool,std::uint32_t,const VkCommandBuffer*){}
VKAPI_ATTR void VKAPI_CALL destroy_pool(VkDevice,VkCommandPool,const VkAllocationCallbacks*){}
VKAPI_ATTR void VKAPI_CALL destroy_fence(VkDevice,VkFence,const VkAllocationCallbacks*){}
struct Fixture {
    Trace log;
    ImGui_ImplVulkan_Data backend;
    ImGui_ImplVulkan_ViewportData data;
    ImGuiViewport viewport;
    ImDrawData draw;
    Fixture(){
        ImGui::CreateContext();trace=&log;g_FunctionsLoaded=true;
        ImGui::GetIO().BackendRendererUserData=&backend;
        backend.VulkanInitInfo.ImageCount=2;
        auto& f=backend.Functions;
        f.pfn_vkAcquireNextImageKHR=acquire;f.pfn_vkWaitForFences=wait;
        f.pfn_vkResetCommandPool=reset_pool;f.pfn_vkBeginCommandBuffer=begin;
        f.pfn_vkCmdBeginRenderPass=begin_pass;f.pfn_vkCmdEndRenderPass=end_pass;
        f.pfn_vkEndCommandBuffer=end;f.pfn_vkResetFences=reset_fences;
        f.pfn_vkQueueSubmit=submit;f.pfn_vkQueuePresentKHR=present;
        f.pfn_vkCmdBindPipeline=bind_pipeline;f.pfn_vkCmdSetViewport=set_viewport;
        f.pfn_vkCmdPushConstants=push;f.pfn_vkCmdBindDescriptorSets=bind_sets;f.pfn_vkCmdSetScissor=set_scissor;
        f.pfn_vkDeviceWaitIdle=idle;f.pfn_vkGetPhysicalDeviceSurfaceCapabilitiesKHR=capabilities;
        f.pfn_vkCreateSwapchainKHR=create_swap;f.pfn_vkGetSwapchainImagesKHR=images;
        f.pfn_vkCreateRenderPass=create_pass;f.pfn_vkCreateImageView=create_view;f.pfn_vkCreateFramebuffer=create_framebuffer;
        f.pfn_vkCreateCommandPool=create_pool;f.pfn_vkAllocateCommandBuffers=allocate;
        f.pfn_vkCreateFence=create_fence;f.pfn_vkCreateSemaphore=create_semaphore;f.pfn_vkGetDeviceQueue=queue;
        f.pfn_vkCmdPipelineBarrier=barriers;f.pfn_vkFreeCommandBuffers=free_commands;
        f.pfn_vkDestroyCommandPool=destroy_pool;f.pfn_vkDestroyFence=destroy_fence;
        data.WindowOwned=true;auto& w=data.Window;
        w.ImageCount=3;w.SemaphoreCount=4;w.Frames.resize(3);w.FrameSemaphores.resize(4);
        for(std::uint32_t i=0;i<3;++i){w.Frames[i]={};w.Frames[i].Framebuffer=handle<VkFramebuffer>(100+i);w.Frames[i].CommandBuffer=handle<VkCommandBuffer>(200+i);}
        for(std::uint32_t i=0;i<4;++i){w.FrameSemaphores[i]={};w.FrameSemaphores[i].ImageAcquiredSemaphore=handle<VkSemaphore>(300+i);w.FrameSemaphores[i].RenderCompleteSemaphore=handle<VkSemaphore>(400+i);}
        viewport.RendererUserData=&data;viewport.DrawData=&draw;draw.OwnerViewport=&viewport;
        draw.DisplaySize=ImVec2(0,0);draw.FramebufferScale=ImVec2(1,1);
    }
    ~Fixture(){viewport.RendererUserData=nullptr;viewport.DrawData=nullptr;ImGui::GetIO().BackendRendererUserData=nullptr;ImGui::DestroyContext();trace=nullptr;}
    void frame(){ImGui_ImplVulkan_RenderWindow(&viewport,nullptr);ImGui_ImplVulkan_SwapBuffers(&viewport,nullptr);}
};
void creation_ownership(){
    Fixture f;f.log.creating=true;ImGui_ImplVulkanH_Window window;
    window.Surface=handle<VkSurfaceKHR>(1);window.SurfaceFormat={VK_FORMAT_B8G8R8A8_UNORM,VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};window.PresentMode=VK_PRESENT_MODE_FIFO_KHR;
    ImGui_ImplVulkanH_CreateOrResizeWindow(VK_NULL_HANDLE,handle<VkPhysicalDevice>(2),handle<VkDevice>(3),&window,0,nullptr,16,16,2,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    require(window.ImageCount==3,"platform swapchain happy-path setup did not complete");
    require(f.log.violations==0&&f.log.submissions==0,"platform creation transitioned or submitted never-acquired swapchain images");
}
void irregular_images(){
    Fixture f;for(std::size_t i=0;i<f.log.images.size();++i)f.frame();
    require(f.log.violations==0,"platform presenter reused a pending presentation semaphore or an unacquired image");
    require(f.log.presents==10&&f.log.submissions==10,"happy-path platform frames were lost");
}
void acquire_timeout(){
    Fixture f;f.log.acquire_result=VK_TIMEOUT;f.frame();
    require(f.log.records==0&&f.log.submissions==0&&f.log.presents==0,"acquire timeout recorded/submitted/presented an unacquired platform image");
    require(f.log.acquire_budget<=8'000'000,"platform acquire exceeded finite event-loop budget");
    f.log.acquire_result=VK_SUCCESS;f.frame();require(f.log.presents==1&&f.log.violations==0,"platform acquire did not recover after timeout");
}
void deferred_image_fence(){
    Fixture f;f.log.defer_fence_once=true;f.frame();
    require(f.log.waits==1&&f.log.submissions==0&&f.log.presents==0,"platform fence timeout spun or submitted instead of yielding");
    require(f.log.wait_budget<=2'000'000,"platform fence exceeded finite event-loop budget");
    f.frame();require(f.log.acquisitions==1&&f.log.submissions==1&&f.log.presents==1&&f.log.violations==0,"deferred platform frame lost or reacquired its owned image");
}
void failed_acquire(){
    Fixture f;f.log.acquire_result=VK_ERROR_SURFACE_LOST_KHR;f.frame();
    require(f.log.records==0&&f.log.submissions==0&&f.log.presents==0,"failed platform acquisition reached recording/submission/presentation");
}
void upload_ring(){
    Fixture f;f.draw.DisplaySize=ImVec2(16,16);
    f.log.images={2,0,2,1,0,1};
    for(const auto image:f.log.images){f.frame();require(f.data.RenderBuffers.Count==3&&f.data.RenderBuffers.Index==image,"platform upload buffers do not follow the acquired image fence");}
    require(f.log.violations==0,"platform upload happy path violated image ownership");
}
}
int main(){
    int failed=0;
    for(auto [name,test]:std::vector<std::pair<const char*,void(*)()>>{{"creation ownership",creation_ownership},{"irregular image ownership",irregular_images},{"acquire timeout",acquire_timeout},{"deferred image fence",deferred_image_fence},{"failed acquisition",failed_acquire},{"per-image upload ring",upload_ring}}){
        try{test();std::cout<<"PASS "<<name<<'\n';}catch(const std::exception& e){++failed;std::cerr<<"FAIL "<<name<<": "<<e.what()<<'\n';}
    }
    return failed?1:0;
}
