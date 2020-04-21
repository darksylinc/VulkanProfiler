#include "profiler.h"
#include "profiler_helpers.h"
#include "farmhash/src/farmhash.h"
#include <sstream>
#include <fstream>

#undef max

namespace Profiler
{
    /***********************************************************************************\

    Function:
        PerformanceProfiler

    Description:
        Constructor

    \***********************************************************************************/
    Profiler::Profiler()
        : m_pDevice( nullptr )
        , m_Config()
        , m_Debug()
        , m_DataMutex()
        , m_Data()
        , m_DataAggregator()
        , m_CurrentFrame( 0 )
        , m_CpuTimestampCounter()
        , m_Allocations()
        , m_DeviceLocalAllocatedMemorySize( 0 )
        , m_DeviceLocalAllocationCount( 0 )
        , m_HostVisibleAllocatedMemorySize( 0 )
        , m_HostVisibleAllocationCount( 0 )
        , m_ProfiledCommandBuffers()
        , m_TimestampPeriod( 0.0f )
    {
    }

    /***********************************************************************************\

    Function:
        Initialize

    Description:
        Initializes profiler resources.

    \***********************************************************************************/
    VkResult Profiler::Initialize( VkDevice_Object* pDevice )
    {
        m_pDevice = pDevice;
        m_CurrentFrame = 0;

        VkResult result;

        // Load config
        std::filesystem::path customConfigPath = ProfilerPlatformFunctions::GetCustomConfigPath();

        // Check if custom configuration file exists
        if( !customConfigPath.empty() &&
            !std::filesystem::exists( customConfigPath ) )
        {
            //m_Output.WriteLine( "ERROR: Custom config file %s not found",
            //    customConfigPath.c_str() );

            customConfigPath = "";
        }

        // Look for path relative to the app
        if( !customConfigPath.is_absolute() )
        {
            customConfigPath = ProfilerPlatformFunctions::GetApplicationDir() / customConfigPath;
        }

        customConfigPath /= "VkLayer_profiler_layer.conf";

        if( std::filesystem::exists( customConfigPath ) )
        {
            std::ifstream config( customConfigPath );

            // Check if configuration file was successfully opened
            while( !config.eof() )
            {
                std::string key; config >> key;
                uint32_t value; config >> value;

                if( key == "MODE" )
                    m_Config.m_DisplayMode = static_cast<VkProfilerModeEXT>(value);

                if( key == "NUM_QUERIES_PER_CMD_BUFFER" )
                    m_Config.m_NumQueriesPerCommandBuffer = value;

                if( key == "OUTPUT_UPDATE_INTERVAL" )
                    m_Config.m_OutputUpdateInterval = static_cast<std::chrono::milliseconds>(value);

                if( key == "OUTPUT_FLAGS" )
                    m_Config.m_OutputFlags = static_cast<VkProfilerOutputFlagsEXT>(value);
            }
        }

        // TODO: Remove
        m_Config.m_OutputFlags |= VK_PROFILER_OUTPUT_FLAG_OVERLAY_BIT_EXT;

        m_Config.m_SamplingMode = m_Config.m_DisplayMode;

        // Check if preemption is enabled
        // It may break the results
        if( ProfilerPlatformFunctions::IsPreemptionEnabled() )
        {
            // Sample per drawcall to avoid DMA packet splits between timestamps
            //m_Config.m_SamplingMode = ProfilerMode::ePerDrawcall;
            #if 0
            m_Output.Summary.Message = "Preemption enabled. Profiler will collect results per drawcall.";
            m_Overlay.Summary.Message = "Preemption enabled. Results may be unstable.";
            #endif
        }

        // Create submit fence
        VkFenceCreateInfo fenceCreateInfo;
        ClearStructure( &fenceCreateInfo, VK_STRUCTURE_TYPE_FENCE_CREATE_INFO );

        result = m_pDevice->Callbacks.CreateFence(
            m_pDevice->Handle, &fenceCreateInfo, nullptr, &m_SubmitFence );

        if( result != VK_SUCCESS )
        {
            // Fence creation failed
            Destroy();
            return result;
        }

        // Get GPU timestamp period
        m_pDevice->pInstance->Callbacks.GetPhysicalDeviceProperties(
            m_pDevice->PhysicalDevice, &m_DeviceProperties );

        m_TimestampPeriod = m_DeviceProperties.limits.timestampPeriod;

        return VK_SUCCESS;
    }

    /***********************************************************************************\

    Function:
        Destroy

    Description:
        Frees resources allocated by the profiler.

    \***********************************************************************************/
    void Profiler::Destroy()
    {
        m_ProfiledCommandBuffers.clear();

        m_Allocations.clear();

        if( m_SubmitFence != VK_NULL_HANDLE )
        {
            m_pDevice->Callbacks.DestroyFence( m_pDevice->Handle, m_SubmitFence, nullptr );
            m_SubmitFence = VK_NULL_HANDLE;
        }

        m_CurrentFrame = 0;
        m_pDevice = nullptr;
    }

