#include "VkProfilerEXT.h"
#include "profiler_layer_functions/VkDevice_functions.h"

using namespace Profiler;

VKAPI_ATTR VkResult VKAPI_CALL vkSetProfilerModeEXT(
    VkDevice device,
    VkProfilerModeEXT mode )
{
    return VkDevice_Functions::DeviceDispatch.Get( device )
        .Profiler.SetMode( mode );
}

VKAPI_ATTR VkResult VKAPI_CALL vkSetProfilerSyncModeEXT(
    VkDevice device,
    VkProfilerSyncModeEXT syncMode )
{
    return VkDevice_Functions::DeviceDispatch.Get( device )
        .Profiler.SetSyncMode( syncMode );
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetProfilerFrameDataEXT(
    VkDevice device,
    VkProfilerRegionDataEXT* pData )
{
    auto& dd = VkDevice_Functions::DeviceDispatch.Get( device );

    // Get latest data from profiler
    ProfilerAggregatedData data = dd.Profiler.GetData();

    // Translate internal data to VkProfilerRegionDataEXT
    pData->regionType = VK_PROFILER_REGION_TYPE_FRAME_EXT;
    pData->duration = data.m_Stats.m_TotalTicks * (dd.Profiler.m_TimestampPeriod / 1000000.f);

    // Describe the frame
    sprintf( pData->regionName, "Frame #%u", dd.Profiler.m_CurrentFrame );

    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetProfilerCommandBufferDataEXT(
    VkDevice device,
    VkCommandBuffer commandBuffer,
    VkProfilerRegionDataEXT* pData )
{
    auto& dd = VkDevice_Functions::DeviceDispatch.Get( device );

    // Get latest data from profiler
    ProfilerAggregatedData data = dd.Profiler.GetData();

    uint64_t commandBufferTotalTicks = 0;

    // Find command buffer
    for( const auto& submitData : data.m_Submits )
    {
        for( const auto& commandBufferData : submitData.m_CommandBuffers )
        {
            // Aggregate command buffer data
            if( commandBufferData.m_Handle == commandBuffer )
            {
                commandBufferTotalTicks += commandBufferData.m_Stats.m_TotalTicks;
            }
        }
    }

    // Translate internal data to VkProfilerRegionDataEXT
    pData->regionType = VK_PROFILER_REGION_TYPE_COMMAND_BUFFER_EXT;
    pData->duration = commandBufferTotalTicks * (dd.Profiler.m_TimestampPeriod / 1000000.f);

    // Describe the command buffer
    auto it = dd.Device.Debug.ObjectNames.find( (uint64_t)commandBuffer );
    if( it != dd.Device.Debug.ObjectNames.end() )
    {
        strcpy( pData->regionName, it->second.c_str() );
    }
    else
    {
        sprintf( pData->regionName, "VkCommandBuffer at 0x%p", commandBuffer );
    }

    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateProfilerMetricPropertiesEXT(
    VkDevice device,
    uint32_t* pProfilerMetricCount,
    VkProfilerMetricPropertiesEXT* pProfilerMetricProperties )
{
    auto& dd = VkDevice_Functions::DeviceDispatch.Get( device );

    bool hasSufficientSpace = true;

    if( dd.Profiler.m_MetricsApiINTEL.IsAvailable() )
    {
        if( (*pProfilerMetricCount) == 0 )
        {
            // Return number of reported metrics
            (*pProfilerMetricCount) += dd.Profiler.m_MetricsApiINTEL.GetMetricsCount();
        }
        else
        {
            // Get reported metrics descriptions
            const std::vector<VkProfilerMetricPropertiesEXT> intelMetricsProperties =
                dd.Profiler.m_MetricsApiINTEL.GetMetricsProperties();

            const uint32_t returnedMetricPropertyCount =
                std::template min<uint32_t>( (*pProfilerMetricCount), intelMetricsProperties.size() );

            std::memcpy( pProfilerMetricProperties,
                intelMetricsProperties.data(),
                returnedMetricPropertyCount * sizeof( VkProfilerMetricPropertiesEXT ) );

            // There may be other metric sources that will be appended to the end
            pProfilerMetricProperties += returnedMetricPropertyCount;
            (*pProfilerMetricCount) -= returnedMetricPropertyCount;

            if( returnedMetricPropertyCount < intelMetricsProperties.size() )
            {
                hasSufficientSpace = false;
            }
        }
    }

    // TODO: Other metric sources (VK_KHR_performance_query)

    if( !hasSufficientSpace )
    {
        // All vkEnumerate* functions return VK_INCOMPLETE when provided buffer was too small
        return VK_INCOMPLETE;
    }

    return VK_SUCCESS;
}
