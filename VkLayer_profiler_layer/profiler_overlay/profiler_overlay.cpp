// Copyright (c) 2019-2021 Lukasz Stalmirski
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// 
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "profiler_overlay.h"
#include "profiler_trace/profiler_trace.h"
#include "profiler_helpers/profiler_data_helpers.h"

#include "imgui_impl_vulkan_layer.h"
#include <string>
#include <sstream>
#include <stack>
#include <fstream>

#include "utils/lockable_unordered_map.h"

#include "imgui_widgets/imgui_breakdown_ex.h"
#include "imgui_widgets/imgui_histogram_ex.h"
#include "imgui_widgets/imgui_table_ex.h"
#include "imgui_widgets/imgui_ex.h"

// Languages
#include "lang/en_us.h"
#include "lang/pl_pl.h"

#if 1
using Lang = Profiler::DeviceProfilerOverlayLanguage_Base;
#else
using Lang = Profiler::DeviceProfilerOverlayLanguage_PL;
#endif

#ifdef VK_USE_PLATFORM_WIN32_KHR
#include "imgui_impl_win32.h"
#endif
#ifdef WIN32
#include <ShlObj.h> // SHGetKnownFolderPath
#endif

#ifdef VK_USE_PLATFORM_WAYLAND_KHR
#include <wayland-client.h>
#endif

#ifdef VK_USE_PLATFORM_XCB_KHR
#include "imgui_impl_xcb.h"
#endif

#ifdef VK_USE_PLATFORM_XLIB_KHR
#include "imgui_impl_xlib.h"
#endif

#ifdef VK_USE_PLATFORM_XLIB_XRANDR_EXT
#include <X11/extensions/Xrandr.h>
#endif

namespace Profiler
{
    // Define static members
    std::mutex ProfilerOverlayOutput::s_ImGuiMutex;

    /***********************************************************************************\

    Function:
        ProfilerOverlayOutput

    Description:
        Constructor.

    \***********************************************************************************/
    ProfilerOverlayOutput::ProfilerOverlayOutput()
        : m_pDevice( nullptr )
        , m_pGraphicsQueue( nullptr )
        , m_pSwapchain( nullptr )
        , m_Window()
        , m_pImGuiContext( nullptr )
        , m_pImGuiVulkanContext( nullptr )
        , m_pImGuiWindowContext( nullptr )
        , m_DescriptorPool( VK_NULL_HANDLE )
        , m_RenderPass( VK_NULL_HANDLE )
        , m_RenderArea( {} )
        , m_ImageFormat( VK_FORMAT_UNDEFINED )
        , m_Images()
        , m_ImageViews()
        , m_Framebuffers()
        , m_CommandPool( VK_NULL_HANDLE )
        , m_CommandBuffers()
        , m_CommandFences()
        , m_CommandSemaphores()
        , m_VendorMetricProperties()
        , m_TimestampPeriod( 0 )
        , m_FrameBrowserSortMode( FrameBrowserSortMode::eSubmissionOrder )
        , m_HistogramGroupMode( HistogramGroupMode::eRenderPass )
        , m_Pause( false )
        , m_ShowDebugLabels( true )
    {
    }

    /***********************************************************************************\

    Function:
        Initialize

    Description:
        Initializes profiler overlay.

    \***********************************************************************************/
    VkResult ProfilerOverlayOutput::Initialize(
        VkDevice_Object& device,
        VkQueue_Object& graphicsQueue,
        VkSwapchainKhr_Object& swapchain,
        const VkSwapchainCreateInfoKHR* pCreateInfo )
    {
        VkResult result = VK_SUCCESS;

        // Setup objects
        m_pDevice = &device;
        m_pGraphicsQueue = &graphicsQueue;
        m_pSwapchain = &swapchain;

        // Create descriptor pool
        if( result == VK_SUCCESS )
        {
            VkDescriptorPoolCreateInfo descriptorPoolCreateInfo = {};
            descriptorPoolCreateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;

            // TODO: Is this necessary?
            const VkDescriptorPoolSize descriptorPoolSizes[] = {
                { VK_DESCRIPTOR_TYPE_SAMPLER, 1000 },
                { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000 },
                { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1000 },
                { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1000 },
                { VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1000 },
                { VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1000 },
                { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1000 },
                { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1000 },
                { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1000 },
                { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1000 },
                { VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1000 } };

            descriptorPoolCreateInfo.maxSets = 1000;
            descriptorPoolCreateInfo.poolSizeCount = std::extent_v<decltype(descriptorPoolSizes)>;
            descriptorPoolCreateInfo.pPoolSizes = descriptorPoolSizes;

            result = m_pDevice->Callbacks.CreateDescriptorPool(
                m_pDevice->Handle,
                &descriptorPoolCreateInfo,
                nullptr,
                &m_DescriptorPool );
        }

        // Create command pool
        if( result == VK_SUCCESS )
        {
            VkCommandPoolCreateInfo info = {};
            info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            info.flags |= VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            info.queueFamilyIndex = m_pGraphicsQueue->Family;

            result = m_pDevice->Callbacks.CreateCommandPool(
                m_pDevice->Handle,
                &info,
                nullptr,
                &m_CommandPool );
        }

        // Get timestamp query period
        if( result == VK_SUCCESS )
        {
            m_TimestampPeriod = Nanoseconds( m_pDevice->Properties.limits.timestampPeriod );
        }

        // Create swapchain-dependent resources
        if( result == VK_SUCCESS )
        {
            result = ResetSwapchain( swapchain, pCreateInfo );
        }

        // Init ImGui
        if( result == VK_SUCCESS )
        {
            std::scoped_lock lk( s_ImGuiMutex );
            IMGUI_CHECKVERSION();

            m_pImGuiContext = ImGui::CreateContext();

            ImGui::SetCurrentContext( m_pImGuiContext );
            ImGui::StyleColorsDark();

            ImGuiIO& io = ImGui::GetIO();
            io.DisplaySize = { (float)m_RenderArea.width, (float)m_RenderArea.height };
            io.DeltaTime = 1.0f / 60.0f;
            io.IniFilename = "VK_LAYER_profiler_imgui.ini";
            io.ConfigFlags = ImGuiConfigFlags_None;

            InitializeImGuiDefaultFont();
        }

        // Init window
        if( result == VK_SUCCESS )
        {
            result = InitializeImGuiWindowHooks( pCreateInfo );
        }

        // Init vulkan
        if( result == VK_SUCCESS )
        {
            result = InitializeImGuiVulkanContext( pCreateInfo );
        }

        // Get vendor metric properties
        if( result == VK_SUCCESS )
        {
            uint32_t vendorMetricCount = 0;
            vkEnumerateProfilerPerformanceCounterPropertiesEXT( device.Handle, &vendorMetricCount, nullptr );

            m_VendorMetricProperties.resize( vendorMetricCount );
            vkEnumerateProfilerPerformanceCounterPropertiesEXT( device.Handle, &vendorMetricCount, m_VendorMetricProperties.data() );
        }

        // Initialize serializer
        if( result == VK_SUCCESS )
        {
            result = (m_pStringSerializer = new (std::nothrow) DeviceProfilerStringSerializer( device ))
                ? VK_SUCCESS
                : VK_ERROR_OUT_OF_HOST_MEMORY;
        }

        // Don't leave object in partly-initialized state if something went wrong
        if( result != VK_SUCCESS )
        {
            Destroy();
        }

        return result;
    }

