/*
 *
 * Andrew Frost
 * vulkanbackend.cpp
 * 2020
 *
 */

#define VK_NO_PROTOTYPES
#include "vulkanbackend.hpp"
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE;

namespace app {

///////////////////////////////////////////////////////////////////////////
// VulkanBackend                                                         //
///////////////////////////////////////////////////////////////////////////

//-------------------------------------------------------------------------
// Setup Vulkan Renderer Backend
//
void VulkanBackend::setupVulkan(const ContextCreateInfo& info, GLFWwindow* window)
{
    initInstance(info);

    setupDebugMessenger(info.enableValidationLayers);

    createSurface(window);

    pickPhysicalDevice(info);

    createLogicalDeviceAndQueues(info);

    createSwapChain();

    createCommandPool();

    createCommandBuffer();

    createDepthBuffer();

    createRenderPass();

    createPipelineCache();

    createFrameBuffers();

    createSyncObjects();

}

//-------------------------------------------------------------------------
// Call on exit
//
void VulkanBackend::destroy()
{
    m_device.waitIdle();

    if (ImGui::GetCurrentContext() != nullptr) {
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();

        m_device.destroyDescriptorPool(m_imguiDescPool);
    }

    m_device.destroyRenderPass(m_renderPass);

    m_device.destroyImageView(m_depthView);
    m_device.destroyImage(m_depthImage);
    m_device.freeMemory(m_depthMemory);

    m_device.destroyPipelineCache(m_pipelineCache);

    for (uint32_t i = 0; i < m_swapchain.getImageCount(); i++) {

        m_device.destroyFramebuffer(m_framebuffers[i]);
        m_device.destroyFence(m_fences[i]);
        m_device.freeCommandBuffers(m_commandPool, m_commandBuffers[i]);
    }

    m_swapchain.destroy();

    m_device.destroyCommandPool(m_commandPool);

    m_device.destroy();

    if (m_debugMessenger)
        m_instance.destroyDebugUtilsMessengerEXT(m_debugMessenger);

    m_instance.destroySurfaceKHR(m_surface);
    m_instance.destroy();
}

//-------------------------------------------------------------------------
// Create Vulkan Instance
//
void VulkanBackend::initInstance(const ContextCreateInfo& info)
{
    vk::DynamicLoader         dl;
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr =
        dl.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
    VULKAN_HPP_DEFAULT_DISPATCHER.init(vkGetInstanceProcAddr);

    if (info.enableValidationLayers && !checkValidationLayerSupport(info)) {
        throw std::runtime_error("validation layers requested, but not available!");
    }

    vk::ApplicationInfo appInfo = {};
    appInfo.pApplicationName = info.appTitle;
    appInfo.pEngineName = info.appEngine;
    appInfo.apiVersion = VK_API_VERSION_1_0;

    vk::InstanceCreateInfo createInfo = {};
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledExtensionCount = info.numInstanceExtensions;
    createInfo.ppEnabledExtensionNames = info.instanceExtensions.data();

    if (info.enableValidationLayers) {
        createInfo.enabledLayerCount = info.numValidationLayers;
        createInfo.ppEnabledLayerNames = info.validationLayers.data();
    }

    try {
        m_instance = vk::createInstance(createInfo);
    }
    catch (vk::SystemError err) {
        throw std::runtime_error("failed to create instance!");
    }

    VULKAN_HPP_DEFAULT_DISPATCHER.init(m_instance);
}

//-------------------------------------------------------------------------
// Create Surface
//
void VulkanBackend::createSurface(GLFWwindow* window)
{
    int width, height;
    glfwGetWindowSize(window, &width, &height);
    m_size = vk::Extent2D(width, height);

    VkSurfaceKHR rawSurface;
    if (glfwCreateWindowSurface(m_instance, window, nullptr, &rawSurface) != VK_SUCCESS) {
        throw std::runtime_error("failed to create window surface!");
    }
    m_surface = vk::SurfaceKHR(rawSurface);

    // Setup camera
    CameraManipulator.setWindowSize(m_size.width, m_size.height);
}

//-------------------------------------------------------------------------
// Pick Physical Device
//
void VulkanBackend::pickPhysicalDevice(const ContextCreateInfo& info)
{
    std::vector<vk::PhysicalDevice> devices = m_instance.enumeratePhysicalDevices();
    if (devices.size() == 0)
        throw std::runtime_error("failed to find GPUs with Vulkan support!");

    // Find a GPU
    for (auto device : devices) {
        uint32_t graphicsIdx = -1;
        uint32_t presentIdx = -1;

        auto queueFamilyProperties = device.getQueueFamilyProperties();
        auto deviceExtensionProperties = device.enumerateDeviceExtensionProperties();

        if (device.getSurfaceFormatsKHR(m_surface).size() == 0) continue;
        if (device.getSurfacePresentModesKHR(m_surface).size() == 0) continue;
        if (!checkDeviceExtensionSupport(info, deviceExtensionProperties))
            continue;

        // supports graphics and compute?
        for (uint32_t j = 0; j < queueFamilyProperties.size(); ++j) {
            vk::QueueFamilyProperties& queueFamily = queueFamilyProperties[j];

            if (queueFamily.queueCount == 0) continue;

            if (queueFamily.queueFlags
                & (vk::QueueFlagBits::eGraphics
                    | vk::QueueFlagBits::eCompute
                    | vk::QueueFlagBits::eTransfer)) {
                graphicsIdx = j;
                break;
            }
        }

        // present queue 
        for (uint32_t j = 0; j < queueFamilyProperties.size(); ++j) {
            vk::QueueFamilyProperties& queueFamily = queueFamilyProperties[j];

            if (queueFamily.queueCount == 0) continue;

            vk::Bool32 supportsPresent = VK_FALSE;
            supportsPresent = device.getSurfaceSupportKHR(j, m_surface);
            if (supportsPresent) {
                presentIdx = j;
                break;
            }
        }

        if (graphicsIdx >= 0 && presentIdx >= 0) {
            m_physicalDevice = device;
            m_graphicsQueueIdx = graphicsIdx;
            m_presentQueueIdx = presentIdx;

            m_vsync = false;
            m_depthFormat = vk::Format::eD32SfloatS8Uint;
            m_colorFormat = vk::Format::eB8G8R8A8Unorm;
            
            VkPhysicalDeviceProperties physicalDeviceProperties = m_physicalDevice.getProperties();
            VkSampleCountFlags rawCounts = (std::min)(physicalDeviceProperties.limits.framebufferColorSampleCounts, physicalDeviceProperties.limits.framebufferDepthSampleCounts);
            vk::SampleCountFlags counts(rawCounts);

            if (counts & vk::SampleCountFlagBits::e64) { m_sampleCount = vk::SampleCountFlagBits::e64; }
            if (counts & vk::SampleCountFlagBits::e32) { m_sampleCount = vk::SampleCountFlagBits::e32; }
            if (counts & vk::SampleCountFlagBits::e16) { m_sampleCount = vk::SampleCountFlagBits::e16; }
            if (counts & vk::SampleCountFlagBits::e8) { m_sampleCount = vk::SampleCountFlagBits::e8; }
            if (counts & vk::SampleCountFlagBits::e4) { m_sampleCount = vk::SampleCountFlagBits::e4; }
            if (counts & vk::SampleCountFlagBits::e2) { m_sampleCount = vk::SampleCountFlagBits::e2; }

            return;
        }
    }

    // Could not find suitable device
    if (!m_physicalDevice) {
        throw std::runtime_error("failed to find a suitable GPU!");
    }
}

//-------------------------------------------------------------------------
// Create Vulkan Device and Queues
//
void VulkanBackend::createLogicalDeviceAndQueues(const ContextCreateInfo& info)
{
    auto queueFamilyProperties = m_physicalDevice.getQueueFamilyProperties();

    std::vector<vk::DeviceQueueCreateInfo> queueCreateInfos;
    std::set<uint32_t> uniqueQueueFamilies = { m_graphicsQueueIdx,  m_presentQueueIdx };

    const float queuePriority = 1.0f;
    for (uint32_t queueFamily : uniqueQueueFamilies) {
        vk::DeviceQueueCreateInfo queueInfo = {};
        queueInfo.queueFamilyIndex = queueFamily;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &queuePriority;

        queueCreateInfos.push_back(queueInfo);
    }
    vk::PhysicalDeviceDescriptorIndexingFeaturesEXT indexFeature = {};

    vk::PhysicalDeviceScalarBlockLayoutFeaturesEXT  scalarFeature = {};
    scalarFeature.pNext = &indexFeature;

    // Vulkan >= 1.1 uses pNext to enable features, and not pEnabledFeatures
    vk::PhysicalDeviceFeatures2 enabledFeatures2 = {};
    enabledFeatures2.features = m_physicalDevice.getFeatures();
    enabledFeatures2.features.samplerAnisotropy = VK_TRUE;
    enabledFeatures2.pNext = &scalarFeature;
    m_physicalDevice.getFeatures2(&enabledFeatures2);

    vk::DeviceCreateInfo deviceCreateInfo = {};
    deviceCreateInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
    deviceCreateInfo.pQueueCreateInfos = queueCreateInfos.data();
    deviceCreateInfo.enabledExtensionCount = info.numDeviceExtensions;
    deviceCreateInfo.ppEnabledExtensionNames = info.deviceExtensions.data();
    deviceCreateInfo.pEnabledFeatures = nullptr;
    deviceCreateInfo.pNext = &enabledFeatures2;

    if (info.enableValidationLayers) {
        deviceCreateInfo.enabledLayerCount = info.numValidationLayers;
        deviceCreateInfo.ppEnabledLayerNames = info.validationLayers.data();
    }

    try {
        m_device = m_physicalDevice.createDevice(deviceCreateInfo);
    }
    catch (vk::SystemError err) {
        throw std::runtime_error("failed to create logical device!");
    }

    // Initialize function pointers
    VULKAN_HPP_DEFAULT_DISPATCHER.init(m_device);

    // Initialize default queues
    m_graphicsQueue = m_device.getQueue(m_graphicsQueueIdx, 0);
    m_presentQueue = m_device.getQueue(m_presentQueueIdx, 0);

    // Initialize debugging tool for queue object names
#if _DEBUG
    m_device.setDebugUtilsObjectNameEXT(
        { vk::ObjectType::eQueue, (uint64_t)(VkQueue)m_graphicsQueue, "graphicsQueue" });

    m_device.setDebugUtilsObjectNameEXT(
        { vk::ObjectType::eQueue, (uint64_t)(VkQueue)m_presentQueue, "presentQueue" });
#endif
}

//-------------------------------------------------------------------------
// Create Swapchain
//
void VulkanBackend::createSwapChain()
{
    m_swapchain.init(m_instance, m_device, m_physicalDevice, m_graphicsQueue, m_graphicsQueueIdx,
        m_presentQueue, m_presentQueueIdx, m_surface, vk::Format::eB8G8R8A8Unorm);

    m_swapchain.update(m_size.width, m_size.height, m_vsync);

    m_colorFormat = m_swapchain.getFormat();
}

//-------------------------------------------------------------------------
// Create Command Pool
//
void VulkanBackend::createCommandPool()
{
    vk::CommandPoolCreateInfo poolInfo;
    poolInfo.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
    poolInfo.queueFamilyIndex = m_graphicsQueueIdx;

    try {
        m_commandPool = m_device.createCommandPool(poolInfo);
    }
    catch (vk::SystemError err) {
        throw std::runtime_error("failed to create command pool!");
    }
}

//-------------------------------------------------------------------------
// Create Command Buffers, store a reference to framebuffer inside their 
// render pass info
//
void VulkanBackend::createCommandBuffer()
{
    m_commandBuffers.resize(m_swapchain.getImageCount());

    vk::CommandBufferAllocateInfo cmdBufferAllocInfo = {};
    cmdBufferAllocInfo.commandPool = m_commandPool;
    cmdBufferAllocInfo.level = vk::CommandBufferLevel::ePrimary;
    cmdBufferAllocInfo.commandBufferCount = m_swapchain.getImageCount();

    try {
        m_commandBuffers = m_device.allocateCommandBuffers(cmdBufferAllocInfo);
    }
    catch (vk::SystemError err) {
        throw std::runtime_error("failed to allocate command buffers!");
    }

#if _DEBUG
    for (size_t i = 0; i < m_commandBuffers.size(); i++) {
        std::string name = std::string("createCmdVulkanBackend") + std::to_string(i);
        m_device.setDebugUtilsObjectNameEXT(
            { vk::ObjectType::eCommandBuffer, 
            reinterpret_cast<const uint64_t&>(m_commandBuffers[i]), name.c_str() });
    }
#endif
}

//-------------------------------------------------------------------------
// Create basic renderpass, most likely to be overwritten
//
void VulkanBackend::createRenderPass()
{
    if (m_renderPass) m_device.destroy(m_renderPass);

    // Color Attachment
    vk::AttachmentDescription colorAttachment = {};
    colorAttachment.format = m_colorFormat;
    colorAttachment.samples = vk::SampleCountFlagBits::e1;
    colorAttachment.loadOp = vk::AttachmentLoadOp::eClear;
    colorAttachment.storeOp = vk::AttachmentStoreOp::eStore;
    colorAttachment.stencilLoadOp = vk::AttachmentLoadOp::eDontCare;
    colorAttachment.stencilStoreOp = vk::AttachmentStoreOp::eDontCare;
    colorAttachment.initialLayout = vk::ImageLayout::eUndefined;
    colorAttachment.finalLayout = vk::ImageLayout::ePresentSrcKHR;

    // Depth Attachment
    vk::AttachmentDescription depthAttachment = {};
    depthAttachment.format = m_depthFormat;
    depthAttachment.samples = vk::SampleCountFlagBits::e1;
    depthAttachment.loadOp = vk::AttachmentLoadOp::eClear;
    depthAttachment.storeOp = vk::AttachmentStoreOp::eStore;
    depthAttachment.stencilLoadOp = vk::AttachmentLoadOp::eDontCare;
    depthAttachment.stencilStoreOp = vk::AttachmentStoreOp::eDontCare;
    depthAttachment.initialLayout = vk::ImageLayout::eUndefined;
    depthAttachment.finalLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal;
    
    const vk::AttachmentReference colorReference{ 0,  vk::ImageLayout::eColorAttachmentOptimal };
    const vk::AttachmentReference depthReference{ 1, vk::ImageLayout::eDepthStencilAttachmentOptimal };
   
    std::array<vk::AttachmentDescription, 2> attachments = { colorAttachment, depthAttachment };

    vk::SubpassDescription subpass = {};
    subpass.pipelineBindPoint = vk::PipelineBindPoint::eGraphics;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorReference;
    subpass.pDepthStencilAttachment = &depthReference;

    vk::SubpassDependency dependency = {};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput;
    dependency.dstStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput;
    dependency.srcAccessMask = vk::AccessFlagBits::eMemoryRead;
    dependency.dstAccessMask = vk::AccessFlagBits::eColorAttachmentRead | vk::AccessFlagBits::eColorAttachmentWrite;
    dependency.dependencyFlags = vk::DependencyFlagBits::eByRegion;

    vk::RenderPassCreateInfo renderPassInfo = {};
    renderPassInfo.attachmentCount = 2;
    renderPassInfo.pAttachments = attachments.data();
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = 1;
    renderPassInfo.pDependencies = &dependency;

    try {
        m_renderPass = m_device.createRenderPass(renderPassInfo);
    }
    catch (vk::SystemError err) {
        throw std::runtime_error("failed to create render pass!");
    }

#ifdef _DEBUG
    m_device.setDebugUtilsObjectNameEXT(
        { vk::ObjectType::eRenderPass, reinterpret_cast<const uint64_t&>(m_renderPass), "renderPassVulkanBackend" });
#endif  
}

//-------------------------------------------------------------------------
// Create Pipeline Cache
//
void VulkanBackend::createPipelineCache()
{
    try {
        m_pipelineCache = m_device.createPipelineCache(vk::PipelineCacheCreateInfo());
    }
    catch (vk::SystemError err) {
        throw std::runtime_error("failed to create pipeline cache!");
    }
}


//-------------------------------------------------------------------------
// Image to be used as depth buffer
//
void VulkanBackend::createDepthBuffer()
{
    m_device.destroyImageView(m_depthView);
    m_device.destroyImage(m_depthImage);
    m_device.freeMemory(m_depthMemory);

    // Depth Info
    const vk::ImageAspectFlags aspect =
        vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil;

    vk::ImageCreateInfo depthStencilCreateInfo = {};
    depthStencilCreateInfo.imageType = vk::ImageType::e2D;
    depthStencilCreateInfo.extent = vk::Extent3D(m_size.width, m_size.height, 1);
    depthStencilCreateInfo.format = m_depthFormat;
    depthStencilCreateInfo.mipLevels = 1;
    depthStencilCreateInfo.arrayLayers = 1;
    depthStencilCreateInfo.samples = vk::SampleCountFlagBits::e1;
    depthStencilCreateInfo.usage = vk::ImageUsageFlagBits::eDepthStencilAttachment
        | vk::ImageUsageFlagBits::eTransferSrc;

    try {
        m_depthImage = m_device.createImage(depthStencilCreateInfo);
    }
    catch (vk::SystemError err) {
        throw std::runtime_error("failed to create depth images!");
    }

    // find memory requirements
    const vk::MemoryRequirements memReqs = m_device.getImageMemoryRequirements(m_depthImage);
    uint32_t memoryTypeIdx = -1;
    {
        auto deviceMemoryProperties = m_physicalDevice.getMemoryProperties();
        for (uint32_t i = 0; i < deviceMemoryProperties.memoryTypeCount; i++) {
            if ((memReqs.memoryTypeBits & (1 << i))
                && (deviceMemoryProperties.memoryTypes[i].propertyFlags & vk::MemoryPropertyFlagBits::eDeviceLocal) == vk::MemoryPropertyFlagBits::eDeviceLocal) {
                memoryTypeIdx = i;
                break;
            }
        }
        if (memoryTypeIdx == -1)
            throw std::runtime_error("failed to find suitable memory type!");
    }

    //Allocate the memory
    vk::MemoryAllocateInfo memAllocInfo = {};
    memAllocInfo.allocationSize  = memReqs.size;
    memAllocInfo.memoryTypeIndex = memoryTypeIdx;

    try {
        m_depthMemory = m_device.allocateMemory(memAllocInfo);
    }
    catch (vk::SystemError err) {
        throw std::runtime_error("failed to allocate depth image memory!");
    }

    // Bind image & memory
    m_device.bindImageMemory(m_depthImage, m_depthMemory, 0);

    // Create an image barrier to change the layout from undefined to DepthStencilAttachmentOptimal
    vk::CommandBufferAllocateInfo cmdBufAllocateInfo = {};
    cmdBufAllocateInfo.commandPool = m_commandPool;
    cmdBufAllocateInfo.level = vk::CommandBufferLevel::ePrimary;
    cmdBufAllocateInfo.commandBufferCount = 1;

    vk::CommandBuffer cmdBuffer;
    cmdBuffer = m_device.allocateCommandBuffers(cmdBufAllocateInfo)[0];
    cmdBuffer.begin(vk::CommandBufferBeginInfo{ vk::CommandBufferUsageFlagBits::eOneTimeSubmit });

    // barrier on top, barrier inside set up cmdbuffer
    vk::ImageSubresourceRange subresourceRange;
    subresourceRange.aspectMask = aspect;
    subresourceRange.levelCount = 1;
    subresourceRange.layerCount = 1;

    vk::ImageMemoryBarrier imageMemoryBarrier;
    imageMemoryBarrier.oldLayout           = vk::ImageLayout::eUndefined;
    imageMemoryBarrier.newLayout           = vk::ImageLayout::eDepthStencilAttachmentOptimal;
    imageMemoryBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    imageMemoryBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    imageMemoryBarrier.image               = m_depthImage;
    imageMemoryBarrier.subresourceRange    = subresourceRange;
    imageMemoryBarrier.srcAccessMask       = vk::AccessFlags();
    imageMemoryBarrier.dstAccessMask       = vk::AccessFlagBits::eDepthStencilAttachmentWrite
                                           | vk::AccessFlagBits::eDepthStencilAttachmentRead;

    const vk::PipelineStageFlags srcStageMask  = vk::PipelineStageFlagBits::eTopOfPipe;
    const vk::PipelineStageFlags destStageMask = vk::PipelineStageFlagBits::eEarlyFragmentTests;

    cmdBuffer.pipelineBarrier(srcStageMask, destStageMask, vk::DependencyFlags(),
        nullptr, nullptr, imageMemoryBarrier);
    cmdBuffer.end();

    m_graphicsQueue.submit(vk::SubmitInfo{ 0, nullptr, nullptr, 1, &cmdBuffer }, vk::Fence());

    m_graphicsQueue.waitIdle();

    m_device.freeCommandBuffers(m_commandPool, cmdBuffer);

    // Setting up the view
    vk::ImageViewCreateInfo depthStencilView;
    depthStencilView.viewType         = vk::ImageViewType::e2D;
    depthStencilView.format           = m_depthFormat;
    depthStencilView.subresourceRange = { aspect, 0, 1, 0, 1 };
    depthStencilView.image            = m_depthImage;
    try {
        m_depthView = m_device.createImageView(depthStencilView);
    }
    catch (vk::SystemError err) {
        throw std::runtime_error("failed to create depth image view!");
    }
}

//-------------------------------------------------------------------------
//  Create the frame buffers where the image will be rendered 
// (Swapchain must be created before)
//
void VulkanBackend::createFrameBuffers()
{
    // recreate frame buffers
    for (auto framebuffer : m_framebuffers)
        m_device.destroyFramebuffer(framebuffer);
    m_framebuffers.resize(m_swapchain.getImageCount());

    // create frame buffer for every swapchain image
    for (uint32_t i = 0; i < m_swapchain.getImageCount(); i++) {
        std::array<vk::ImageView, 2> attachments;
        attachments[0] = m_swapchain.getImageView(i);
        attachments[1] = m_depthView;

        vk::FramebufferCreateInfo framebufferInfo = {};
        framebufferInfo.renderPass      = m_renderPass;
        framebufferInfo.attachmentCount = 2;
        framebufferInfo.pAttachments    = attachments.data();
        framebufferInfo.width           = m_size.width;
        framebufferInfo.height          = m_size.height;
        framebufferInfo.layers          = 1;

        try {
            m_framebuffers[i] = m_device.createFramebuffer(framebufferInfo);
        }
        catch (vk::SystemError err) {
            throw std::runtime_error("failed to create framebuffer!");
        }
    }

#ifdef _DEBUG
    for (size_t i = 0; i < m_framebuffers.size(); i++) {
        std::string name = std::string("frameBufferVulkanBackend") + std::to_string(i);
        m_device.setDebugUtilsObjectNameEXT(
            { vk::ObjectType::eFramebuffer, reinterpret_cast<const uint64_t&>(m_framebuffers[i]), name.c_str() });
    }
#endif
}

//-------------------------------------------------------------------------
// Create Sync Objects
// Fences - are used to synchronize the CPU and the GPU
//
void VulkanBackend::createSyncObjects()
{
    m_fences.resize(m_swapchain.getImageCount());

    try {
        for (uint32_t i = 0; i < m_swapchain.getImageCount(); ++i) {
            m_fences[i] = m_device.createFence({ vk::FenceCreateFlagBits::eSignaled });
        }
    }
    catch (vk::SystemError err) {
        throw std::runtime_error("failed to create synchronization objects for a frame!");
    }
}

//-------------------------------------------------------------------------
// function to call before rendering
//
void VulkanBackend::prepareFrame()
{
    // Acquire the next image from the swap chain
    auto result = m_swapchain.acquire();    
    // Recreate the swapchain if it's no longer compatible with the surface
    if (result == vk::Result::eErrorOutOfDateKHR || result == vk::Result::eSuboptimalKHR) {
        onWindowResize(m_size.width, m_size.height);
    }
    else if (result != vk::Result::eSuccess) {
        throw std::runtime_error("failed to acquire image from swapchain!");
    }

    // fence until cmd buffer has finished executing before using again
    uint32_t imageIndex = m_swapchain.getActiveImageIndex();
    while (m_device.waitForFences(m_fences[imageIndex], VK_TRUE, 10000) == vk::Result::eTimeout) {}
}

//-------------------------------------------------------------------------
// function to call for submitting the rendering command
//
void VulkanBackend::submitFrame()
{
    uint32_t imageIndex = m_swapchain.getActiveImageIndex();
    m_device.resetFences(m_fences[imageIndex]);

    vk::Semaphore semaphoreRead  = m_swapchain.getActiveReadSemaphore();
    vk::Semaphore semaphoreWrite = m_swapchain.getActiveWrittenSemaphore();

    // Pipeline stage at which the queue submission will wait (via pWaitSemaphores)
    const vk::PipelineStageFlags waitStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput;

    vk::SubmitInfo submitInfo = {};
    submitInfo.waitSemaphoreCount   = 1;                              // One wait semaphore
    submitInfo.pWaitSemaphores      = &semaphoreRead;                 // Semaphore(s) to wait upon before the submitted command buffer starts executing
    submitInfo.pWaitDstStageMask    = &waitStageMask;                 // Pointer to the list of pipeline stages that the semaphore waits will occur at
    submitInfo.commandBufferCount   = 1;                              // One Command Buffer
    submitInfo.pCommandBuffers      = &m_commandBuffers[imageIndex];  // Command buffers(s) to execute in this batch (submission)
    submitInfo.signalSemaphoreCount = 1;                              // One signal Semaphore
    submitInfo.pSignalSemaphores    = &semaphoreWrite;                // Semaphore(s) to be signaled when command buffers have completed

    // Submit to the graphics queue passing a wait fence
    try {
        m_graphicsQueue.submit(submitInfo, m_fences[imageIndex]);
    }
    catch (vk::SystemError err) {
        throw std::runtime_error("failed to submit draw command buffer!");
    }

    m_swapchain.present(m_graphicsQueue);
}

//-------------------------------------------------------------------------
// When the pipeline is set for using dynamic, this becomes useful
//
void VulkanBackend::setViewport(const vk::CommandBuffer& cmdBuffer)
{
    vk::Viewport viewport = {};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = (float)m_size.width;
    viewport.height = (float)m_size.height;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;

    vk::Rect2D scissor = {};
    scissor.offset = vk::Offset2D{ 0,0 };
    scissor.extent = m_size;

    cmdBuffer.setViewport(0, { viewport });
    cmdBuffer.setScissor(0, { scissor });
}

//-------------------------------------------------------------------------
// Returns true if window is minimized
//
bool VulkanBackend::isMinimized(bool doSleeping)
{
    int w, h;
    glfwGetWindowSize(m_window, &w, &h);
    bool minimized(w == 0 || h == 0);
    if (minimized && doSleeping)
    {
#ifdef _WIN32
        Sleep(50);
#else
        usleep(50);
#endif
    }
    return minimized;
}


///////////////////////////////////////////////////////////////////////////
// GLFW Callbacks / ImGUI                                                //
///////////////////////////////////////////////////////////////////////////

//-------------------------------------------------------------------------
// GLFW Callback setup
//
void VulkanBackend::setupGlfwCallbacks(GLFWwindow* window)
{
    m_window = window;

    glfwSetWindowUserPointer(window, this);
    glfwSetKeyCallback(window, &onKeyCallback);
    glfwSetCharCallback(window, &onCharCallback);
    glfwSetCursorPosCallback(window, &onMouseMoveCallback);
    glfwSetMouseButtonCallback(window, &onMouseButtonCallback);
    glfwSetScrollCallback(window, &onScrollCallback);
    glfwSetWindowSizeCallback(window, &onWindowSizeCallback);
}

//-------------------------------------------------------------------------
// On Key Callback
// - Handling ImGui and a default camera
//
void VulkanBackend::onKeyboard(int key, int scancode, int action, int mods)
{
    const bool capture = 
        ImGui::GetCurrentContext() != nullptr 
        && ImGui::GetIO().WantCaptureKeyboard;

    const bool pressed = action != GLFW_RELEASE;

    if (key == GLFW_KEY_LEFT_CONTROL)
        m_inputs.ctrl = pressed;
    
    if (key == GLFW_KEY_LEFT_SHIFT)
        m_inputs.shift = pressed;
    
    if (key == GLFW_KEY_LEFT_ALT)
        m_inputs.alt = pressed;    

    if (action == GLFW_RELEASE || capture)
        return;

    switch (key) {
    case GLFW_KEY_ESCAPE:
        glfwSetWindowShouldClose(m_window, 1);
        break;
    case GLFW_KEY_LEFT:
        m_inertCamera.tau = s_keyTau;
        m_inertCamera.rotateH(s_moveStep, m_inputs.ctrl);
        break;
    case GLFW_KEY_UP:
        m_inertCamera.tau = s_keyTau;
        m_inertCamera.rotateV(s_moveStep, m_inputs.ctrl);
        break;
    case GLFW_KEY_RIGHT:
        m_inertCamera.tau = s_keyTau;
        m_inertCamera.rotateH(-s_moveStep, m_inputs.ctrl);
        break;
    case GLFW_KEY_DOWN:
        m_inertCamera.tau = s_keyTau;
        m_inertCamera.rotateV(-s_moveStep, m_inputs.ctrl);
        break;
    case GLFW_KEY_PAGE_UP:
        m_inertCamera.tau = s_keyTau;
        m_inertCamera.move(s_moveStep, m_inputs.ctrl);
        break;
    case GLFW_KEY_PAGE_DOWN:
        m_inertCamera.tau = s_keyTau;
        m_inertCamera.move(-s_moveStep, m_inputs.ctrl);
        break;
    default:
        break;
    }
}

void VulkanBackend::onKeyCallback(GLFWwindow* window, int key, int scancode, int action, int mods)
{
    auto app = reinterpret_cast<VulkanBackend*>(glfwGetWindowUserPointer(window));
    app->onKeyboard(key, scancode, action, mods);
}

//-------------------------------------------------------------------------
// On Char Callback
//
void VulkanBackend::onKeyboardChar(unsigned int key)
{
    if (ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().WantCaptureMouse)
        return;

    // Toggling vsync TODO
    if (key == 'v') {

    }
}

void VulkanBackend::onCharCallback(GLFWwindow* window, unsigned int key)
{
    auto app = reinterpret_cast<VulkanBackend*>(glfwGetWindowUserPointer(window));
    app->onKeyboardChar(key);
}

//-------------------------------------------------------------------------
// On Mouse Mouve Callback
// - Handling ImGui and a default camera
//
void VulkanBackend::onMouseMove(int x, int y)
{
    if (ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().WantCaptureMouse) 
        return;
    
    // Old mouse position
    int ox, oy;  
    CameraManipulator.getMousePosition(ox, oy);

    if (m_inputs.lmb || m_inputs.rmb || m_inputs.mmb)
        CameraManipulator.mouseMove(x, y, m_inputs);
    
    // difference mouse position
    const int dx = x - ox, dy = y - oy;  

    // Left Mouse Button
    if (m_inputs.lmb) {
        const float hval = 2 * dx / static_cast<float>(m_size.width);
        const float vval = 2 * dy / static_cast<float>(m_size.height);
        m_inertCamera.tau = s_cameraTau;
        m_inertCamera.rotateH(hval);
        m_inertCamera.rotateV(vval);
    }

    //Middle Mouse Button
    if (m_inputs.mmb) {
        const float hval = 2 * dx / static_cast<float>(m_size.width);
        const float vval = 2 * dy / static_cast<float>(m_size.height);
        m_inertCamera.tau = s_cameraTau;
        m_inertCamera.rotateH(hval, true);
        m_inertCamera.rotateV(vval, true);
    }

    //Right mouse button
    if (m_inputs.rmb) {
        const float hval = 2 * dx / static_cast<float>(m_size.width);
        const float vval = -2 * dy / static_cast<float>(m_size.height);
        m_inertCamera.tau = s_cameraTau;
        m_inertCamera.rotateH(hval, m_inputs.ctrl);
        m_inertCamera.move(vval, m_inputs.ctrl);
    }
}

void VulkanBackend::onMouseMoveCallback(GLFWwindow* window, double x, double y)
{
    auto app = reinterpret_cast<VulkanBackend*>(glfwGetWindowUserPointer(window));
    app->onMouseMove(static_cast<int>(x), static_cast<int>(y));
}

//-------------------------------------------------------------------------
// On Mouse Button Callback
// - Handling ImGui and a default camera
//
void VulkanBackend::onMouseButton(int button, int action, int mod)
{
    if (ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().WantCaptureMouse)
        return;

    double xpos, ypos;
    glfwGetCursorPos(m_window, &xpos, &ypos);
    CameraManipulator.setMousePosition(static_cast<int>(xpos), static_cast<int>(ypos));

    m_inputs.lmb = (button == GLFW_MOUSE_BUTTON_LEFT) && (action == GLFW_PRESS);
    m_inputs.mmb = (button == GLFW_MOUSE_BUTTON_MIDDLE) && (action == GLFW_PRESS);
    m_inputs.rmb = (button == GLFW_MOUSE_BUTTON_RIGHT) && (action == GLFW_PRESS);
}

void VulkanBackend::onMouseButtonCallback(GLFWwindow* window, int button, int action, int mods)
{
    auto app = reinterpret_cast<VulkanBackend*>(glfwGetWindowUserPointer(window));
    app->onMouseButton(button, action, mods);
}

//-------------------------------------------------------------------------
// On Scroll Callback 
// - Handling ImGui and a default camera
//
void VulkanBackend::onScroll(int delta)
{
    if (ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().WantCaptureMouse)
        return;

    CameraManipulator.wheel(delta > 0 ? 1 : -1, m_inputs);

    m_inertCamera.tau = s_keyTau;
    m_inertCamera.move(delta > 0 ? s_moveStep : -s_moveStep, m_inputs.ctrl);
}

void VulkanBackend::onScrollCallback(GLFWwindow* window, double x, double y)
{
    auto app = reinterpret_cast<VulkanBackend*>(glfwGetWindowUserPointer(window));
    app->onScroll(static_cast<int>(y));
}

//-------------------------------------------------------------------------
// On Window Size Callback
// - Destroy allocated frames, then rebuild with new size
//
void VulkanBackend::onWindowResize(uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0) return;