    /***********************************************************************************\

    \***********************************************************************************/
    VkResult Profiler::SetMode( VkProfilerModeEXT mode )
    {
        // TODO: Invalidate all command buffers
        m_Config.m_DisplayMode = mode;

        return VK_SUCCESS;
    }

    /***********************************************************************************\

    \***********************************************************************************/
    ProfilerAggregatedData Profiler::GetData() const
    {
        // Hold aggregator updates to keep m_Data consistent
        std::scoped_lock lk( m_DataMutex );
        return m_Data;
    }

    /***********************************************************************************\

    Function:
        SetDebugObjectName

    Description:
        Assign user-defined name to the object

    \***********************************************************************************/
    void Profiler::SetDebugObjectName( uint64_t objectHandle, const char* pObjectName )
    {
        m_Debug.SetDebugObjectName( objectHandle, pObjectName );
    }

    /***********************************************************************************\

    Function:
        PreDraw

    Description:

    \***********************************************************************************/
    void Profiler::PreDraw( VkCommandBuffer commandBuffer )
    {
        // ProfilerCommandBuffer object should already be in the map
        auto& profilerCommandBuffer = m_ProfiledCommandBuffers.interlocked_at( commandBuffer );

        profilerCommandBuffer.PreDraw();
    }

    /***********************************************************************************\

    Function:
        PostDraw

    Description:

    \***********************************************************************************/
    void Profiler::PostDraw( VkCommandBuffer commandBuffer )
    {
        // ProfilerCommandBuffer object should already be in the map
        auto& profilerCommandBuffer = m_ProfiledCommandBuffers.interlocked_at( commandBuffer );

        profilerCommandBuffer.PostDraw();
    }

    /***********************************************************************************\

    Function:
        PreDrawIndirect

    Description:

    \***********************************************************************************/
    void Profiler::PreDrawIndirect( VkCommandBuffer commandBuffer )
    {
        // ProfilerCommandBuffer object should already be in the map
        auto& profilerCommandBuffer = m_ProfiledCommandBuffers.interlocked_at( commandBuffer );

        profilerCommandBuffer.PreDrawIndirect();
    }

    /***********************************************************************************\

    Function:
        PostDrawIndirect

    Description:

    \***********************************************************************************/
    void Profiler::PostDrawIndirect( VkCommandBuffer commandBuffer )
    {
        // ProfilerCommandBuffer object should already be in the map
        auto& profilerCommandBuffer = m_ProfiledCommandBuffers.interlocked_at( commandBuffer );

        profilerCommandBuffer.PostDrawIndirect();
    }

    /***********************************************************************************\

    Function:
        PreDispatch

    Description:

    \***********************************************************************************/
    void Profiler::PreDispatch( VkCommandBuffer commandBuffer )
    {
        // ProfilerCommandBuffer object should already be in the map
        auto& profilerCommandBuffer = m_ProfiledCommandBuffers.interlocked_at( commandBuffer );

        profilerCommandBuffer.PreDispatch();
    }

    /***********************************************************************************\

    Function:
        PostDispatch

    Description:

    \***********************************************************************************/
    void Profiler::PostDispatch( VkCommandBuffer commandBuffer )
    {
        // ProfilerCommandBuffer object should already be in the map
        auto& profilerCommandBuffer = m_ProfiledCommandBuffers.interlocked_at( commandBuffer );

        profilerCommandBuffer.PostDispatch();
    }

    /***********************************************************************************\

    Function:
        PreDispatchIndirect

    Description:

    \***********************************************************************************/
    void Profiler::PreDispatchIndirect( VkCommandBuffer commandBuffer )
    {
        // ProfilerCommandBuffer object should already be in the map
        auto& profilerCommandBuffer = m_ProfiledCommandBuffers.interlocked_at( commandBuffer );

        profilerCommandBuffer.PreDispatchIndirect();
    }

    /***********************************************************************************\

    Function:
        PostDispatchIndirect

    Description:

    \***********************************************************************************/
    void Profiler::PostDispatchIndirect( VkCommandBuffer commandBuffer )
    {
        // ProfilerCommandBuffer object should already be in the map
        auto& profilerCommandBuffer = m_ProfiledCommandBuffers.interlocked_at( commandBuffer );

        profilerCommandBuffer.PostDispatchIndirect();
    }

    /***********************************************************************************\

    Function:
        PreCopy

    Description:

    \***********************************************************************************/
    void Profiler::PreCopy( VkCommandBuffer commandBuffer )
    {
        // ProfilerCommandBuffer object should already be in the map
        auto& profilerCommandBuffer = m_ProfiledCommandBuffers.interlocked_at( commandBuffer );

        profilerCommandBuffer.PreCopy();
    }