    /***********************************************************************************\

    Function:
        Destroy

    Description:
        Destructor.

    \***********************************************************************************/
    void ProfilerOverlayOutput::Destroy()
    {
        if( m_pDevice )
        {
            m_pDevice->Callbacks.DeviceWaitIdle( m_pDevice->Handle );
        }

        if( m_pStringSerializer )
        {
            delete m_pStringSerializer;
            m_pStringSerializer = nullptr;
        }

        if( m_pImGuiVulkanContext )
        {
            delete m_pImGuiVulkanContext;
            m_pImGuiVulkanContext = nullptr;
        }

        if( m_pImGuiWindowContext )
        {
            delete m_pImGuiWindowContext;
            m_pImGuiWindowContext = nullptr;
        }

        if( m_pImGuiContext )
        {
            ImGui::DestroyContext( m_pImGuiContext );
            m_pImGuiContext = nullptr;
        }

        if( m_DescriptorPool )
        {
            m_pDevice->Callbacks.DestroyDescriptorPool( m_pDevice->Handle, m_DescriptorPool, nullptr );
            m_DescriptorPool = VK_NULL_HANDLE;
        }

        if( m_RenderPass )
        {
            m_pDevice->Callbacks.DestroyRenderPass( m_pDevice->Handle, m_RenderPass, nullptr );
            m_RenderPass = VK_NULL_HANDLE;
        }

        if( m_CommandPool )
        {
            m_pDevice->Callbacks.DestroyCommandPool( m_pDevice->Handle, m_CommandPool, nullptr );
            m_CommandPool = VK_NULL_HANDLE;
        }

        m_CommandBuffers.clear();

        for( auto& framebuffer : m_Framebuffers )
        {
            m_pDevice->Callbacks.DestroyFramebuffer( m_pDevice->Handle, framebuffer, nullptr );
        }

        m_Framebuffers.clear();

        for( auto& imageView : m_ImageViews )
        {
            m_pDevice->Callbacks.DestroyImageView( m_pDevice->Handle, imageView, nullptr );
        }

        m_ImageViews.clear();

        for( auto& fence : m_CommandFences )
        {
            m_pDevice->Callbacks.DestroyFence( m_pDevice->Handle, fence, nullptr );
        }

        m_CommandFences.clear();

        for( auto& semaphore : m_CommandSemaphores )
        {
            m_pDevice->Callbacks.DestroySemaphore( m_pDevice->Handle, semaphore, nullptr );
        }

        m_CommandSemaphores.clear();

        m_Window = OSWindowHandle();
        m_pDevice = nullptr;
    }

    /***********************************************************************************\

    Function:
        IsAvailable

    Description:
        Check if profiler overlay is ready for presenting.

    \***********************************************************************************/
    bool ProfilerOverlayOutput::IsAvailable() const
    {
        #ifndef _DEBUG
        // There are many other objects that could be checked here, but we're keeping
        // object quite consistent in case of any errors during initialization, so
        // checking just one should be sufficient.
        return (m_pSwapchain);
        #else
        // Check object state to confirm the note above
        return (m_pSwapchain)
            && (m_pDevice)
            && (m_pGraphicsQueue)
            && (m_pImGuiContext)
            && (m_pImGuiVulkanContext)
            && (m_pImGuiWindowContext)
            && (m_RenderPass)
            && (!m_CommandBuffers.empty());
        #endif
    }

    /***********************************************************************************\

    Function:
        GetSwapchain

    Description:
        Return swapchain the overlay is associated with.

    \***********************************************************************************/
    VkSwapchainKHR ProfilerOverlayOutput::GetSwapchain() const
    {
        return m_pSwapchain->Handle;
    }

    /***********************************************************************************\

    Function:
        ResetSwapchain

    Description:
        Move overlay to the new swapchain.

    \***********************************************************************************/
    VkResult ProfilerOverlayOutput::ResetSwapchain(
        VkSwapchainKhr_Object& swapchain,
        const VkSwapchainCreateInfoKHR* pCreateInfo )
    {
        assert( m_pSwapchain == nullptr ||
            pCreateInfo->oldSwapchain == m_pSwapchain->Handle ||
            pCreateInfo->oldSwapchain == VK_NULL_HANDLE );

        VkResult result = VK_SUCCESS;

        // Get swapchain images
        uint32_t swapchainImageCount = 0;
        m_pDevice->Callbacks.GetSwapchainImagesKHR(
            m_pDevice->Handle,
            swapchain.Handle,
            &swapchainImageCount,
            nullptr );

        std::vector<VkImage> images( swapchainImageCount );
        result = m_pDevice->Callbacks.GetSwapchainImagesKHR(
            m_pDevice->Handle,
            swapchain.Handle,
            &swapchainImageCount,
            images.data() );

        assert( result == VK_SUCCESS );

        // Recreate render pass if swapchain format has changed
        if( (result == VK_SUCCESS) && (pCreateInfo->imageFormat != m_ImageFormat) )
        {
            if( m_RenderPass != VK_NULL_HANDLE )
            {
                // Destroy old render pass
                m_pDevice->Callbacks.DestroyRenderPass( m_pDevice->Handle, m_RenderPass, nullptr );
            }

            VkAttachmentDescription attachment = {};
            attachment.format = pCreateInfo->imageFormat;
            attachment.samples = VK_SAMPLE_COUNT_1_BIT;
            attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            attachment.initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

            VkAttachmentReference color_attachment = {};
            color_attachment.attachment = 0;
            color_attachment.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

            VkSubpassDescription subpass = {};
            subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
            subpass.colorAttachmentCount = 1;
            subpass.pColorAttachments = &color_attachment;

            VkSubpassDependency dependency = {};
            dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
            dependency.dstSubpass = 0;
            dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            dependency.srcAccessMask = 0;
            dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

            VkRenderPassCreateInfo info = {};
            info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
            info.attachmentCount = 1;
            info.pAttachments = &attachment;
            info.subpassCount = 1;
            info.pSubpasses = &subpass;
            info.dependencyCount = 1;
            info.pDependencies = &dependency;

            result = m_pDevice->Callbacks.CreateRenderPass(
                m_pDevice->Handle,
                &info,
                nullptr,
                &m_RenderPass );

            m_ImageFormat = pCreateInfo->imageFormat;
        }

        // Recreate image views and framebuffers
        // This is required because swapchain images have changed and current framebuffer is out of date
        if( result == VK_SUCCESS )
        {
            if( !m_Images.empty() )
            {
                // Destroy previous framebuffers
                for( int i = 0; i < m_Images.size(); ++i )
                {
                    m_pDevice->Callbacks.DestroyFramebuffer( m_pDevice->Handle, m_Framebuffers[ i ], nullptr );
                    m_pDevice->Callbacks.DestroyImageView( m_pDevice->Handle, m_ImageViews[ i ], nullptr );
                }

                m_Framebuffers.clear();
                m_ImageViews.clear();
            }

            for( uint32_t i = 0; i < swapchainImageCount; i++ )
            {
                VkImageView imageView = VK_NULL_HANDLE;
                VkFramebuffer framebuffer = VK_NULL_HANDLE;

                // Create swapchain image view
                if( result == VK_SUCCESS )
                {
                    VkImageViewCreateInfo info = {};
                    info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
                    info.viewType = VK_IMAGE_VIEW_TYPE_2D;
                    info.format = pCreateInfo->imageFormat;
                    info.image = images[ i ];
                    info.components.r = VK_COMPONENT_SWIZZLE_R;
                    info.components.g = VK_COMPONENT_SWIZZLE_G;
                    info.components.b = VK_COMPONENT_SWIZZLE_B;
                    info.components.a = VK_COMPONENT_SWIZZLE_A;

                    VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                    info.subresourceRange = range;

                    result = m_pDevice->Callbacks.CreateImageView(
                        m_pDevice->Handle,
                        &info,
                        nullptr,
                        &imageView );

                    m_ImageViews.push_back( imageView );
                }

                // Create framebuffer
                if( result == VK_SUCCESS )
                {
                    VkFramebufferCreateInfo info = {};
                    info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
                    info.renderPass = m_RenderPass;
                    info.attachmentCount = 1;
                    info.pAttachments = &imageView;
                    info.width = pCreateInfo->imageExtent.width;
                    info.height = pCreateInfo->imageExtent.height;
                    info.layers = 1;

                    result = m_pDevice->Callbacks.CreateFramebuffer(
                        m_pDevice->Handle,
                        &info,
                        nullptr,
                        &framebuffer );

                    m_Framebuffers.push_back( framebuffer );
                }
            }

            m_RenderArea = pCreateInfo->imageExtent;
        }

        // Allocate additional command buffers, fences and semaphores
        if( (result == VK_SUCCESS) && (swapchainImageCount > m_Images.size()) )
        {
            VkCommandBufferAllocateInfo allocInfo = {};
            allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocInfo.commandPool = m_CommandPool;
            allocInfo.commandBufferCount = swapchainImageCount - m_Images.size();

            std::vector<VkCommandBuffer> commandBuffers( swapchainImageCount );

            result = m_pDevice->Callbacks.AllocateCommandBuffers(
                m_pDevice->Handle,
                &allocInfo,
                commandBuffers.data() );

            if( result == VK_SUCCESS )
            {
                // Append created command buffers to end
                // We need to do this right after allocation to avoid leaks if something fails later
                m_CommandBuffers.insert( m_CommandBuffers.end(), commandBuffers.begin(), commandBuffers.end() );
            }

            for( auto cmdBuffer : commandBuffers )
            {
                if( result == VK_SUCCESS )
                {
                    // Command buffers are dispatchable handles, update pointers to parent's dispatch table
                    result = m_pDevice->SetDeviceLoaderData( m_pDevice->Handle, cmdBuffer );
                }
            }

            // Create additional per-command-buffer semaphores and fences
            for( int i = m_Images.size(); i < swapchainImageCount; ++i )
            {
                VkFence fence;
                VkSemaphore semaphore;

                // Create command buffer fence
                if( result == VK_SUCCESS )
                {
                    VkFenceCreateInfo fenceInfo = {};
                    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
                    fenceInfo.flags |= VK_FENCE_CREATE_SIGNALED_BIT;

                    result = m_pDevice->Callbacks.CreateFence(
                        m_pDevice->Handle,
                        &fenceInfo,
                        nullptr,
                        &fence );

                    m_CommandFences.push_back( fence );
                }

                // Create present semaphore
                if( result == VK_SUCCESS )
                {
                    VkSemaphoreCreateInfo semaphoreInfo = {};
                    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

                    result = m_pDevice->Callbacks.CreateSemaphore(
                        m_pDevice->Handle,
                        &semaphoreInfo,
                        nullptr,
                        &semaphore );

                    m_CommandSemaphores.push_back( semaphore );
                }
            }
        }
        
        // Update objects
        if( result == VK_SUCCESS )
        {
            m_pSwapchain = &swapchain;
            m_Images = images;
        }

        // Reinitialize ImGui
        if( (m_pImGuiContext) )
        {
            if( result == VK_SUCCESS )
            {
                // Reinit window
                result = InitializeImGuiWindowHooks( pCreateInfo );
            }

            if( result == VK_SUCCESS )
            {
                // Init vulkan
                result = InitializeImGuiVulkanContext( pCreateInfo );
            }
        }

        // Don't leave object in partly-initialized state
        if( result != VK_SUCCESS )
        {
            Destroy();
        }

        return result;
    }