    m_size.width = width;
    m_size.height = height;

    if (ImGui::GetCurrentContext() != nullptr)
    {
        auto& imgui_io = ImGui::GetIO();
        imgui_io.DisplaySize = ImVec2(static_cast<float>(width), static_cast<float>(height));
    }
    CameraManipulator.setWindowSize(width, height);


    m_device.waitIdle();
    m_graphicsQueue.waitIdle();

    m_swapchain.update(m_size.width, m_size.height, m_vsync);
    onResize(width, height);
    createDepthBuffer();
    createFrameBuffers();
}

void VulkanBackend::onWindowSizeCallback(GLFWwindow* window, int w, int h)
{
    auto app = reinterpret_cast<VulkanBackend*>(glfwGetWindowUserPointer(window));
    app->onWindowResize(w, h);
}

//--------------------------------------------------------------------------------------------------
// 
//
static void checkVkResult(VkResult err)
{
    if (err == 0)
        return;
    printf("VkResult %d\n", err);
    if (err < 0)
        abort();
}

//-------------------------------------------------------------------------
// Initialization of the GUI
// - Called AFTER device creation
//
void VulkanBackend::initGUI(GLFWwindow* window)
{
    assert(m_renderPass && "Render Pass must be set");

    // create pool onfo descriptor used by ImGUI
    std::vector<vk::DescriptorPoolSize> counters{ {vk::DescriptorType::eCombinedImageSampler, 2} };

    vk::DescriptorPoolCreateInfo poolInfo = {};
    poolInfo.poolSizeCount = static_cast<uint32_t>(counters.size());
    poolInfo.pPoolSizes    = counters.data();
    poolInfo.maxSets       = static_cast<uint32_t>(counters.size());

    m_imguiDescPool = m_device.createDescriptorPool(poolInfo);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;

    ImGui_ImplVulkan_InitInfo imGuiInitInfo = {};
    imGuiInitInfo.Allocator       = nullptr;
    imGuiInitInfo.DescriptorPool  = m_imguiDescPool;
    imGuiInitInfo.Device          = m_device;
    imGuiInitInfo.ImageCount      = (uint32_t)m_framebuffers.size();
    imGuiInitInfo.Instance        = m_instance;
    imGuiInitInfo.MinImageCount   = (uint32_t)m_framebuffers.size();
    imGuiInitInfo.PhysicalDevice  = m_physicalDevice;
    imGuiInitInfo.PipelineCache   = m_pipelineCache;
    imGuiInitInfo.Queue           = m_graphicsQueue;
    imGuiInitInfo.QueueFamily     = m_graphicsQueueIdx;
    imGuiInitInfo.MSAASamples     = VK_SAMPLE_COUNT_1_BIT;
    imGuiInitInfo.CheckVkResultFn = checkVkResult;

    ImGui_ImplVulkan_Init(&imGuiInitInfo, m_renderPass);

    // Setup style
    ImGui::StyleColorsDark();

    // Upload Fonts
    app::CommandPool cmdBufferGen(m_device, m_graphicsQueueIdx);

    auto cmdBuffer = cmdBufferGen.createBuffer();
    ImGui_ImplVulkan_CreateFontsTexture(cmdBuffer);
    cmdBufferGen.submitAndWait(cmdBuffer);

    ImGui_ImplVulkan_DestroyFontUploadObjects();

    ImGui::GetIO().IniFilename = nullptr;
}

