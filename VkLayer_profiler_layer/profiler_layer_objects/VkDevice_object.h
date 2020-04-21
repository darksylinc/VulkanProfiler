#pragma once
#include "VkInstance_object.h"
#include "VkQueue_object.h"
#include "VkSwapchainKHR_object.h"
#include <vector>
#include <vk_layer.h>
#include <vk_dispatch_table_helper.h>

namespace Profiler
{
    struct VkDevice_debug_Object
    {
        std::map<uint64_t, std::string> ObjectNames;
    };

    struct VkDevice_Object
    {
        VkDevice Handle;

        VkInstance_Object* pInstance;
        VkPhysicalDevice PhysicalDevice;

        // Dispatch tables
        VkLayerDispatchTable Callbacks;

        VkPhysicalDeviceProperties Properties;
        VkPhysicalDeviceMemoryProperties MemoryProperties;
        
        VkDevice_debug_Object Debug;

        std::unordered_map<VkQueue, VkQueue_Object> Queues;

        // Swapchains created with this device
        std::unordered_map<VkSwapchainKHR, VkSwapchainKHR_Object> Swapchains;
    };
}