    /***********************************************************************************\

    Function:
        Present

    Description:
        Draw profiler overlay before presenting the image to screen.

    \***********************************************************************************/
    void ProfilerOverlayOutput::Present(
        const DeviceProfilerFrameData& data,
        const VkQueue_Object& queue,
        VkPresentInfoKHR* pPresentInfo )
    {
        // Record interface draw commands
        Update( data );

        if( ImGui::GetDrawData() )
        {
            // Grab command buffer for overlay commands
            const uint32_t imageIndex = pPresentInfo->pImageIndices[ 0 ];

            // Per-
            VkFence& fence = m_CommandFences[ imageIndex ];
            VkSemaphore& semaphore = m_CommandSemaphores[ imageIndex ];
            VkCommandBuffer& commandBuffer = m_CommandBuffers[ imageIndex ];
            VkFramebuffer& framebuffer = m_Framebuffers[ imageIndex ];

            m_pDevice->Callbacks.WaitForFences( m_pDevice->Handle, 1, &fence, VK_TRUE, UINT64_MAX );
            m_pDevice->Callbacks.ResetFences( m_pDevice->Handle, 1, &fence );

            {
                VkCommandBufferBeginInfo info = {};
                info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                info.flags |= VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                m_pDevice->Callbacks.BeginCommandBuffer( commandBuffer, &info );
            }
            {
                VkRenderPassBeginInfo info = {};
                info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
                info.renderPass = m_RenderPass;
                info.framebuffer = framebuffer;
                info.renderArea.extent.width = m_RenderArea.width;
                info.renderArea.extent.height = m_RenderArea.height;
                m_pDevice->Callbacks.CmdBeginRenderPass( commandBuffer, &info, VK_SUBPASS_CONTENTS_INLINE );
            }

            // Record Imgui Draw Data and draw funcs into command buffer
            m_pImGuiVulkanContext->RenderDrawData( ImGui::GetDrawData(), commandBuffer );

            // Submit command buffer
            m_pDevice->Callbacks.CmdEndRenderPass( commandBuffer );

            {
                VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
                VkSubmitInfo info = {};
                info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                info.waitSemaphoreCount = pPresentInfo->waitSemaphoreCount;
                info.pWaitSemaphores = pPresentInfo->pWaitSemaphores;
                info.pWaitDstStageMask = &wait_stage;
                info.commandBufferCount = 1;
                info.pCommandBuffers = &commandBuffer;
                info.signalSemaphoreCount = 1;
                info.pSignalSemaphores = &semaphore;

                m_pDevice->Callbacks.EndCommandBuffer( commandBuffer );
                m_pDevice->Callbacks.QueueSubmit( m_pGraphicsQueue->Handle, 1, &info, fence );
            }

            // Override wait semaphore
            pPresentInfo->waitSemaphoreCount = 1;
            pPresentInfo->pWaitSemaphores = &semaphore;
        }
    }

