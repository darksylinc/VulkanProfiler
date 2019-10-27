#include "VkCommandBuffer_functions.h"

namespace Profiler
{
    /***********************************************************************************\

    Function:
        BeginCommandBuffer

    Description:

    \***********************************************************************************/
    VKAPI_ATTR VkResult VKAPI_CALL VkCommandBuffer_Functions::BeginCommandBuffer(
        VkCommandBuffer commandBuffer,
        const VkCommandBufferBeginInfo* pBeginInfo )
    {
        auto& dd = DeviceDispatch.Get( commandBuffer );

        dd.Profiler.BeginCommandBuffer( commandBuffer );

        return dd.DispatchTable.BeginCommandBuffer( commandBuffer, pBeginInfo );
    }

    /***********************************************************************************\

    Function:
        EndCommandBuffer

    Description:

    \***********************************************************************************/
    VKAPI_ATTR VkResult VKAPI_CALL VkCommandBuffer_Functions::EndCommandBuffer(
        VkCommandBuffer commandBuffer )
    {
        auto& dd = DeviceDispatch.Get( commandBuffer );

        dd.Profiler.EndCommandBuffer( commandBuffer );

        return dd.DispatchTable.EndCommandBuffer( commandBuffer );
    }

    /***********************************************************************************\

    Function:
        CmdBeginRenderPass

    Description:

    \***********************************************************************************/
    VKAPI_ATTR void VKAPI_CALL VkCommandBuffer_Functions::CmdBeginRenderPass(
        VkCommandBuffer commandBuffer,
        const VkRenderPassBeginInfo* pBeginInfo,
        VkSubpassContents subpassContents )
    {
        auto& dd = DeviceDispatch.Get( commandBuffer );

        // Profile the render pass time
        dd.Profiler.BeginRenderPass( commandBuffer, pBeginInfo->renderPass );

        // Begin the render pass
        dd.DispatchTable.CmdBeginRenderPass( commandBuffer, pBeginInfo, subpassContents );
    }

    /***********************************************************************************\

    Function:
        CmdEndRenderPass

    Description:

    \***********************************************************************************/
    VKAPI_ATTR void VKAPI_CALL VkCommandBuffer_Functions::CmdEndRenderPass(
        VkCommandBuffer commandBuffer )
    {
        auto& dd = DeviceDispatch.Get( commandBuffer );

        // End the render pass
        dd.DispatchTable.CmdEndRenderPass( commandBuffer );

        // Profile the render pass time
        dd.Profiler.EndRenderPass( commandBuffer );
    }

    /***********************************************************************************\

    Function:
        CmdBindPipeline

    Description:

    \***********************************************************************************/
    VKAPI_ATTR void VKAPI_CALL VkCommandBuffer_Functions::CmdBindPipeline(
        VkCommandBuffer commandBuffer,
        VkPipelineBindPoint bindPoint,
        VkPipeline pipeline )
    {
        auto& dd = DeviceDispatch.Get( commandBuffer );

        // Bind the pipeline
        dd.DispatchTable.CmdBindPipeline( commandBuffer, bindPoint, pipeline );

        // Profile the pipeline time
        dd.Profiler.BindPipeline( commandBuffer, pipeline );
    }

    /***********************************************************************************\

    Function:
        CmdDraw

    Description:

    \***********************************************************************************/
    VKAPI_ATTR void VKAPI_CALL VkCommandBuffer_Functions::CmdDraw(
        VkCommandBuffer commandBuffer,
        uint32_t vertexCount,
        uint32_t instanceCount,
        uint32_t firstVertex,
        uint32_t firstInstance )
    {
        auto& dd = DeviceDispatch.Get( commandBuffer );

        // Increment drawcall counter
        dd.Profiler.GetCurrentFrameStats().drawCount++;

        dd.Profiler.PreDraw( commandBuffer );

        // Invoke next layer's implementation
        dd.DispatchTable.CmdDraw(
            commandBuffer, vertexCount, instanceCount, firstVertex, firstInstance );

        dd.Profiler.PostDraw( commandBuffer );
    }

    /***********************************************************************************\

    Function:
        CmdDrawIndexed

    Description:

    \***********************************************************************************/
    VKAPI_ATTR void VKAPI_CALL VkCommandBuffer_Functions::CmdDrawIndexed(
        VkCommandBuffer commandBuffer,
        uint32_t indexCount,
        uint32_t instanceCount,
        uint32_t firstIndex,
        int32_t vertexOffset,
        uint32_t firstInstance )
    {
        auto& dd = DeviceDispatch.Get( commandBuffer );

        // Increment drawcall counter
        dd.Profiler.GetCurrentFrameStats().drawCount++;

        dd.Profiler.PreDraw( commandBuffer );

        // Invoke next layer's implementation
        dd.DispatchTable.CmdDrawIndexed(
            commandBuffer, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance );

        dd.Profiler.PostDraw( commandBuffer );
    }
}