    /***********************************************************************************\

    Function:
        PostCopy

    Description:

    \***********************************************************************************/
    void Profiler::PostCopy( VkCommandBuffer commandBuffer )
    {
        // ProfilerCommandBuffer object should already be in the map
        auto& profilerCommandBuffer = m_ProfiledCommandBuffers.interlocked_at( commandBuffer );

        profilerCommandBuffer.PostCopy();
    }

    /***********************************************************************************\

    Function:
        PreClear

    Description:

    \***********************************************************************************/
    void Profiler::PreClear( VkCommandBuffer commandBuffer )
    {
        // ProfilerCommandBuffer object should already be in the map
        auto& profiledCommandBuffer = m_ProfiledCommandBuffers.interlocked_at( commandBuffer );

        profiledCommandBuffer.PreClear();
    }

    /***********************************************************************************\

    Function:
        PostClear

    Description:

    \***********************************************************************************/
    void Profiler::PostClear( VkCommandBuffer commandBuffer, uint32_t attachmentCount )
    {
        // ProfilerCommandBuffer object should already be in the map
        auto& profiledCommandBuffer = m_ProfiledCommandBuffers.interlocked_at( commandBuffer );

        profiledCommandBuffer.PostClear( attachmentCount );
    }

    /***********************************************************************************\

    Function:
        PipelineBarrier

    Description:
        Collects barrier statistics.

    \***********************************************************************************/
    void Profiler::OnPipelineBarrier( VkCommandBuffer commandBuffer,
        uint32_t memoryBarrierCount, const VkMemoryBarrier* pMemoryBarriers,
        uint32_t bufferMemoryBarrierCount, const VkBufferMemoryBarrier* pBufferMemoryBarriers,
        uint32_t imageMemoryBarrierCount, const VkImageMemoryBarrier* pImageMemoryBarriers )
    {
        // ProfilerCommandBuffer object should already be in the map
        auto& profiledCommandBuffer = m_ProfiledCommandBuffers.interlocked_at( commandBuffer );

        profiledCommandBuffer.OnPipelineBarrier(
            memoryBarrierCount, pMemoryBarriers,
            bufferMemoryBarrierCount, pBufferMemoryBarriers,
            imageMemoryBarrierCount, pImageMemoryBarriers );

        // Transitions from undefined layout are slower than from other layouts
        // BUT are required in some cases (e.g., texture is used for the first time)
        for( uint32_t i = 0; i < imageMemoryBarrierCount; ++i )
        {
            const VkImageMemoryBarrier& barrier = pImageMemoryBarriers[ i ];

            if( barrier.oldLayout == VK_IMAGE_LAYOUT_UNDEFINED )
            {
                // TODO: Message
            }
        }
    }

    /***********************************************************************************\

    Function:
        CreatePipelines

    Description:

    \***********************************************************************************/
    void Profiler::CreatePipelines( uint32_t pipelineCount, const VkGraphicsPipelineCreateInfo* pCreateInfos, VkPipeline* pPipelines )
    {
        for( uint32_t i = 0; i < pipelineCount; ++i )
        {
            ProfilerPipeline profilerPipeline;
            profilerPipeline.m_Handle = pPipelines[i];
            profilerPipeline.m_ShaderTuple = CreateShaderTuple( pCreateInfos[i] );

            std::stringstream stringBuilder;
            stringBuilder
                << "VS=" << std::hex << std::setfill( '0' ) << std::setw( 8 )
                << profilerPipeline.m_ShaderTuple.m_Vert
                << ",PS=" << std::hex << std::setfill( '0' ) << std::setw( 8 )
                << profilerPipeline.m_ShaderTuple.m_Frag;

            m_Debug.SetDebugObjectName((uint64_t)profilerPipeline.m_Handle,
                stringBuilder.str().c_str() );

            m_ProfiledPipelines.interlocked_emplace( pPipelines[i], profilerPipeline );
        }
    }

    /***********************************************************************************\

    Function:
        DestroyPipeline

    Description:

    \***********************************************************************************/
    void Profiler::DestroyPipeline( VkPipeline pipeline )
    {
        m_ProfiledPipelines.interlocked_erase( pipeline );
    }

    /***********************************************************************************\

    Function:
        BindPipeline

    Description:

    \***********************************************************************************/
    void Profiler::BindPipeline( VkCommandBuffer commandBuffer, VkPipeline pipeline )
    {
        // ProfilerCommandBuffer object should already be in the map
        auto& profilerCommandBuffer = m_ProfiledCommandBuffers.interlocked_at( commandBuffer );

        // ProfilerPipeline object should already be in the map
        // TMP
        auto it = m_ProfiledPipelines.find( pipeline );
        if( it != m_ProfiledPipelines.end() )
        {
            profilerCommandBuffer.BindPipeline( it->second );
        }
    }