    /***********************************************************************************\

    Function:
        Update

    Description:
        Update overlay.

    \***********************************************************************************/
    void ProfilerOverlayOutput::Update( const DeviceProfilerFrameData& data )
    {
        std::scoped_lock lk( s_ImGuiMutex );
        ImGui::SetCurrentContext( m_pImGuiContext );

        m_pImGuiVulkanContext->NewFrame();

        m_pImGuiWindowContext->NewFrame();

        ImGui::NewFrame();
        ImGui::Begin( Lang::WindowName );

        // Update input clipping rect
        m_pImGuiWindowContext->UpdateWindowRect();

        // GPU properties
        ImGui::Text( "%s: %s", Lang::Device, m_pDevice->Properties.deviceName );

        ImGuiX::TextAlignRight( "Vulkan %u.%u",
            VK_VERSION_MAJOR( m_pDevice->pInstance->ApplicationInfo.apiVersion ),
            VK_VERSION_MINOR( m_pDevice->pInstance->ApplicationInfo.apiVersion ) );

        // Save results to file
        if( ImGui::Button( Lang::Save ) )
        {
            DeviceProfilerTraceSerializer serializer(
                m_pStringSerializer,
                m_TimestampPeriod );

            serializer.Serialize( data );
        }

        // Keep results
        ImGui::SameLine();
        ImGui::Checkbox( Lang::Pause, &m_Pause );

        if( !m_Pause )
        {
            // Update data
            m_Data = data;
        }

        ImGui::BeginTabBar( "" );

        if( ImGui::BeginTabItem( Lang::Performance ) )
        {
            UpdatePerformanceTab();
            ImGui::EndTabItem();
        }
        if( ImGui::BeginTabItem( Lang::Memory ) )
        {
            UpdateMemoryTab();
            ImGui::EndTabItem();
        }
        if( ImGui::BeginTabItem( Lang::Statistics ) )
        {
            UpdateStatisticsTab();
            ImGui::EndTabItem();
        }
        if( ImGui::BeginTabItem( Lang::Settings ) )
        {
            UpdateSettingsTab();
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();

        ImGui::End();
        ImGui::Render();
    }

    /***********************************************************************************\

    Function:
        InitializeImGuiWindowHooks

    Description:

    \***********************************************************************************/
    VkResult ProfilerOverlayOutput::InitializeImGuiWindowHooks( const VkSwapchainCreateInfoKHR* pCreateInfo )
    {
        VkResult result = VK_SUCCESS;

        // Get window handle from the swapchain surface
        OSWindowHandle window = m_pDevice->pInstance->Surfaces.at( pCreateInfo->surface ).Window;

        if( m_Window == window )
        {
            // No need to update window hooks
            return result;
        }

        // Free current window
        delete m_pImGuiWindowContext;

        try
        {
            #ifdef VK_USE_PLATFORM_WIN32_KHR
            if( window.Type == OSWindowHandleType::eWin32 )
            {
                m_pImGuiWindowContext = new ImGui_ImplWin32_Context( window.Win32Handle );
            }
            #endif // VK_USE_PLATFORM_WIN32_KHR

            #ifdef VK_USE_PLATFORM_WAYLAND_KHR
            if( window.Type == OSWindowHandleType::eWayland )
            {
                m_pImGuiWindowContext = new ImGui_ImplWayland_Context( window.WaylandHandle );
            }
            #endif // VK_USE_PLATFORM_WAYLAND_KHR

            #ifdef VK_USE_PLATFORM_XCB_KHR
            if( window.Type == OSWindowHandleType::eXcb )
            {
                m_pImGuiWindowContext = new ImGui_ImplXcb_Context( window.XcbHandle );
            }
            #endif // VK_USE_PLATFORM_XCB_KHR

            #ifdef VK_USE_PLATFORM_XLIB_KHR
            if( window.Type == OSWindowHandleType::eXlib )
            {
                m_pImGuiWindowContext = new ImGui_ImplXlib_Context( window.XlibHandle );
            }
            #endif // VK_USE_PLATFORM_XLIB_KHR
        }
        catch( ... )
        {
            // Catch exceptions thrown by OS-specific ImGui window constructors
            result = VK_ERROR_INITIALIZATION_FAILED;
        }

        // Deinitialize context if something failed
        if( result != VK_SUCCESS )
        {
            delete m_pImGuiWindowContext;
            m_pImGuiWindowContext = nullptr;
        }

        // Update objects
        m_Window = window;

        return result;
    }

    /***********************************************************************************\

    Function:
        InitializeImGuiDefaultFont

    Description:

    \***********************************************************************************/
    void ProfilerOverlayOutput::InitializeImGuiDefaultFont()
    {
        ImGuiIO& io = ImGui::GetIO();

        // Absolute path to the selected font
        std::filesystem::path fontPath;

        #ifdef WIN32
        {
            // Locate system fonts directory
            std::filesystem::path fontsPath;

            PWSTR pFontsDirectoryPath = nullptr;

            if( SUCCEEDED( SHGetKnownFolderPath( FOLDERID_Fonts, KF_FLAG_DEFAULT, nullptr, &pFontsDirectoryPath ) ) )
            {
                fontsPath = pFontsDirectoryPath;
                CoTaskMemFree( pFontsDirectoryPath );
            }

            // List of fonts to use (in this order)
            const char* fonts[] = {
                "segoeui.ttf",
                "tahoma.ttf" };

            for( const char* font : fonts )
            {
                fontPath = fontsPath / font;
                if( std::filesystem::exists( fontPath ) )
                    break;
                else fontPath = "";
            }
        }
        #endif
        #ifdef __linux__
        {
            // Linux distros use multiple font directories (or X server, TODO)
            std::vector<std::filesystem::path> fontDirectories = {
                "/usr/share/fonts",
                "/usr/local/share/fonts",
                "~/.fonts" };

            // Some systems may have these directories specified in conf file
            // https://stackoverflow.com/questions/3954223/platform-independent-way-to-get-font-directory
            const char* fontConfigurationFiles[] = {
                "/etc/fonts/fonts.conf",
                "/etc/fonts/local.conf" };

            std::vector<std::filesystem::path> configurationDirectories = {};

            for( const char* fontConfigurationFile : fontConfigurationFiles )
            {
                if( std::filesystem::exists( fontConfigurationFile ) )
                {
                    // Try to open configuration file for reading
                    std::ifstream conf( fontConfigurationFile );

                    if( conf.is_open() )
                    {
                        std::string line;

                        // conf is XML file, read line by line and find <dir> tag
                        while( std::getline( conf, line ) )
                        {
                            const size_t dirTagOpen = line.find( "<dir>" );
                            const size_t dirTagClose = line.find( "</dir>" );

                            // TODO: tags can be in different lines
                            if( (dirTagOpen != std::string::npos) && (dirTagClose != std::string::npos) )
                            {
                                configurationDirectories.push_back( line.substr( dirTagOpen + 5, dirTagClose - dirTagOpen - 5 ) );
                            }
                        }
                    }
                }
            }

            if( !configurationDirectories.empty() )
            {
                // Override predefined font directories
                fontDirectories = configurationDirectories;
            }

            // List of fonts to use (in this order)
            const char* fonts[] = {
                "Ubuntu-R.ttf",
                "LiberationSans-Regural.ttf",
                "DejaVuSans.ttf" };

            for( const char* font : fonts )
            {
                for( const std::filesystem::path& fontDirectory : fontDirectories )
                {
                    fontPath = ProfilerPlatformFunctions::FindFile( fontDirectory, font );
                    if( !fontPath.empty() )
                        break;
                }
                if( !fontPath.empty() )
                    break;
            }
        }
        #endif

        if( !fontPath.empty() )
        {
            // Include all glyphs in the font to support non-latin letters
            const ImWchar range[] = { 0x20, 0xFFFF, 0 };

            io.Fonts->AddFontFromFileTTF( fontPath.string().c_str(), 16.f, nullptr, range );
        }

        // Build atlas
        unsigned char* tex_pixels = NULL;
        int tex_w, tex_h;
        io.Fonts->GetTexDataAsRGBA32( &tex_pixels, &tex_w, &tex_h );
    }

    /***********************************************************************************\

    Function:
        InitializeImGuiVulkanContext

    Description:

    \***********************************************************************************/
    VkResult ProfilerOverlayOutput::InitializeImGuiVulkanContext( const VkSwapchainCreateInfoKHR* pCreateInfo )
    {
        VkResult result = VK_SUCCESS;

        // Free current context
        delete m_pImGuiVulkanContext;

        try
        {
            ImGui_ImplVulkan_InitInfo imGuiInitInfo;
            std::memset( &imGuiInitInfo, 0, sizeof( imGuiInitInfo ) );

            imGuiInitInfo.Queue = m_pGraphicsQueue->Handle;
            imGuiInitInfo.QueueFamily = m_pGraphicsQueue->Family;

            imGuiInitInfo.Instance = m_pDevice->pInstance->Handle;
            imGuiInitInfo.PhysicalDevice = m_pDevice->PhysicalDevice;
            imGuiInitInfo.Device = m_pDevice->Handle;

            imGuiInitInfo.pInstanceDispatchTable = &m_pDevice->pInstance->Callbacks;
            imGuiInitInfo.pDispatchTable = &m_pDevice->Callbacks;

            imGuiInitInfo.Allocator = nullptr;
            imGuiInitInfo.PipelineCache = VK_NULL_HANDLE;
            imGuiInitInfo.CheckVkResultFn = nullptr;

            imGuiInitInfo.MinImageCount = pCreateInfo->minImageCount;
            imGuiInitInfo.ImageCount = m_Images.size();
            imGuiInitInfo.MSAASamples = VK_SAMPLE_COUNT_1_BIT;

            imGuiInitInfo.DescriptorPool = m_DescriptorPool;

            m_pImGuiVulkanContext = new ImGui_ImplVulkan_Context( &imGuiInitInfo, m_RenderPass );
        }
        catch( ... )
        {
            // Catch all exceptions thrown by the context constructor and return VkResult
            result = VK_ERROR_INITIALIZATION_FAILED;
        }

        // Initialize fonts
        if( result == VK_SUCCESS )
        {
            result = m_pDevice->Callbacks.ResetFences( m_pDevice->Handle, 1, &m_CommandFences[ 0 ] );
        }

        if( result == VK_SUCCESS )
        {
            VkCommandBufferBeginInfo info = {};
            info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            info.flags |= VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

            result = m_pDevice->Callbacks.BeginCommandBuffer( m_CommandBuffers[ 0 ], &info );
        }

        if( result == VK_SUCCESS )
        {
            m_pImGuiVulkanContext->CreateFontsTexture( m_CommandBuffers[ 0 ] );
        }

        if( result == VK_SUCCESS )
        {
            result = m_pDevice->Callbacks.EndCommandBuffer( m_CommandBuffers[ 0 ] );
        }

        // Submit initialization work
        if( result == VK_SUCCESS )
        {
            VkSubmitInfo info = {};
            info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            info.commandBufferCount = 1;
            info.pCommandBuffers = &m_CommandBuffers[ 0 ];

            result = m_pDevice->Callbacks.QueueSubmit( m_pGraphicsQueue->Handle, 1, &info, m_CommandFences[ 0 ] );
        }

        // Deinitialize context if something failed
        if( result != VK_SUCCESS )
        {
            delete m_pImGuiVulkanContext;
            m_pImGuiVulkanContext = nullptr;
        }

        return result;
    }

    /***********************************************************************************\

    Function:
        UpdatePerformanceTab

    Description:
        Updates "Performance" tab.

    \***********************************************************************************/
    void ProfilerOverlayOutput::UpdatePerformanceTab()
    {
        // Header
        {
            const Milliseconds gpuTimeMs = m_Data.m_Ticks * m_TimestampPeriod;
            const Milliseconds cpuTimeMs = m_Data.m_CPU.m_EndTimestamp - m_Data.m_CPU.m_BeginTimestamp;

            ImGui::Text( "%s: %.2f ms", Lang::GPUTime, gpuTimeMs.count() );
            ImGui::Text( "%s: %.2f ms", Lang::CPUTime, cpuTimeMs.count() );
            ImGuiX::TextAlignRight( "%.1f %s", m_Data.m_CPU.m_FramesPerSec, Lang::FPS );
        }

        // Histogram
        {
            std::vector<float> contributions;

            static const char* groupOptions[] = {
                Lang::RenderPasses,
                Lang::Pipelines,
                Lang::Drawcalls };

            const char* selectedOption = groupOptions[ (size_t)m_HistogramGroupMode ];

            // Select group mode
            {
                if( ImGui::BeginCombo( Lang::HistogramGroups, selectedOption, ImGuiComboFlags_NoPreview ) )
                {
                    for( size_t i = 0; i < std::extent_v<decltype(groupOptions)>; ++i )
                    {
                        bool isSelected = (selectedOption == groupOptions[ i ]);

                        if( ImGui::Selectable( groupOptions[ i ], isSelected ) )
                        {
                            // Selection changed
                            selectedOption = groupOptions[ i ];
                            isSelected = true;

                            m_HistogramGroupMode = HistogramGroupMode( i );
                        }

                        if( isSelected )
                        {
                            ImGui::SetItemDefaultFocus();
                        }
                    }

                    ImGui::EndCombo();
                }
            }

            if( m_Data.m_Ticks > 0 )
            {
                // Enumerate submits batches in frame
                for( const auto& submitBatch : m_Data.m_Submits )
                {
                    // Enumerate submits in submit batch
                    for( const auto& submit : submitBatch.m_Submits )
                    {
                        // Enumerate command buffers in submit
                        for( const auto& cmdBuffer : submit.m_CommandBuffers )
                        {
                            // Enumerate render passes in command buffer
                            for( const auto& renderPass : cmdBuffer.m_RenderPasses )
                            {
                                if( m_HistogramGroupMode > HistogramGroupMode::eRenderPass )
                                {
                                    // Enumerate subpasses in render pass
                                    for( const auto& subpass : renderPass.m_Subpasses )
                                    {
                                        if( subpass.m_Contents == VK_SUBPASS_CONTENTS_INLINE )
                                        {
                                            // Enumerate pipelines in subpass
                                            for( const auto& pipeline : subpass.m_Pipelines )
                                            {
                                                if( m_HistogramGroupMode > HistogramGroupMode::ePipeline )
                                                {
                                                    // Enumerate drawcalls in pipeline
                                                    for( const auto& drawcall : pipeline.m_Drawcalls )
                                                    {
                                                        // Insert drawcall cycle count to histogram
                                                        contributions.push_back( drawcall.m_EndTimestamp - drawcall.m_BeginTimestamp );
                                                    }
                                                }
                                                else
                                                {
                                                    // Insert pipeline cycle count to histogram
                                                    contributions.push_back( pipeline.m_EndTimestamp - pipeline.m_BeginTimestamp );
                                                }
                                            }
                                        }
                                        else if( subpass.m_Contents == VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS )
                                        {
                                            // TODO
                                        }
                                    }
                                }
                                else
                                {
                                    // Insert render pass cycle count to histogram
                                    contributions.push_back( renderPass.m_EndTimestamp - renderPass.m_BeginTimestamp );
                                }
                            }
                        }
                    }
                }
            }

            char pHistogramDescription[ 32 ];
            snprintf( pHistogramDescription, sizeof( pHistogramDescription ),
                "%s (%s)",
                Lang::GPUCycles,
                selectedOption );

            ImGui::PushItemWidth( -1 );
            ImGuiX::PlotHistogramEx(
                "",
                contributions.data(), // Scale x with y
                contributions.data(),
                contributions.size(),
                0, pHistogramDescription, 0, FLT_MAX, { 0, 100 } );
        }

        // Top pipelines
        if( ImGui::CollapsingHeader( Lang::TopPipelines ) )
        {
            uint32_t i = 0;

            for( const auto& pipeline : m_Data.m_TopPipelines )
            {
                if( pipeline.m_Handle != VK_NULL_HANDLE )
                {
                    const uint64_t pipelineTicks = (pipeline.m_EndTimestamp - pipeline.m_BeginTimestamp);

                    ImGui::Text( "%2u. %s", i + 1, m_pStringSerializer->GetName( pipeline ).c_str() );
                    ImGuiX::TextAlignRight( "(%.1f %%) %.2f ms",
                        pipelineTicks * 100.f / m_Data.m_Ticks, 
                        pipelineTicks * m_TimestampPeriod.count() );

                    // Print up to 10 top pipelines
                    if( (++i) == 10 ) break;
                }
            }
        }

        // Vendor-specific
        if( !m_Data.m_VendorMetrics.empty() &&
            ImGui::CollapsingHeader( Lang::PerformanceCounters ) )
        {
            assert( m_Data.m_VendorMetrics.size() == m_VendorMetricProperties.size() );

            ImGui::BeginTable( "Performance counters table",
                /* columns_count */ 3,
                ImGuiTableFlags_Resizable |
                ImGuiTableFlags_NoClipX |
                ImGuiTableFlags_Borders );

            // Headers
            ImGui::TableSetupColumn( Lang::Metric, ImGuiTableColumnFlags_WidthAlwaysAutoResize );
            ImGui::TableSetupColumn( Lang::Frame, ImGuiTableColumnFlags_WidthStretch );
            ImGui::TableSetupColumn( "", ImGuiTableColumnFlags_WidthAlwaysAutoResize );
            ImGui::TableAutoHeaders();

            for( uint32_t i = 0; i < m_Data.m_VendorMetrics.size(); ++i )
            {
                const VkProfilerPerformanceCounterResultEXT& metric = m_Data.m_VendorMetrics[ i ];
                const VkProfilerPerformanceCounterPropertiesEXT& metricProperties = m_VendorMetricProperties[ i ];

                ImGui::TableNextCell();
                {
                    ImGui::Text( "%s", metricProperties.shortName );

                    if( ImGui::IsItemHovered() &&
                        metricProperties.description[ 0 ] )
                    {
                        ImGui::BeginTooltip();
                        ImGui::PushTextWrapPos( 350.f );
                        ImGui::TextUnformatted( metricProperties.description );
                        ImGui::PopTextWrapPos();
                        ImGui::EndTooltip();
                    }
                }

                ImGui::TableNextCell();
                {
                    const float columnWidth = ImGuiX::TableGetColumnWidth();
                    switch( metricProperties.storage )
                    {
                    case VK_PERFORMANCE_COUNTER_STORAGE_FLOAT32_KHR:
                        ImGuiX::TextAlignRight( columnWidth, "%.2f", metric.float32 );
                        break;

                    case VK_PERFORMANCE_COUNTER_STORAGE_UINT32_KHR:
                        ImGuiX::TextAlignRight( columnWidth, "%u", metric.uint32 );
                        break;

                    case VK_PERFORMANCE_COUNTER_STORAGE_UINT64_KHR:
                        ImGuiX::TextAlignRight( columnWidth, "%llu", metric.uint64 );
                        break;
                    }
                }

                ImGui::TableNextCell();
                {
                    const char* pUnitString = "???";

                    assert( metricProperties.unit < 11 );
                    static const char* const ppUnitString[ 11 ] =
                    {
                        "" /* VK_PERFORMANCE_COUNTER_UNIT_GENERIC_KHR */,
                        "%" /* VK_PERFORMANCE_COUNTER_UNIT_PERCENTAGE_KHR */,
                        "ns" /* VK_PERFORMANCE_COUNTER_UNIT_NANOSECONDS_KHR */,
                        "B" /* VK_PERFORMANCE_COUNTER_UNIT_BYTES_KHR */,
                        "B/s" /* VK_PERFORMANCE_COUNTER_UNIT_BYTES_PER_SECOND_KHR */,
                        "K" /* VK_PERFORMANCE_COUNTER_UNIT_KELVIN_KHR */,
                        "W" /* VK_PERFORMANCE_COUNTER_UNIT_WATTS_KHR */,
                        "V" /* VK_PERFORMANCE_COUNTER_UNIT_VOLTS_KHR */,
                        "A" /* VK_PERFORMANCE_COUNTER_UNIT_AMPS_KHR */,
                        "Hz" /* VK_PERFORMANCE_COUNTER_UNIT_HERTZ_KHR */,
                        "clk" /* VK_PERFORMANCE_COUNTER_UNIT_CYCLES_KHR */
                    };

                    if( metricProperties.unit < 11 )
                    {
                        pUnitString = ppUnitString[ metricProperties.unit ];
                    }

                    ImGui::TextUnformatted( pUnitString );
                }
            }

            ImGui::EndTable();
        }

        // Frame browser
        if( ImGui::CollapsingHeader( Lang::FrameBrowser ) )
        {
            // Select sort mode
            {
                static const char* sortOptions[] = {
                    Lang::SubmissionOrder,
                    Lang::DurationDescending,
                    Lang::DurationAscending };

                const char* selectedOption = sortOptions[ (size_t)m_FrameBrowserSortMode ];

                ImGui::Text( Lang::Sort );
                ImGui::SameLine();

                if( ImGui::BeginCombo( "FrameBrowserSortMode", selectedOption ) )
                {
                    for( size_t i = 0; i < std::extent_v<decltype(sortOptions)>; ++i )
                    {
                        bool isSelected = (selectedOption == sortOptions[ i ]);

                        if( ImGui::Selectable( sortOptions[ i ], isSelected ) )
                        {
                            // Selection changed
                            selectedOption = sortOptions[ i ];
                            isSelected = true;

                            m_FrameBrowserSortMode = FrameBrowserSortMode( i );
                        }

                        if( isSelected )
                        {
                            ImGui::SetItemDefaultFocus();
                        }
                    }

                    ImGui::EndCombo();
                }
            }

            FrameBrowserTreeNodeIndex index = {};

            // Enumerate submits in frame
            for( const auto& submitBatch : m_Data.m_Submits )
            {
                const std::string queueName = m_pStringSerializer->GetName( submitBatch.m_Handle );

                index.SubmitIndex = 0;
                index.PrimaryCommandBufferIndex = 0;

                char indexStr[ 30 ] = {};
                structtohex( indexStr, index );

                if( ImGui::TreeNode( indexStr, "vkQueueSubmit(%s, %u)",
                    queueName.c_str(),
                    static_cast<uint32_t>(submitBatch.m_Submits.size()) ) )
                {
                    for( const auto& submit : submitBatch.m_Submits )
                    {
                        structtohex( indexStr, index );

                        const bool inSubmitSubtree =
                            (submitBatch.m_Submits.size() > 1) &&
                            (ImGui::TreeNode( indexStr, "VkSubmitInfo #%u", index.SubmitIndex ));

                        if( (inSubmitSubtree) || (submitBatch.m_Submits.size() == 1) )
                        {
                            // Sort frame browser data
                            std::list<const DeviceProfilerCommandBufferData*> pCommandBuffers =
                                SortFrameBrowserData( submit.m_CommandBuffers );

                            // Enumerate command buffers in submit
                            for( const auto* pCommandBuffer : pCommandBuffers )
                            {
                                PrintCommandBuffer( *pCommandBuffer, index );
                                index.PrimaryCommandBufferIndex++;
                            }
                        }

                        if( inSubmitSubtree )
                        {
                            // Finish submit subtree
                            ImGui::TreePop();
                        }

                        index.SubmitIndex++;
                    }

                    // Finish submit batch subtree
                    ImGui::TreePop();
                }

                index.SubmitBatchIndex++;
            }
        }
    }

    /***********************************************************************************\

    Function:
        UpdateMemoryTab

    Description:
        Updates "Memory" tab.

    \***********************************************************************************/
    void ProfilerOverlayOutput::UpdateMemoryTab()
    {
        const VkPhysicalDeviceMemoryProperties& memoryProperties =
            m_pDevice->MemoryProperties;

        if( ImGui::CollapsingHeader( Lang::MemoryHeapUsage ) )
        {
            for( uint32_t i = 0; i < memoryProperties.memoryHeapCount; ++i )
            {
                ImGui::Text( "%s %u", Lang::MemoryHeap, i );

                ImGuiX::TextAlignRight( "%u %s", m_Data.m_Memory.m_Heaps[ i ].m_AllocationCount, Lang::Allocations );

                float usage = 0.f;
                char usageStr[ 64 ] = {};

                if( memoryProperties.memoryHeaps[ i ].size != 0 )
                {
                    usage = (float)m_Data.m_Memory.m_Heaps[ i ].m_AllocationSize / memoryProperties.memoryHeaps[ i ].size;

                    snprintf( usageStr, sizeof( usageStr ),
                        "%.2f/%.2f MB (%.1f%%)",
                        m_Data.m_Memory.m_Heaps[ i ].m_AllocationSize / 1048576.f,
                        memoryProperties.memoryHeaps[ i ].size / 1048576.f,
                        usage * 100.f );
                }

                ImGui::ProgressBar( usage, { -1, 0 }, usageStr );

                if( ImGui::IsItemHovered() && (memoryProperties.memoryHeaps[ i ].flags != 0) )
                {
                    ImGui::BeginTooltip();

                    if( memoryProperties.memoryHeaps[ i ].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT )
                    {
                        ImGui::TextUnformatted( "VK_MEMORY_HEAP_DEVICE_LOCAL_BIT" );
                    }

                    if( memoryProperties.memoryHeaps[ i ].flags & VK_MEMORY_HEAP_MULTI_INSTANCE_BIT )
                    {
                        ImGui::TextUnformatted( "VK_MEMORY_HEAP_MULTI_INSTANCE_BIT" );
                    }

                    ImGui::EndTooltip();
                }

                std::vector<float> memoryTypeUsages( memoryProperties.memoryTypeCount );
                std::vector<std::string> memoryTypeDescriptors( memoryProperties.memoryTypeCount );

                for( uint32_t typeIndex = 0; typeIndex < memoryProperties.memoryTypeCount; ++typeIndex )
                {
                    if( memoryProperties.memoryTypes[ typeIndex ].heapIndex == i )
                    {
                        memoryTypeUsages[ typeIndex ] = m_Data.m_Memory.m_Types[ typeIndex ].m_AllocationSize;

                        // Prepare descriptor for memory type
                        std::stringstream sstr;

                        sstr << Lang::MemoryTypeIndex << " " << typeIndex << "\n"
                            << m_Data.m_Memory.m_Types[ typeIndex ].m_AllocationCount << " " << Lang::Allocations << "\n";

                        if( memoryProperties.memoryTypes[ typeIndex ].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT )
                        {
                            sstr << "VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT\n";
                        }

                        if( memoryProperties.memoryTypes[ typeIndex ].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD )
                        {
                            sstr << "VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD\n";
                        }

                        if( memoryProperties.memoryTypes[ typeIndex ].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_UNCACHED_BIT_AMD )
                        {
                            sstr << "VK_MEMORY_PROPERTY_DEVICE_UNCACHED_BIT_AMD\n";
                        }

                        if( memoryProperties.memoryTypes[ typeIndex ].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT )
                        {
                            sstr << "VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT\n";
                        }

                        if( memoryProperties.memoryTypes[ typeIndex ].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT )
                        {
                            sstr << "VK_MEMORY_PROPERTY_HOST_COHERENT_BIT\n";
                        }

                        if( memoryProperties.memoryTypes[ typeIndex ].propertyFlags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT )
                        {
                            sstr << "VK_MEMORY_PROPERTY_HOST_CACHED_BIT\n";
                        }

                        if( memoryProperties.memoryTypes[ typeIndex ].propertyFlags & VK_MEMORY_PROPERTY_PROTECTED_BIT )
                        {
                            sstr << "VK_MEMORY_PROPERTY_PROTECTED_BIT\n";
                        }

                        if( memoryProperties.memoryTypes[ typeIndex ].propertyFlags & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT )
                        {
                            sstr << "VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT\n";
                        }

                        memoryTypeDescriptors[ typeIndex ] = sstr.str();
                    }
                }

                // Get descriptor pointers
                std::vector<const char*> memoryTypeDescriptorPointers( memoryProperties.memoryTypeCount );

                for( uint32_t typeIndex = 0; typeIndex < memoryProperties.memoryTypeCount; ++typeIndex )
                {
                    memoryTypeDescriptorPointers[ typeIndex ] = memoryTypeDescriptors[ typeIndex ].c_str();
                }

                ImGuiX::PlotBreakdownEx(
                    "HEAP_BREAKDOWN",
                    memoryTypeUsages.data(),
                    memoryProperties.memoryTypeCount, 0,
                    memoryTypeDescriptorPointers.data() );
            }
        }
    }

    /***********************************************************************************\

    Function:
        UpdateStatisticsTab

    Description:
        Updates "Statistics" tab.

    \***********************************************************************************/
    void ProfilerOverlayOutput::UpdateStatisticsTab()
    {
        // Draw count statistics
        {
            ImGui::TextUnformatted( Lang::DrawCalls );
            ImGuiX::TextAlignRight( "%u", m_Data.m_Stats.m_DrawCount );

            ImGui::TextUnformatted( Lang::DrawCallsIndirect );
            ImGuiX::TextAlignRight( "%u", m_Data.m_Stats.m_DispatchIndirectCount );

            ImGui::TextUnformatted( Lang::DispatchCalls );
            ImGuiX::TextAlignRight( "%u", m_Data.m_Stats.m_DispatchCount );

            ImGui::TextUnformatted( Lang::DispatchCallsIndirect );
            ImGuiX::TextAlignRight( "%u", m_Data.m_Stats.m_DispatchIndirectCount );

            ImGui::TextUnformatted( Lang::CopyBufferCalls );
            ImGuiX::TextAlignRight( "%u", m_Data.m_Stats.m_CopyBufferCount );

            ImGui::TextUnformatted( Lang::CopyBufferToImageCalls );
            ImGuiX::TextAlignRight( "%u", m_Data.m_Stats.m_CopyBufferToImageCount );

            ImGui::TextUnformatted( Lang::CopyImageCalls );
            ImGuiX::TextAlignRight( "%u", m_Data.m_Stats.m_CopyImageCount );

            ImGui::TextUnformatted( Lang::CopyImageToBufferCalls );
            ImGuiX::TextAlignRight( "%u", m_Data.m_Stats.m_CopyImageToBufferCount );

            ImGui::TextUnformatted( Lang::PipelineBarriers );
            ImGuiX::TextAlignRight( "%u", m_Data.m_Stats.m_PipelineBarrierCount );

            ImGui::TextUnformatted( Lang::ColorClearCalls );
            ImGuiX::TextAlignRight( "%u", m_Data.m_Stats.m_ClearColorCount );

            ImGui::TextUnformatted( Lang::DepthStencilClearCalls );
            ImGuiX::TextAlignRight( "%u", m_Data.m_Stats.m_ClearDepthStencilCount );

            ImGui::TextUnformatted( Lang::ResolveCalls );
            ImGuiX::TextAlignRight( "%u", m_Data.m_Stats.m_ResolveCount );

            ImGui::TextUnformatted( Lang::BlitCalls );
            ImGuiX::TextAlignRight( "%u", m_Data.m_Stats.m_BlitImageCount );

            ImGui::TextUnformatted( Lang::FillBufferCalls );
            ImGuiX::TextAlignRight( "%u", m_Data.m_Stats.m_FillBufferCount );

            ImGui::TextUnformatted( Lang::UpdateBufferCalls );
            ImGuiX::TextAlignRight( "%u", m_Data.m_Stats.m_UpdateBufferCount );
        }
    }

    /***********************************************************************************\

    Function:
        UpdateSettingsTab

    Description:
        Updates "Settings" tab.

    \***********************************************************************************/
    void ProfilerOverlayOutput::UpdateSettingsTab()
    {
        // Select synchronization mode
        {
            static const char* groupOptions[] = {
                Lang::Present,
                Lang::Submit };

            // TMP
            static int selectedOption = 0;
            int previousSelectedOption = selectedOption;

            ImGui::Combo( Lang::SyncMode, &selectedOption, groupOptions, 2 );

            if( selectedOption != previousSelectedOption )
            {
                vkSetProfilerSyncModeEXT( m_pDevice->Handle, (VkProfilerSyncModeEXT)selectedOption );
            }

            ImGui::Checkbox( Lang::ShowDebugLabels, &m_ShowDebugLabels );
        }
    }

    /***********************************************************************************\

    Function:
        PrintCommandBuffer

    Description:
        Writes command buffer data to the overlay.

    \***********************************************************************************/
    void ProfilerOverlayOutput::PrintCommandBuffer( const DeviceProfilerCommandBufferData& cmdBuffer, FrameBrowserTreeNodeIndex index )
    {
        const uint64_t commandBufferTicks = (cmdBuffer.m_EndTimestamp - cmdBuffer.m_BeginTimestamp);

        // Mark hotspots with color
        DrawSignificanceRect( (float)commandBufferTicks / m_Data.m_Ticks );

        const std::string cmdBufferName = m_pStringSerializer->GetName( cmdBuffer.m_Handle );

        #if 0
        // Disable empty command buffer expanding
        if( cmdBuffer.m_Subregions.empty() )
        {
            assert( cmdBuffer.m_Stats.m_TotalTicks == 0 );

            ImGui::TextUnformatted( cmdBufferName.c_str() );
            return;
        }
        #endif

        char indexStr[ 30 ] = {};
        structtohex( indexStr, index );

        if( ImGui::TreeNode( indexStr, "%s", cmdBufferName.c_str() ) )
        {
            // Command buffer opened
            ImGuiX::TextAlignRight( "%.2f ms", commandBufferTicks * m_TimestampPeriod.count() );

            // Sort frame browser data
            std::list<const DeviceProfilerRenderPassData*> pRenderPasses =
                SortFrameBrowserData( cmdBuffer.m_RenderPasses );

            // Enumerate render passes in command buffer
            for( const DeviceProfilerRenderPassData* pRenderPass : pRenderPasses )
            {
                PrintRenderPass( *pRenderPass, index );
                index.RenderPassIndex++;
            }

            ImGui::TreePop();
        }
        else
        {
            // Command buffer collapsed
            ImGuiX::TextAlignRight( "%.2f ms", commandBufferTicks * m_TimestampPeriod.count() );
        }
    }

    /***********************************************************************************\

    Function:
        PrintRenderPass

    Description:
        Writes render pass data to the overlay.

    \***********************************************************************************/
    void ProfilerOverlayOutput::PrintRenderPass( const DeviceProfilerRenderPassData& renderPass, FrameBrowserTreeNodeIndex index )
    {
        const uint64_t renderPassTicks = (renderPass.m_EndTimestamp - renderPass.m_BeginTimestamp);

        // Mark hotspots with color
        DrawSignificanceRect( (float)renderPassTicks / m_Data.m_Ticks );

        char indexStr[ 30 ] = {};
        structtohex( indexStr, index );

        // At least one subpass must be present
        assert( !renderPass.m_Subpasses.empty() );

        const bool inRenderPassSubtree =
            (renderPass.m_Handle != VK_NULL_HANDLE) &&
            (ImGui::TreeNode( indexStr, "%s",
                m_pStringSerializer->GetName( renderPass.m_Handle ).c_str() ));

        if( inRenderPassSubtree )
        {
            const uint64_t renderPassBeginTicks = (renderPass.m_Begin.m_EndTimestamp - renderPass.m_Begin.m_BeginTimestamp);

            // Render pass subtree opened
            ImGuiX::TextAlignRight( "%.2f ms", renderPassTicks * m_TimestampPeriod.count() );

            // Mark hotspots with color
            DrawSignificanceRect( (float)renderPassBeginTicks / m_Data.m_Ticks );

            // Print BeginRenderPass pipeline
            ImGui::TextUnformatted( "vkCmdBeginRenderPass" );
            ImGuiX::TextAlignRight( "%.2f ms", renderPassBeginTicks * m_TimestampPeriod.count() );
        }

        if( inRenderPassSubtree ||
            (renderPass.m_Handle == VK_NULL_HANDLE) )
        {
            // Sort frame browser data
            std::list<const DeviceProfilerSubpassData*> pSubpasses =
                SortFrameBrowserData( renderPass.m_Subpasses );

            // Enumerate subpasses
            for( const DeviceProfilerSubpassData* pSubpass : pSubpasses )
            {
                PrintSubpass( *pSubpass, index, (pSubpasses.size() == 1) );
                index.SubpassIndex++;
            }
        }

        if( inRenderPassSubtree )
        {
            const uint64_t renderPassEndTicks = (renderPass.m_End.m_EndTimestamp - renderPass.m_End.m_BeginTimestamp);

            // Mark hotspots with color
            DrawSignificanceRect( (float)renderPassEndTicks / m_Data.m_Ticks );

            // Print EndRenderPass pipeline
            ImGui::TextUnformatted( "vkCmdEndRenderPass" );
            ImGuiX::TextAlignRight( "%.2f ms", renderPassEndTicks * m_TimestampPeriod.count() );

            ImGui::TreePop();
        }

        if( !inRenderPassSubtree &&
            (renderPass.m_Handle != VK_NULL_HANDLE) )
        {
            // Render pass collapsed
            ImGuiX::TextAlignRight( "%.2f ms", renderPassTicks * m_TimestampPeriod.count() );
        }
    }

    /***********************************************************************************\

    Function:
        PrintSubpass

    Description:
        Writes subpass data to the overlay.

    \***********************************************************************************/
    void ProfilerOverlayOutput::PrintSubpass( const DeviceProfilerSubpassData& subpass, FrameBrowserTreeNodeIndex index, bool isOnlySubpass )
    {
        const uint64_t subpassTicks = (subpass.m_EndTimestamp - subpass.m_BeginTimestamp);
        bool inSubpassSubtree = false;

        if( !isOnlySubpass )
        {
            // Mark hotspots with color
            DrawSignificanceRect( (float)subpassTicks / m_Data.m_Ticks );

            char indexStr[ 30 ] = {};
            structtohex( indexStr, index );

            inSubpassSubtree =
                (subpass.m_Index != -1) &&
                (ImGui::TreeNode( indexStr, "Subpass #%u", subpass.m_Index ));
        }

        if( inSubpassSubtree )
        {
            // Subpass subtree opened
            ImGuiX::TextAlignRight( "%.2f ms", subpassTicks * m_TimestampPeriod.count() );
        }

        if( inSubpassSubtree ||
            isOnlySubpass ||
            (subpass.m_Index == -1) )
        {
            if( subpass.m_Contents == VK_SUBPASS_CONTENTS_INLINE )
            {
                // Sort frame browser data
                std::list<const DeviceProfilerPipelineData*> pPipelines =
                    SortFrameBrowserData( subpass.m_Pipelines );

                // Enumerate pipelines in subpass
                for( const DeviceProfilerPipelineData* pPipeline : pPipelines )
                {
                    PrintPipeline( *pPipeline, index );
                    index.PipelineIndex++;
                }
            }

            else if( subpass.m_Contents == VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS )
            {
                // Sort command buffers
                std::list<const DeviceProfilerCommandBufferData*> pCommandBuffers =
                    SortFrameBrowserData( subpass.m_SecondaryCommandBuffers );

                // Enumerate command buffers in subpass
                for( const DeviceProfilerCommandBufferData* pCommandBuffer : pCommandBuffers )
                {
                    PrintCommandBuffer( *pCommandBuffer, index );
                    index.SecondaryCommandBufferIndex++;
                }
            }
        }

        if( inSubpassSubtree )
        {
            // Finish subpass tree
            ImGui::TreePop();
        }

        if( !inSubpassSubtree && !isOnlySubpass && (subpass.m_Index != -1) )
        {
            // Subpass collapsed
            ImGuiX::TextAlignRight( "%.2f ms", subpassTicks * m_TimestampPeriod.count() );
        }
    }

    /***********************************************************************************\

    Function:
        PrintPipeline

    Description:
        Writes pipeline data to the overlay.

    \***********************************************************************************/
    void ProfilerOverlayOutput::PrintPipeline( const DeviceProfilerPipelineData& pipeline, FrameBrowserTreeNodeIndex index )
    {
        const uint64_t pipelineTicks = (pipeline.m_EndTimestamp - pipeline.m_BeginTimestamp);

        const bool printPipelineInline =
            (pipeline.m_Handle == VK_NULL_HANDLE) ||
            ((pipeline.m_ShaderTuple.m_Hash & 0xFFFF) == 0);

        bool inPipelineSubtree = false;

        if( !printPipelineInline )
        {
            // Mark hotspots with color
            DrawSignificanceRect( (float)pipelineTicks / m_Data.m_Ticks );

            char indexStr[ 30 ] = {};
            structtohex( indexStr, index );

            inPipelineSubtree =
                (ImGui::TreeNode( indexStr, "%s", m_pStringSerializer->GetName( pipeline ).c_str() ));
        }

        if( inPipelineSubtree )
        {
            // Pipeline subtree opened
            ImGuiX::TextAlignRight( "%.2f ms", pipelineTicks * m_TimestampPeriod.count() );
        }

        if( inPipelineSubtree || printPipelineInline )
        {
            // Sort frame browser data
            std::list<const DeviceProfilerDrawcall*> pDrawcalls =
                SortFrameBrowserData( pipeline.m_Drawcalls );

            // Enumerate drawcalls in pipeline
            for( const DeviceProfilerDrawcall* pDrawcall : pDrawcalls )
            {
                PrintDrawcall( *pDrawcall );
            }
        }

        if( inPipelineSubtree )
        {
            // Finish pipeline subtree
            ImGui::TreePop();
        }

        if( !inPipelineSubtree && !printPipelineInline )
        {
            // Pipeline collapsed
            ImGuiX::TextAlignRight( "%.2f ms", pipelineTicks * m_TimestampPeriod.count() );
        }
    }

    /***********************************************************************************\

    Function:
        PrintDrawcall

    Description:
        Writes drawcall data to the overlay.

    \***********************************************************************************/
    void ProfilerOverlayOutput::PrintDrawcall( const DeviceProfilerDrawcall& drawcall )
    {
        if( drawcall.GetPipelineType() != DeviceProfilerPipelineType::eDebug )
        {
            const uint64_t drawcallTicks = (drawcall.m_EndTimestamp - drawcall.m_BeginTimestamp);

            // Mark hotspots with color
            DrawSignificanceRect( (float)drawcallTicks / m_Data.m_Ticks );

            const std::string drawcallString = m_pStringSerializer->GetName( drawcall );
            ImGui::TextUnformatted( drawcallString.c_str() );

            // Print drawcall duration
            ImGuiX::TextAlignRight( "%.2f ms", drawcallTicks * m_TimestampPeriod.count() );
        }
        else
        {
            // Draw debug label
            PrintDebugLabel( drawcall.m_Payload.m_DebugLabel.m_pName, drawcall.m_Payload.m_DebugLabel.m_Color );
        }
    }

    /***********************************************************************************\

    Function:
        DrawSignificanceRect

    Description:

    \***********************************************************************************/
    void ProfilerOverlayOutput::DrawSignificanceRect( float significance )
    {
        ImVec2 cursorPosition = ImGui::GetCursorScreenPos();
        ImVec2 rectSize;

        cursorPosition.x = ImGui::GetWindowPos().x;

        rectSize.x = cursorPosition.x + ImGui::GetWindowSize().x;
        rectSize.y = cursorPosition.y + ImGui::GetTextLineHeight();

        ImU32 color = ImGui::GetColorU32( { 1, 0, 0, significance } );

        ImDrawList* pDrawList = ImGui::GetWindowDrawList();
        pDrawList->AddRectFilled( cursorPosition, rectSize, color );
    }

    /***********************************************************************************\

    Function:
        DrawDebugLabel

    Description:

    \***********************************************************************************/
    void ProfilerOverlayOutput::PrintDebugLabel( const char* pName, const float pColor[ 4 ] )
    {
        if( !(m_ShowDebugLabels) ||
            (m_FrameBrowserSortMode != FrameBrowserSortMode::eSubmissionOrder) ||
            !(pName) )
        {
            // Don't print debug labels if frame browser is sorted out of submission order
            return;
        }

        ImVec2 cursorPosition = ImGui::GetCursorScreenPos();
        ImVec2 rectSize;

        rectSize.x = cursorPosition.x + 8;
        rectSize.y = cursorPosition.y + ImGui::GetTextLineHeight();

        // Resolve debug label color
        ImU32 color = ImGui::GetColorU32( *reinterpret_cast<const ImVec4*>(pColor) );

        ImDrawList* pDrawList = ImGui::GetWindowDrawList();
        pDrawList->AddRectFilled( cursorPosition, rectSize, color );
        pDrawList->AddRect( cursorPosition, rectSize, ImGui::GetColorU32( ImGuiCol_Border ) );

        cursorPosition.x += 12;
        ImGui::SetCursorScreenPos( cursorPosition );

        ImGui::TextUnformatted( pName );
    }
}