//-------------------------------------------------------------------------
// Fit the camera to the Bounding box
//
void VulkanBackend::fitCamera(const glm::vec3& boxMin, const glm::vec3 boxMax, bool instantFit)
{
    CameraManipulator.fit(boxMin, boxMax, instantFit);
}

///////////////////////////////////////////////////////////////////////////
// Debug                                                                 //
///////////////////////////////////////////////////////////////////////////

//-------------------------------------------------------------------------
// 
//
static VKAPI_ATTR VkBool32 VKAPI_CALL
debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
    VkDebugUtilsMessageTypeFlagsEXT messageType,
    const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
    void* pUserData)
{
    std::cerr << "validation layer: " << pCallbackData->pMessage << std::endl;
    return VK_FALSE;
}

//-------------------------------------------------------------------------
// Set up Debug Messenger
//
void VulkanBackend::setupDebugMessenger(bool enableValidationLayers)
{
    if (!enableValidationLayers) return;

    vk::DebugUtilsMessengerCreateInfoEXT debugInfo = {};
    debugInfo.messageSeverity = vk::DebugUtilsMessageSeverityFlagBitsEXT::eError
                              | vk::DebugUtilsMessageSeverityFlagBitsEXT::eVerbose
                              | vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning;
    debugInfo.messageType     = vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral
                              | vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation
                              | vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance;
    debugInfo.pfnUserCallback = debugCallback;

    try {
        m_debugMessenger = m_instance.createDebugUtilsMessengerEXT(debugInfo);
    }
    catch (vk::SystemError err) {
        throw std::runtime_error("failed to set up debug messenger!");
    }
}