    /***********************************************************************************\

    Function:
        CreateShaderModule

    Description:

    \***********************************************************************************/
    void Profiler::CreateShaderModule( VkShaderModule module, const VkShaderModuleCreateInfo* pCreateInfo )
    {
        // Compute shader code hash to use later
        const uint32_t hash = Hash::Fingerprint32( reinterpret_cast<const char*>(pCreateInfo->pCode), pCreateInfo->codeSize );

        m_ProfiledShaderModules.interlocked_emplace( module, hash );
    }

    /***********************************************************************************\

    Function:
        DestroyShaderModule

    Description:

    \***********************************************************************************/
    void Profiler::DestroyShaderModule( VkShaderModule module )
    {
        m_ProfiledShaderModules.interlocked_erase( module );
    }

    /***********************************************************************************\

    Function:
        BeginRenderPass

    Description:

    \***********************************************************************************/
    void Profiler::BeginRenderPass( VkCommandBuffer commandBuffer, const VkRenderPassBeginInfo* pBeginInfo )
    {
        // ProfilerCommandBuffer object should already be in the map
        auto& profilerCommandBuffer = m_ProfiledCommandBuffers.interlocked_at( commandBuffer );

        profilerCommandBuffer.BeginRenderPass( pBeginInfo );
    }

    /***********************************************************************************\

    Function:
        EndRenderPass

    Description:

    \***********************************************************************************/
    void Profiler::EndRenderPass( VkCommandBuffer commandBuffer )
    {
        // ProfilerCommandBuffer object should already be in the map
        auto& profilerCommandBuffer = m_ProfiledCommandBuffers.interlocked_at( commandBuffer );

        profilerCommandBuffer.EndRenderPass();
    }

    /***********************************************************************************\

    Function:
        BeginCommandBuffer

    Description:

    \***********************************************************************************/
    void Profiler::BeginCommandBuffer( VkCommandBuffer commandBuffer, const VkCommandBufferBeginInfo* pBeginInfo )
    {
        auto emplaced = m_ProfiledCommandBuffers.interlocked_try_emplace( commandBuffer,
            std::ref( *this ), commandBuffer );

        // Grab reference to the ProfilerCommandBuffer instance
        auto& profilerCommandBuffer = emplaced.first->second;

        // Prepare the command buffer for the next profiling run
        profilerCommandBuffer.Begin( pBeginInfo );
    }

    /***********************************************************************************\

    Function:
        EndCommandBuffer

    Description:

    \***********************************************************************************/
    void Profiler::EndCommandBuffer( VkCommandBuffer commandBuffer )
    {
        // ProfilerCommandBuffer object should already be in the map
        auto& profilerCommandBuffer = m_ProfiledCommandBuffers.interlocked_at( commandBuffer );

        // Prepare the command buffer for the next profiling run
        profilerCommandBuffer.End();
    }

    /***********************************************************************************\

    Function:
        FreeCommandBuffers

    Description:

    \***********************************************************************************/
    void Profiler::FreeCommandBuffers( uint32_t commandBufferCount, const VkCommandBuffer* pCommandBuffers )
    {
        // Block access from other threads
        std::scoped_lock lk( m_ProfiledCommandBuffers );

        for( uint32_t i = 0; i < commandBufferCount; ++i )
        {
            m_ProfiledCommandBuffers.erase( pCommandBuffers[i] );
        }
    }

    /***********************************************************************************\

    Function:
        PreSubmitCommandBuffers

    Description:

    \***********************************************************************************/

    /***********************************************************************************\

    Function:
        PostSubmitCommandBuffers

    Description:

    \***********************************************************************************/
    void Profiler::PostSubmitCommandBuffers( VkQueue queue, uint32_t count, const VkSubmitInfo* pSubmitInfo, VkFence fence )
    {
        // Wait for the submitted command buffers to execute
        if( m_Config.m_SamplingMode < VK_PROFILER_MODE_PER_FRAME_EXT )
        {
            m_pDevice->Callbacks.QueueSubmit( queue, 0, nullptr, m_SubmitFence );
            m_pDevice->Callbacks.WaitForFences( m_pDevice->Handle, 1, &m_SubmitFence, true, std::numeric_limits<uint64_t>::max() );
        }

        // Store submitted command buffers and get results
        for( uint32_t submitIdx = 0; submitIdx < count; ++submitIdx )
        {
            const VkSubmitInfo& submitInfo = pSubmitInfo[submitIdx];

            // Wrap submit info into our structure
            ProfilerSubmitData submit;

            for( uint32_t commandBufferIdx = 0; commandBufferIdx < submitInfo.commandBufferCount; ++commandBufferIdx )
            {
                // Get command buffer handle
                VkCommandBuffer commandBuffer = submitInfo.pCommandBuffers[commandBufferIdx];

                // Block access from other threads
                std::scoped_lock lk( m_ProfiledCommandBuffers );

                auto& profilerCommandBuffer = m_ProfiledCommandBuffers.at( commandBuffer );

                // Dirty command buffer profiling data
                profilerCommandBuffer.Submit();

                submit.m_CommandBuffers.push_back( profilerCommandBuffer.GetData() );
            }

            // Store the submit wrapper
            m_DataAggregator.AppendData( submit );
        }
    }

    /***********************************************************************************\

    Function:
        PostPresent

    Description:

    \***********************************************************************************/
    void Profiler::Present( const VkQueue_Object& queue, VkPresentInfoKHR* pPresentInfo )
    {
        m_CurrentFrame++;

        m_CpuTimestampCounter.End();

        // TMP
        std::scoped_lock lk( m_DataMutex );
        m_Data = m_DataAggregator.GetAggregatedData();

        // TODO: Move to memory tracker
        m_Data.m_Memory.m_TotalAllocationCount = m_TotalAllocationCount;
        m_Data.m_Memory.m_TotalAllocationSize = m_TotalAllocatedMemorySize;
        m_Data.m_Memory.m_DeviceLocalAllocationSize = m_DeviceLocalAllocatedMemorySize;
        m_Data.m_Memory.m_HostVisibleAllocationSize = m_HostVisibleAllocatedMemorySize;

        // TODO: Move to memory tracker
        ClearStructure( &m_MemoryProperties2, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2 );
        m_pDevice->pInstance->Callbacks.GetPhysicalDeviceMemoryProperties2KHR(
            m_pDevice->PhysicalDevice,
            &m_MemoryProperties2 );

        if( m_CpuTimestampCounter.GetValue<std::chrono::milliseconds>() > m_Config.m_OutputUpdateInterval )
        {
            // Process data (TODO: Move to profiler_ext)
            #if 0
            m_DataStaging.memory.deviceLocalMemoryAllocated = data.m_Memory.m_DeviceLocalAllocationSize;

            FreeProfilerData( &m_DataStaging.frame );

            m_DataStaging.frame.regionType = VK_PROFILER_REGION_TYPE_FRAME_EXT;
            m_DataStaging.frame.pRegionName = CreateRegionName( "Frame #", m_CurrentFrame );
            
            // Frame stats
            FillProfilerData( &m_DataStaging.frame, data.m_Stats );

            m_DataStaging.frame.subregionCount = data.m_Submits.size();
            m_DataStaging.frame.pSubregions = new VkProfilerRegionDataEXT[ data.m_Submits.size() ];

            uint32_t submitIdx = 0;
            VkProfilerRegionDataEXT* pSubmitRegion = m_DataStaging.frame.pSubregions;

            for( const auto& submit : data.m_Submits )
            {
                pSubmitRegion->regionType = VK_PROFILER_REGION_TYPE_SUBMIT_EXT;
                pSubmitRegion->regionObject = VK_NULL_HANDLE;
                pSubmitRegion->pRegionName = CreateRegionName( "Submit #", submitIdx );

                // Submit stats (TODO)

                pSubmitRegion->subregionCount = submit.m_CommandBuffers.size();
                pSubmitRegion->pSubregions = new VkProfilerRegionDataEXT[ submit.m_CommandBuffers.size() ];

                VkProfilerRegionDataEXT* pCmdBufferRegion = pSubmitRegion->pSubregions;

                for( const auto& cmdBuffer : submit.m_CommandBuffers )
                {
                    pCmdBufferRegion
                }

                // Move to next submit region
                submitIdx++;
                pSubmitRegion++;
            }
            #endif

            m_CpuTimestampCounter.Begin();
        }

        m_LastFrameBeginTimestamp = m_Data.m_Stats.m_BeginTimestamp;

        // TMP
        m_DataAggregator.Reset();
    }

    /***********************************************************************************\

    Function:
        Destroy

    Description:

    \***********************************************************************************/
    void Profiler::OnAllocateMemory( VkDeviceMemory allocatedMemory, const VkMemoryAllocateInfo* pAllocateInfo )
    {
        // Insert allocation info to the map, it will be needed during deallocation.
        m_Allocations.emplace( allocatedMemory, *pAllocateInfo );

        const VkMemoryType& memoryType =
            m_MemoryProperties.memoryTypes[ pAllocateInfo->memoryTypeIndex ];

        if( memoryType.propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT )
        {
            m_DeviceLocalAllocationCount++;
            m_DeviceLocalAllocatedMemorySize += pAllocateInfo->allocationSize;
        }

        if( memoryType.propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT )
        {
            m_HostVisibleAllocationCount++;
            m_HostVisibleAllocatedMemorySize += pAllocateInfo->allocationSize;
        }
    }