//-------------------------------------------------------------------------
// check Validation Layer Support
//
bool VulkanBackend::checkValidationLayerSupport(const ContextCreateInfo& info)
{
    std::vector<vk::LayerProperties> availableLayers =
        vk::enumerateInstanceLayerProperties();

    for (const char* layerName : info.validationLayers) {
        bool layerFound = false;

        for (const auto& layerProperties : availableLayers) {
            if (strcmp(layerName, layerProperties.layerName) == 0) {
                layerFound = true;
                break;
            }
        }

        if (!layerFound) {
            return false;
        }
    }

    return true;
}

//-------------------------------------------------------------------------
// check Device Extension Support
//
bool VulkanBackend::checkDeviceExtensionSupport(const ContextCreateInfo& info, std::vector<vk::ExtensionProperties>& extensionProperties)
{
    std::set<std::string> requiredExtensions(info.deviceExtensions.begin(), info.deviceExtensions.end());

    for (const auto& extension : extensionProperties) {
        requiredExtensions.erase(extension.extensionName);
    }

    return requiredExtensions.empty();
}


///////////////////////////////////////////////////////////////////////////
// ContextCreateInfo                                                     //
///////////////////////////////////////////////////////////////////////////

//-------------------------------------------------------------------------
// 
//
ContextCreateInfo::ContextCreateInfo()
{
    numDeviceExtensions = 0;
    deviceExtensions    = std::vector<const char*>();

    numValidationLayers = 0;
    validationLayers    = std::vector<const char*>();

    numInstanceExtensions = 0;
    instanceExtensions    = std::vector<const char*>();

    if (enableValidationLayers) {
        numValidationLayers++;
        validationLayers.emplace_back("VK_LAYER_KHRONOS_validation");

        numInstanceExtensions++;
        instanceExtensions.emplace_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }
}

//-------------------------------------------------------------------------
// 
//
void ContextCreateInfo::addDeviceExtension(const char* name)
{
    numDeviceExtensions++;
    deviceExtensions.emplace_back(name);
}


//-------------------------------------------------------------------------
// 
//
void ContextCreateInfo::addInstanceExtension(const char* name)
{
    numInstanceExtensions++;
    instanceExtensions.emplace_back(name);
}

//-------------------------------------------------------------------------
// 
//
void ContextCreateInfo::addValidationLayer(const char* name)
{
    numValidationLayers++;
    deviceExtensions.emplace_back(name);
}

} // namespace app