    /***********************************************************************************\

    Function:
        Destroy

    Description:

    \***********************************************************************************/
    void Profiler::OnFreeMemory( VkDeviceMemory allocatedMemory )
    {
        auto it = m_Allocations.find( allocatedMemory );
        if( it != m_Allocations.end() )
        {
            const VkMemoryType& memoryType =
                m_MemoryProperties.memoryTypes[ it->second.memoryTypeIndex ];

            if( memoryType.propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT )
            {
                m_DeviceLocalAllocationCount--;
                m_DeviceLocalAllocatedMemorySize -= it->second.allocationSize;
            }

            if( memoryType.propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT )
            {
                m_HostVisibleAllocationCount--;
                m_HostVisibleAllocatedMemorySize -= it->second.allocationSize;
            }

            // Remove allocation entry from the map
            m_Allocations.erase( it );
        }
    }

    #if 0
    /***********************************************************************************\

    Function:
        PresentResults

    Description:

    \***********************************************************************************/
    void Profiler::PresentResults( const ProfilerAggregatedData& data )
    {
        // Summary stats
        m_Output.Summary.FPS = 1000000000. / (m_TimestampPeriod * (data.m_Stats.m_BeginTimestamp - m_LastFrameBeginTimestamp));

        if( m_Output.NextLinesVisible( 12 ) )
        {
            // Tuple stats
            int numTopTuples = 10;
            m_Output.WriteLine( " Top 10 pipelines:" );
            m_Output.WriteLine( "     Vert     Frag" );
            for( const auto& pipeline : data.m_TopPipelines )
            {
                if( numTopTuples == 0 )
                    break;

                m_Output.WriteLine( " %2d. %08x %08x  (%0.1f%%)",
                    11 - numTopTuples,
                    pipeline.m_ShaderTuple.m_Vert,
                    pipeline.m_ShaderTuple.m_Frag,
                    100.f * static_cast<float>(pipeline.m_Stats.m_TotalTicks) / static_cast<float>(data.m_Stats.m_TotalTicks) );

                numTopTuples--;
            }
            m_Output.SkipLines( numTopTuples );

            // Drawcall stats
            m_Output.WriteAt( 40, 3, "Frame statistics:" );
            m_Output.WriteAt( 40, 4, " Draw:                 %5u", data.m_Stats.m_TotalDrawCount );
            m_Output.WriteAt( 40, 5, " Draw (indirect):      %5u", data.m_Stats.m_TotalDrawIndirectCount );
            m_Output.WriteAt( 40, 6, " Dispatch:             %5u", data.m_Stats.m_TotalDispatchCount );
            m_Output.WriteAt( 40, 7, " Dispatch (indirect):  %5u", data.m_Stats.m_TotalDispatchIndirectCount );
            m_Output.WriteAt( 40, 8, " Copy:                 %5u", data.m_Stats.m_TotalCopyCount );
            m_Output.WriteAt( 40, 9, " Clear:                %5u", data.m_Stats.m_TotalClearCount );
            m_Output.WriteAt( 40, 10, " Clear (implicit):     %5u", data.m_Stats.m_TotalClearImplicitCount );
            m_Output.WriteAt( 40, 11, " Barrier:              %5u", data.m_Stats.m_TotalBarrierCount );
            m_Output.WriteAt( 40, 12, "                            " ); // clear
            m_Output.WriteAt( 40, 13, " TOTAL:                %5u",
                data.m_Stats.m_TotalDrawCount +
                data.m_Stats.m_TotalDrawIndirectCount +
                data.m_Stats.m_TotalDispatchCount +
                data.m_Stats.m_TotalDispatchIndirectCount +
                data.m_Stats.m_TotalCopyCount +
                data.m_Stats.m_TotalClearCount +
                data.m_Stats.m_TotalClearImplicitCount +
                data.m_Stats.m_TotalBarrierCount );
        }
        else
        {
            m_Output.SkipLines( 12 );
        }

        m_Output.WriteLine( "" );

        for( uint32_t submitIdx = 0; submitIdx < data.m_Submits.size(); ++submitIdx )
        {
            PresentSubmit( submitIdx, data.m_Submits.at( submitIdx ) );
        }

        m_Output.WriteLine( "" );
        m_Output.Flush();
    }

    namespace
    {
        static constexpr char fillLine[] =
            "...................................................................................................."
            "...................................................................................................."
            "...................................................................................................."
            "....................................................................................................";

    }

    /***********************************************************************************\

    Function:
        PresentResults

    Description:

    \***********************************************************************************/
    void Profiler::PresentSubmit( uint32_t submitIdx, const ProfilerSubmitData& submit )
    {
        // Calculate how many lines this submit will generate
        uint32_t requiredLineCount = 1 + submit.m_CommandBuffers.size();

        if( m_Config.m_DisplayMode <= VK_PROFILER_MODE_PER_RENDER_PASS_EXT )
        for( const auto& commandBuffer : submit.m_CommandBuffers )
        {
            // Add number of valid render passes
            for( const auto& renderPass : commandBuffer.m_Subregions )
            {
                if( renderPass.m_Handle != VK_NULL_HANDLE )
                    requiredLineCount++;

                // Add number of valid pipelines in the render pass
                for( const auto& pipeline : renderPass.m_Subregions )
                {
                    if( pipeline.m_Handle != VK_NULL_HANDLE )
                        requiredLineCount++;

                    if( m_Config.m_DisplayMode <= VK_PROFILER_MODE_PER_DRAWCALL_EXT )
                    {
                        // Add number of drawcalls in each pipeline
                        requiredLineCount += pipeline.m_Subregions.size();
                    }
                }
            }
        }

        if( !m_Output.NextLinesVisible( requiredLineCount ) )
        {
            m_Output.SkipLines( requiredLineCount );
            return;
        }

        m_Output.WriteLine( "VkSubmitInfo #%2u", submitIdx );

        for( uint32_t i = 0; i < submit.m_CommandBuffers.size(); ++i )
        {
            const auto& commandBuffer = submit.m_CommandBuffers[ i ];

            m_Output.WriteLine( " VkCommandBuffer (%s)",
                m_Debug.GetDebugObjectName( (uint64_t)commandBuffer.m_Handle ).c_str() );

            if( commandBuffer.m_Subregions.empty() )
            {
                continue;
            }

            if( m_Config.m_DisplayMode > VK_PROFILER_MODE_PER_RENDER_PASS_EXT )
            {
                continue;
            }

            uint32_t currentRenderPass = 0;

            while( currentRenderPass < commandBuffer.m_Subregions.size() )
            {
                const auto& renderPass = commandBuffer.m_Subregions[currentRenderPass];
                const uint32_t pipelineCount = renderPass.m_Subregions.size();
                const uint32_t drawcallCount = renderPass.m_Stats.m_TotalDrawcallCount;

                const uint32_t sublines =
                    (m_Config.m_DisplayMode <= VK_PROFILER_MODE_PER_PIPELINE_EXT) * pipelineCount +
                    (m_Config.m_DisplayMode <= VK_PROFILER_MODE_PER_DRAWCALL_EXT) * drawcallCount;

                if( !m_Output.NextLinesVisible( 1 + sublines ) )
                {
                    m_Output.SkipLines( 1 + pipelineCount );
                    currentRenderPass++;
                    continue;
                }

                if( renderPass.m_Handle != VK_NULL_HANDLE )
                {
                    // Compute time difference between timestamps
                    float us = (renderPass.m_Stats.m_TotalTicks * m_TimestampPeriod) / 1000.0f;

                    uint32_t lineWidth = 48 +
                        DigitCount( currentRenderPass ) +
                        DigitCount( pipelineCount );

                    std::string renderPassName =
                        m_Debug.GetDebugObjectName( (uint64_t)renderPass.m_Handle );

                    lineWidth += renderPassName.length();

                    m_Output.WriteLine( "  VkRenderPass #%u (%s) - %u pipelines %.*s %10.4f us",
                        currentRenderPass,
                        renderPassName.c_str(),
                        pipelineCount,
                        m_Output.Width() - lineWidth, fillLine,
                        us );
                }

                if( m_Config.m_DisplayMode > VK_PROFILER_MODE_PER_PIPELINE_EXT )
                {
                    currentRenderPass++;
                    continue;
                }

                uint32_t currentRenderPassPipeline = 0;

                while( currentRenderPassPipeline < pipelineCount )
                {
                    const auto& pipeline = renderPass.m_Subregions[currentRenderPassPipeline];
                    const size_t drawcallCount = pipeline.m_Subregions.size();

                    const uint32_t sublines =
                        (m_Config.m_DisplayMode <= VK_PROFILER_MODE_PER_DRAWCALL_EXT) * drawcallCount;

                    if( !m_Output.NextLinesVisible( 1 + sublines ) )
                    {
                        m_Output.SkipLines( 1 + drawcallCount );
                        currentRenderPassPipeline++;
                        continue;
                    }

                    if( pipeline.m_Handle != VK_NULL_HANDLE )
                    {
                        // Convert to microseconds
                        float us = (pipeline.m_Stats.m_TotalTicks * m_TimestampPeriod) / 1000.0f;

                        uint32_t lineWidth = 47 +
                            DigitCount( currentRenderPassPipeline ) +
                            DigitCount( drawcallCount );

                        std::string pipelineName = m_Debug.GetDebugObjectName( (uint64_t)pipeline.m_Handle );

                        lineWidth += pipelineName.length();

                        m_Output.WriteLine( "   VkPipeline #%u (%s) - %u drawcalls %.*s %10.4f us",
                            currentRenderPassPipeline,
                            pipelineName.c_str(),
                            drawcallCount,
                            m_Output.Width() - lineWidth, fillLine,
                            us );
                    }

                    if( m_Config.m_DisplayMode > VK_PROFILER_MODE_PER_DRAWCALL_EXT )
                    {
                        currentRenderPassPipeline++;
                        continue;
                    }

                    uint32_t currentPipelineDrawcall = 0;

                    while( currentPipelineDrawcall < drawcallCount )
                    {
                        const auto& drawcall = pipeline.m_Subregions[ currentPipelineDrawcall ];

                        if( !m_Output.NextLinesVisible( 1 ) )
                        {
                            m_Output.SkipLines( 1 );
                            currentPipelineDrawcall++;
                            continue;
                        }

                        // Convert to microseconds
                        float us = (drawcall.m_Ticks * m_TimestampPeriod) / 1000.0f;

                        uint32_t lineWidth = 50 +
                            DigitCount( currentPipelineDrawcall );

                        switch( drawcall.m_Type )
                        {
                        case ProfilerDrawcallType::eDraw:
                            m_Output.WriteLine( "    vkCmdDraw %.*s %8.4f us",
                                //drawcall.m_DrawArgs.vertexCount,
                                //drawcall.m_DrawArgs.instanceCount,
                                //drawcall.m_DrawArgs.firstVertex,
                                //drawcall.m_DrawArgs.firstInstance,
                                m_Output.Width() - lineWidth, fillLine, us );
                            break;

                        case ProfilerDrawcallType::eCopy:
                            m_Output.WriteLine( "    vkCmdCopy %.*s %8.4f us",
                                m_Output.Width() - lineWidth, fillLine, us );
                            break;
                        }


                        currentPipelineDrawcall++;
                    }

                    currentRenderPassPipeline++;
                }

                currentRenderPass++;
            }
        }
    }
    #endif

    /***********************************************************************************\

    Function:
        FreeProfilerData

    Description:

    \***********************************************************************************/
    void Profiler::FreeProfilerData( VkProfilerRegionDataEXT* pData ) const
    {
        for( uint32_t i = 0; i < pData->subregionCount; ++i )
        {
            FreeProfilerData( &pData->pSubregions[ i ] );
            delete[] pData->pSubregions;
        }

        std::free( const_cast<char*>(pData->pRegionName) );

        std::memset( pData, 0, sizeof( VkProfilerRegionDataEXT ) );
    }

    /***********************************************************************************\

    Function:
        FillProfilerData

    Description:

    \***********************************************************************************/
    void Profiler::FillProfilerData( VkProfilerRegionDataEXT* pData, const ProfilerRangeStats& stats ) const
    {
        pData->duration = 1000000.f * stats.m_TotalTicks / m_TimestampPeriod;
        pData->drawCount = stats.m_TotalDrawCount;
        pData->drawIndirectCount = stats.m_TotalDrawIndirectCount;
        pData->dispatchCount = stats.m_TotalDispatchCount;
        pData->dispatchIndirectCount = stats.m_TotalDispatchIndirectCount;
        pData->clearCount = stats.m_TotalClearCount + stats.m_TotalClearImplicitCount;
        pData->barrierCount = stats.m_TotalBarrierCount;
    }

    /***********************************************************************************\

    Function:
        CreateShaderTuple

    Description:

    \***********************************************************************************/
    ProfilerShaderTuple Profiler::CreateShaderTuple( const VkGraphicsPipelineCreateInfo& createInfo )
    {
        ProfilerShaderTuple tuple;

        for( uint32_t i = 0; i < createInfo.stageCount; ++i )
        {
            // VkShaderModule entry should already be in the map
            uint32_t hash = m_ProfiledShaderModules.interlocked_at( createInfo.pStages[i].module );

            const char* entrypoint = createInfo.pStages[i].pName;

            // Hash the entrypoint and append it to the final hash
            hash ^= Hash::Fingerprint32( entrypoint, std::strlen( entrypoint ) );

            switch( createInfo.pStages[i].stage )
            {
            case VK_SHADER_STAGE_VERTEX_BIT: tuple.m_Vert = hash; break;
            case VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT: tuple.m_Tesc = hash; break;
            case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT: tuple.m_Tese = hash; break;
            case VK_SHADER_STAGE_GEOMETRY_BIT: tuple.m_Geom = hash; break;
            case VK_SHADER_STAGE_FRAGMENT_BIT: tuple.m_Frag = hash; break;

            default:
            {
                // Break in debug builds
                assert( !"Usupported graphics shader stage" );
            }
            }
        }

        // Compute aggregated tuple hash for fast comparison
        tuple.m_Hash = Hash::Fingerprint32( reinterpret_cast<const char*>(&tuple), sizeof( tuple ) );

        return tuple;
    }
}
