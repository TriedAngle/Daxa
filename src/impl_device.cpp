#include "impl_device.hpp"

namespace daxa
{
    Device::Device(ManagedPtr impl) : ManagedPtr(std::move(impl)) {}

    auto Device::info() const -> DeviceInfo const &
    {
        auto & impl = *as<ImplDevice>();
        return impl.info;
    }

    auto Device::properties() const -> DeviceProperties const &
    {
        auto & impl = *as<ImplDevice>();
        return impl.vk_info;
    }

    void Device::wait_idle()
    {
        auto & impl = *as<ImplDevice>();
        impl.wait_idle();
    }

    void Device::submit_commands(CommandSubmitInfo const & submit_info)
    {
        auto & impl = *as<ImplDevice>();

        impl.main_queue_collect_garbage();

        u64 curreny_main_queue_cpu_timeline_value = DAXA_ATOMIC_FETCH_INC(impl.main_queue_cpu_timeline) + 1;

        std::pair<u64, std::vector<ManagedPtr>> submit = {curreny_main_queue_cpu_timeline_value, {}};

        std::vector<VkCommandBuffer> submit_vk_command_buffers = {};
        for (auto & command_list : submit_info.command_lists)
        {
            auto & impl_cmd_list = *command_list.as<ImplCommandList>();
            DAXA_DBG_ASSERT_TRUE_M(impl_cmd_list.recording_complete, "all submitted command lists must be completed before submission");
            submit.second.push_back(command_list);
            submit_vk_command_buffers.push_back(impl_cmd_list.vk_cmd_buffer);
        }

        std::vector<VkSemaphore> submit_semaphore_signals = {}; // All timeline semaphores come first, then binary semaphores follow.
        std::vector<u64> submit_semaphore_signal_values = {};   // Used for timeline semaphores. Ignored (push dummy value) for binary semaphores.

        // Add main queue timeline signaling as first timeline semaphore singaling:
        submit_semaphore_signals.push_back(impl.vk_main_queue_gpu_timeline_semaphore);
        submit_semaphore_signal_values.push_back(curreny_main_queue_cpu_timeline_value);

        for (auto & [timeline_semaphore, signal_value] : submit_info.signal_timeline_semaphores)
        {
            auto & impl_timeline_semaphore = *timeline_semaphore.as<ImplTimelineSemaphore>();
            submit_semaphore_signals.push_back(impl_timeline_semaphore.vk_semaphore);
            submit_semaphore_signal_values.push_back(signal_value);
        }

        for (auto & binary_semaphore : submit_info.signal_binary_semaphores)
        {
            auto & impl_binary_semaphore = *binary_semaphore.as<ImplBinarySemaphore>();
            submit_semaphore_signals.push_back(impl_binary_semaphore.vk_semaphore);
            submit_semaphore_signal_values.push_back(0); // The vulkan spec requires to have dummy values for binary semaphores.
        }

        // used to synchronize with previous submits:
        std::vector<VkSemaphore> submit_semaphore_waits = {}; // All timeline semaphores come first, then binary semaphores follow.
        std::vector<VkPipelineStageFlags> submit_semaphore_wait_stage_masks = {};
        std::vector<u64> submit_semaphore_wait_values = {}; // Used for timeline semaphores. Ignored (push dummy value) for binary semaphores.

        for (auto & [timeline_semaphore, wait_value] : submit_info.wait_timeline_semaphores)
        {
            auto & impl_timeline_semaphore = *timeline_semaphore.as<ImplTimelineSemaphore>();
            submit_semaphore_waits.push_back(impl_timeline_semaphore.vk_semaphore);
            submit_semaphore_wait_stage_masks.push_back(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
            submit_semaphore_wait_values.push_back(wait_value);
        }

        for (auto & binary_semaphore : submit_info.wait_binary_semaphores)
        {
            auto & impl_binary_semaphore = *binary_semaphore.as<ImplBinarySemaphore>();
            submit_semaphore_waits.push_back(impl_binary_semaphore.vk_semaphore);
            submit_semaphore_wait_stage_masks.push_back(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
            submit_semaphore_wait_values.push_back(0);
        }

        VkTimelineSemaphoreSubmitInfo timelineInfo{
            .sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
            .pNext = nullptr,
            .waitSemaphoreValueCount = static_cast<u32>(submit_semaphore_wait_values.size()),
            .pWaitSemaphoreValues = submit_semaphore_wait_values.data(),
            .signalSemaphoreValueCount = static_cast<u32>(submit_semaphore_signal_values.size()),
            .pSignalSemaphoreValues = submit_semaphore_signal_values.data(),
        };

        VkPipelineStageFlags pipe_stage_flags = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSubmitInfo vk_submit_info{
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .pNext = reinterpret_cast<void *>(&timelineInfo),
            .waitSemaphoreCount = static_cast<u32>(submit_semaphore_waits.size()),
            .pWaitSemaphores = submit_semaphore_waits.data(),
            .pWaitDstStageMask = submit_semaphore_wait_stage_masks.data(),
            .commandBufferCount = static_cast<u32>(submit_vk_command_buffers.size()),
            .pCommandBuffers = submit_vk_command_buffers.data(),
            .signalSemaphoreCount = static_cast<u32>(submit_semaphore_signals.size()),
            .pSignalSemaphores = submit_semaphore_signals.data(),
        };
        vkQueueSubmit(impl.main_queue_vk_queue, 1, &vk_submit_info, VK_NULL_HANDLE);

        DAXA_ONLY_IF_THREADSAFETY(std::unique_lock lock{impl.main_queue_zombies_mtx});
        impl.main_queue_submits_zombies.push_front(std::move(submit));
    }

    void Device::present_frame(PresentInfo const & info)
    {
        auto & impl = *as<ImplDevice>();
        auto & swapchain_impl = *info.swapchain.as<ImplSwapchain>();

        // used to synchronise with previous submits:
        std::vector<VkSemaphore> submit_semaphore_waits = {};

        for (auto & binary_semaphore : info.wait_binary_semaphores)
        {
            auto & impl_binary_semaphore = *binary_semaphore.as<ImplBinarySemaphore>();
            submit_semaphore_waits.push_back(impl_binary_semaphore.vk_semaphore);
        }

        u32 image_index = 0;
        VkPresentInfoKHR present_info{
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .pNext = nullptr,
            .waitSemaphoreCount = static_cast<u32>(submit_semaphore_waits.size()),
            .pWaitSemaphores = submit_semaphore_waits.data(),
            .swapchainCount = static_cast<u32>(1),
            .pSwapchains = &swapchain_impl.vk_swapchain,
            .pImageIndices = &swapchain_impl.current_image_index,
        };

        VkResult err;
        err = vkQueuePresentKHR(impl.main_queue_vk_queue, &present_info);

        if (err == VK_ERROR_OUT_OF_DATE_KHR)
        {
            swapchain_impl.recreate();
        }
        else if (err == VK_SUBOPTIMAL_KHR)
        {
            VkSurfaceCapabilitiesKHR surface_capabilities;
            vkGetPhysicalDeviceSurfaceCapabilitiesKHR(swapchain_impl.impl_device.as<ImplDevice>()->vk_physical_device, swapchain_impl.vk_surface, &surface_capabilities);
            if (surface_capabilities.currentExtent.width != swapchain_impl.info.width ||
                surface_capabilities.currentExtent.height != swapchain_impl.info.height)
            {
                swapchain_impl.recreate();
            }
        }
        else if (err == VK_ERROR_SURFACE_LOST_KHR)
        {
            swapchain_impl.recreate_surface();
            swapchain_impl.recreate();
        }
        else if (err != VK_SUCCESS)
        {
            throw std::runtime_error("Unexpected swapchain error");
        }

        collect_garbage();
    }

    void Device::collect_garbage()
    {
        auto & impl = *as<ImplDevice>();
        impl.main_queue_collect_garbage();
    }

    auto Device::create_swapchain(SwapchainInfo const & info) -> Swapchain
    {
        return Swapchain{ManagedPtr{new ImplSwapchain(this->make_weak(), info)}};
    }

    auto Device::create_pipeline_compiler(PipelineCompilerInfo const & info) -> PipelineCompiler
    {
        return PipelineCompiler{new ImplPipelineCompiler(this->make_weak(), info)};
    }

    auto Device::create_command_list(CommandListInfo const & info) -> CommandList
    {
        auto impl = as<ImplDevice>();
        return CommandList{ManagedPtr{impl->command_list_recyclable_list.recycle_or_create_new(this->make_weak(), info).release()}};
    }

    auto Device::create_binary_semaphore(BinarySemaphoreInfo const & info) -> BinarySemaphore
    {
        auto impl = as<ImplDevice>();
        return BinarySemaphore{ManagedPtr{impl->binary_semaphore_recyclable_list.recycle_or_create_new(this->make_weak(), info).release()}};
    }

    auto Device::create_timeline_semaphore(TimelineSemaphoreInfo const & info) -> TimelineSemaphore
    {
        auto impl = as<ImplDevice>();
        return TimelineSemaphore{ManagedPtr{new ImplTimelineSemaphore(this->make_weak(), info)}};
    }

    auto Device::create_buffer(BufferInfo const & info) -> BufferId
    {
        auto & impl = *as<ImplDevice>();
        return impl.new_buffer(info);
    }

    auto Device::create_image(ImageInfo const & info) -> ImageId
    {
        auto & impl = *as<ImplDevice>();
        return impl.new_image(info);
    }

    auto Device::create_image_view(ImageViewInfo const & info) -> ImageViewId
    {
        auto & impl = *as<ImplDevice>();
        return impl.new_image_view(info);
    }

    auto Device::create_sampler(SamplerInfo const & info) -> SamplerId
    {
        auto & impl = *as<ImplDevice>();
        return impl.new_sampler(info);
    }

    void Device::destroy_buffer(BufferId id)
    {
        auto & impl = *as<ImplDevice>();
        impl.zombiefy_buffer(id);
    }

    void Device::destroy_image(ImageId id)
    {
        auto & impl = *as<ImplDevice>();
        impl.zombiefy_image(id);
    }

    void Device::destroy_image_view(ImageViewId id)
    {
        auto & impl = *as<ImplDevice>();
        impl.zombiefy_image_view(id);
    }

    void Device::destroy_sampler(SamplerId id)
    {
        auto & impl = *as<ImplDevice>();
        impl.zombiefy_sampler(id);
    }

    auto Device::info_buffer(BufferId id) const -> BufferInfo
    {
        auto & impl = *as<ImplDevice>();
        return impl.slot(id).info;
    }

    auto Device::info_image(ImageId id) const -> ImageInfo
    {
        auto & impl = *as<ImplDevice>();
        return impl.slot(id).info;
    }

    auto Device::info_image_view(ImageViewId id) const -> ImageViewInfo
    {
        auto & impl = *as<ImplDevice>();
        return impl.slot(id).info;
    }

    auto Device::info_sampler(SamplerId id) const -> SamplerInfo
    {
        auto & impl = *as<ImplDevice>();
        return impl.slot(id).info;
    }

    auto Device::map_memory(BufferId id) -> void *
    {
        auto & impl = *as<ImplDevice>();
        void * ret = nullptr;
        vmaMapMemory(impl.vma_allocator, impl.slot(id).vma_allocation, &ret);
        return ret;
    }

    void Device::unmap_memory(BufferId id)
    {
        auto & impl = *as<ImplDevice>();
        void * ret = nullptr;
        vmaUnmapMemory(impl.vma_allocator, impl.slot(id).vma_allocation);
    }

    static const VkPhysicalDeviceFeatures REQUIRED_PHYSICAL_DEVICE_FEATURES{
        .robustBufferAccess = VK_FALSE,
        .fullDrawIndexUint32 = VK_FALSE,
        .imageCubeArray = VK_FALSE,
        .independentBlend = VK_FALSE,
        .geometryShader = VK_FALSE,
        .tessellationShader = VK_FALSE,
        .sampleRateShading = VK_FALSE,
        .dualSrcBlend = VK_FALSE,
        .logicOp = VK_FALSE,
        .multiDrawIndirect = VK_FALSE,
        .drawIndirectFirstInstance = VK_FALSE,
        .depthClamp = VK_FALSE,
        .depthBiasClamp = VK_FALSE,
        .fillModeNonSolid = VK_FALSE,
        .depthBounds = VK_FALSE,
        .wideLines = VK_FALSE,
        .largePoints = VK_FALSE,
        .alphaToOne = VK_FALSE,
        .multiViewport = VK_FALSE,
        .samplerAnisotropy = VK_FALSE,
        .textureCompressionETC2 = VK_FALSE,
        .textureCompressionASTC_LDR = VK_FALSE,
        .textureCompressionBC = VK_FALSE,
        .occlusionQueryPrecise = VK_FALSE,
        .pipelineStatisticsQuery = VK_FALSE,
        .vertexPipelineStoresAndAtomics = VK_FALSE,
        .fragmentStoresAndAtomics = VK_FALSE,
        .shaderTessellationAndGeometryPointSize = VK_FALSE,
        .shaderImageGatherExtended = VK_FALSE,
        .shaderStorageImageExtendedFormats = VK_FALSE,
        .shaderStorageImageMultisample = VK_FALSE,
        .shaderStorageImageReadWithoutFormat = VK_FALSE,
        .shaderStorageImageWriteWithoutFormat = VK_FALSE,
        .shaderUniformBufferArrayDynamicIndexing = VK_FALSE,
        .shaderSampledImageArrayDynamicIndexing = VK_FALSE,
        .shaderStorageBufferArrayDynamicIndexing = VK_FALSE,
        .shaderStorageImageArrayDynamicIndexing = VK_FALSE,
        .shaderClipDistance = VK_FALSE,
        .shaderCullDistance = VK_FALSE,
        .shaderFloat64 = VK_FALSE,
        .shaderInt64 = VK_FALSE,
        .shaderInt16 = VK_FALSE,
        .shaderResourceResidency = VK_FALSE,
        .shaderResourceMinLod = VK_FALSE,
        .sparseBinding = VK_FALSE,
        .sparseResidencyBuffer = VK_FALSE,
        .sparseResidencyImage2D = VK_FALSE,
        .sparseResidencyImage3D = VK_FALSE,
        .sparseResidency2Samples = VK_FALSE,
        .sparseResidency4Samples = VK_FALSE,
        .sparseResidency8Samples = VK_FALSE,
        .sparseResidency16Samples = VK_FALSE,
        .sparseResidencyAliased = VK_FALSE,
        .variableMultisampleRate = VK_FALSE,
        .inheritedQueries = VK_FALSE,
    };

    static const VkPhysicalDeviceDescriptorIndexingFeatures REQUIRED_PHYSICAL_DEVICE_FEATURES_DESCRIPTOR_INDEXING{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES,
        .pNext = nullptr,
        .shaderInputAttachmentArrayDynamicIndexing = VK_FALSE, // no render passes
        .shaderUniformTexelBufferArrayDynamicIndexing = VK_TRUE,
        .shaderStorageTexelBufferArrayDynamicIndexing = VK_TRUE,
        .shaderUniformBufferArrayNonUniformIndexing = VK_TRUE,
        .shaderSampledImageArrayNonUniformIndexing = VK_TRUE,
        .shaderStorageBufferArrayNonUniformIndexing = VK_TRUE,
        .shaderStorageImageArrayNonUniformIndexing = VK_TRUE,
        .shaderInputAttachmentArrayNonUniformIndexing = VK_FALSE, // no render passes
        .shaderUniformTexelBufferArrayNonUniformIndexing = VK_TRUE,
        .shaderStorageTexelBufferArrayNonUniformIndexing = VK_TRUE,
        .descriptorBindingUniformBufferUpdateAfterBind = VK_FALSE, // no uniform buffers
        .descriptorBindingSampledImageUpdateAfterBind = VK_TRUE,
        .descriptorBindingStorageImageUpdateAfterBind = VK_TRUE,
        .descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE,
        .descriptorBindingUniformTexelBufferUpdateAfterBind = VK_FALSE, // no uniform buffers
        .descriptorBindingStorageTexelBufferUpdateAfterBind = VK_TRUE,
        .descriptorBindingUpdateUnusedWhilePending = VK_TRUE,
        .descriptorBindingPartiallyBound = VK_TRUE,
        .descriptorBindingVariableDescriptorCount = VK_TRUE,
        .runtimeDescriptorArray = VK_TRUE,
    };

    static const VkPhysicalDeviceDynamicRenderingFeatures REQUIRED_PHYSICAL_DEVICE_FEATURES_DYNAMIC_RENDERING{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR,
        .pNext = (void *)(&REQUIRED_PHYSICAL_DEVICE_FEATURES_DESCRIPTOR_INDEXING),
        .dynamicRendering = VK_TRUE,
    };

    static const VkPhysicalDeviceTimelineSemaphoreFeatures REQUIRED_PHYSICAL_DEVICE_FEATURES_TIMELINE_SEMAPHORE{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES,
        .pNext = (void *)(&REQUIRED_PHYSICAL_DEVICE_FEATURES_DYNAMIC_RENDERING),
        .timelineSemaphore = VK_TRUE,
    };

    static const VkPhysicalDeviceSynchronization2Features REQUIRED_PHYSICAL_DEVICE_FEATURES_SYNCHRONIZATION_2{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES_KHR,
        .pNext = (void *)(&REQUIRED_PHYSICAL_DEVICE_FEATURES_TIMELINE_SEMAPHORE),
        .synchronization2 = VK_TRUE,
    };

    static const VkPhysicalDeviceRobustness2FeaturesEXT REQUIRED_PHYSICAL_DEVICE_FEATURES_ROBUSTNESS_2{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT,
        .pNext = (void *)(&REQUIRED_PHYSICAL_DEVICE_FEATURES_SYNCHRONIZATION_2),
        .nullDescriptor = VK_TRUE,
    };

    static VkPhysicalDeviceScalarBlockLayoutFeatures REQUIRED_PHYSICAL_DEVICE_FEATURES_SCALAR_LAYOUT{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES,
        .pNext = (void *)(&REQUIRED_PHYSICAL_DEVICE_FEATURES_ROBUSTNESS_2),
        .scalarBlockLayout = VK_TRUE,
    };

    // static const VkPhysicalDeviceMultiDrawFeaturesEXT REQUIRED_PHYSICAL_DEVICE_FEATURES_MULTI_DRAW{
    //     .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTI_DRAW_FEATURES_EXT,
    //     .pNext = (void *)(&REQUIRED_PHYSICAL_DEVICE_FEATURES_SCALAR_LAYOUT),
    //     .multiDraw = VK_TRUE,
    // };

    typedef struct VkPhysicalDeviceScalarBlockLayoutFeatures
    {
        VkStructureType sType;
        void * pNext;
        VkBool32 scalarBlockLayout;
    } VkPhysicalDeviceScalarBlockLayoutFeatures;

    static void * REQUIRED_DEVICE_FEATURE_P_CHAIN = (void *)(&REQUIRED_PHYSICAL_DEVICE_FEATURES_SCALAR_LAYOUT);

    ImplDevice::ImplDevice(DeviceInfo const & a_info, DeviceProperties const & a_vk_info, ManagedWeakPtr a_impl_ctx, VkPhysicalDevice a_physical_device)
        : info{a_info}, vk_info{a_vk_info}, impl_ctx{a_impl_ctx}, vk_physical_device{a_physical_device}
    {
        // SELECT QUEUE
        this->main_queue_family_index = std::numeric_limits<u32>::max();
        u32 queue_family_props_count = 0;
        std::vector<VkQueueFamilyProperties> queue_props;
        vkGetPhysicalDeviceQueueFamilyProperties(a_physical_device, &queue_family_props_count, nullptr);
        queue_props.resize(queue_family_props_count);
        vkGetPhysicalDeviceQueueFamilyProperties(a_physical_device, &queue_family_props_count, queue_props.data());
        std::vector<VkBool32> supports_present;
        supports_present.resize(queue_family_props_count);

        // Maybe check if device supports present for this surface?
        // this requires that the surface was already created.. which
        // is really CRINGE
        // for (u32 i = 0; i < queue_family_props_count; i++)
        //     vkGetPhysicalDeviceSurfaceSupportKHR(physical_device, i, surface, &supports_present[i]);

        for (u32 i = 0; i < queue_family_props_count; i++)
        {
            if ((queue_props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0 && (queue_props[i].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0 && (queue_props[i].queueFlags & VK_QUEUE_TRANSFER_BIT) != 0)
            {
                this->main_queue_family_index = i;
                break;
            }
        }
        DAXA_DBG_ASSERT_TRUE_M(this->main_queue_family_index != std::numeric_limits<u32>::max(), "found no suitable queue family");

        f32 queue_priorities[1] = {0.0};
        VkDeviceQueueCreateInfo queue_ci{
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .queueFamilyIndex = this->main_queue_family_index,
            .queueCount = 1,
            .pQueuePriorities = queue_priorities,
        };

        REQUIRED_PHYSICAL_DEVICE_FEATURES_SCALAR_LAYOUT.scalarBlockLayout = this->info.use_scalar_layout ? VK_TRUE : VK_FALSE;

        VkPhysicalDeviceFeatures2 physical_device_features_2{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
            .pNext = REQUIRED_DEVICE_FEATURE_P_CHAIN,
            .features = REQUIRED_PHYSICAL_DEVICE_FEATURES,
        };

        std::vector<char const *> extension_names;
        std::vector<char const *> enabled_layers;

        if (impl_ctx.as<ImplContext>()->info.enable_validation)
        {
            enabled_layers.push_back("VK_LAYER_KHRONOS_validation");
        }
        extension_names.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
        extension_names.push_back(VK_KHR_SHADER_DRAW_PARAMETERS_EXTENSION_NAME);
        // extension_names.push_back(VK_EXT_MULTI_DRAW_EXTENSION_NAME);

        VkDeviceCreateInfo device_ci = {
            .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            .pNext = &physical_device_features_2,
            .queueCreateInfoCount = static_cast<u32>(1),
            .pQueueCreateInfos = &queue_ci,
            .enabledLayerCount = static_cast<u32>(enabled_layers.size()),
            .ppEnabledLayerNames = enabled_layers.data(),
            .enabledExtensionCount = static_cast<u32>(extension_names.size()),
            .ppEnabledExtensionNames = extension_names.data(),
            .pEnabledFeatures = nullptr,
        };
        vkCreateDevice(a_physical_device, &device_ci, nullptr, &this->vk_device);
        volkLoadDevice(this->vk_device);
        u32 max_buffers = this->vk_info.limits.max_descriptor_set_storage_buffers;
        u32 max_images = std::min(this->vk_info.limits.max_descriptor_set_sampled_images, this->vk_info.limits.max_descriptor_set_storage_images);
        u32 max_samplers = this->vk_info.limits.max_descriptor_set_samplers;
        gpu_table.initialize(
            std::min({max_buffers, 1'000u}),
            std::min({max_images, 1'000u}),
            std::min({max_samplers, 1'000u}),
            vk_device);

        vkGetDeviceQueue(this->vk_device, this->main_queue_family_index, 0, &this->main_queue_vk_queue);

        VkSemaphoreTypeCreateInfo timelineCreateInfo{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
            .pNext = nullptr,
            .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
            .initialValue = 0,
        };

        VkSemaphoreCreateInfo vk_semaphore_create_info{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
            .pNext = reinterpret_cast<void *>(&timelineCreateInfo),
            .flags = {}};

        vkCreateSemaphore(this->vk_device, &vk_semaphore_create_info, nullptr, &this->vk_main_queue_gpu_timeline_semaphore);

        VmaVulkanFunctions vma_vulkan_functions{
            .vkGetInstanceProcAddr = vkGetInstanceProcAddr,
            .vkGetDeviceProcAddr = vkGetDeviceProcAddr,
        };

        VmaAllocatorCreateInfo vma_allocator_create_info{
            .flags = {},
            .physicalDevice = this->vk_physical_device,
            .device = this->vk_device,
            .preferredLargeHeapBlockSize = 0, // Sets it to lib internal default (256MiB).
            .pAllocationCallbacks = nullptr,
            .pDeviceMemoryCallbacks = nullptr,
            .pHeapSizeLimit = nullptr,
            .pVulkanFunctions = &vma_vulkan_functions,
            .instance = this->impl_ctx.as<ImplContext>()->vk_instance,
            .vulkanApiVersion = VK_API_VERSION_1_3,
        };

        vmaCreateAllocator(&vma_allocator_create_info, &this->vma_allocator);

        VkSamplerCreateInfo vk_sampler_create_info{
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .pNext = nullptr,
            .flags = {},
            .magFilter = VkFilter::VK_FILTER_LINEAR,
            .minFilter = VkFilter::VK_FILTER_LINEAR,
            .mipmapMode = VkSamplerMipmapMode::VK_SAMPLER_MIPMAP_MODE_LINEAR,
            .addressModeU = VkSamplerAddressMode::VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeV = VkSamplerAddressMode::VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeW = VkSamplerAddressMode::VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .mipLodBias = 0.0f,
            .anisotropyEnable = VK_FALSE,
            .maxAnisotropy = 0,
            .compareEnable = VK_FALSE,
            .compareOp = VkCompareOp::VK_COMPARE_OP_ALWAYS,
            .minLod = 0,
            .maxLod = 0,
            .borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK,
            .unnormalizedCoordinates = VK_FALSE,
        };
        vkCreateSampler(vk_device, &vk_sampler_create_info, nullptr, &this->vk_dummy_sampler);

        if (this->impl_ctx.as<ImplContext>()->enable_debug_names && this->info.debug_name.size() > 0)
        {
            VkDebugUtilsObjectNameInfoEXT device_name_info{
                .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
                .pNext = nullptr,
                .objectType = VK_OBJECT_TYPE_DEVICE,
                .objectHandle = reinterpret_cast<uint64_t>(this->vk_device),
                .pObjectName = this->info.debug_name.c_str(),
            };
            vkSetDebugUtilsObjectNameEXT(vk_device, &device_name_info);

            VkDebugUtilsObjectNameInfoEXT device_main_queue_name_info{
                .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
                .pNext = nullptr,
                .objectType = VK_OBJECT_TYPE_QUEUE,
                .objectHandle = reinterpret_cast<uint64_t>(this->main_queue_vk_queue),
                .pObjectName = this->info.debug_name.c_str(),
            };
            vkSetDebugUtilsObjectNameEXT(vk_device, &device_main_queue_name_info);

            VkDebugUtilsObjectNameInfoEXT device_main_queue_timeline_semaphore_name_info{
                .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
                .pNext = nullptr,
                .objectType = VK_OBJECT_TYPE_SEMAPHORE,
                .objectHandle = reinterpret_cast<uint64_t>(this->vk_main_queue_gpu_timeline_semaphore),
                .pObjectName = this->info.debug_name.c_str(),
            };
            vkSetDebugUtilsObjectNameEXT(vk_device, &device_main_queue_timeline_semaphore_name_info);
        }
    }

    void ImplDevice::main_queue_collect_garbage()
    {
        DAXA_ONLY_IF_THREADSAFETY(std::unique_lock lock{this->main_queue_zombies_mtx});
        u64 main_queue_cpu_timeline = DAXA_ATOMIC_FETCH(this->main_queue_cpu_timeline);

        u64 gpu_timeline_value = std::numeric_limits<u64>::max();
        auto vk_result = vkGetSemaphoreCounterValue(this->vk_device, this->vk_main_queue_gpu_timeline_semaphore, &gpu_timeline_value);
        DAXA_DBG_ASSERT_TRUE_M(vk_result != VK_ERROR_DEVICE_LOST, "device lost");

        auto check_and_cleanup_gpu_resources = [&](auto & zombies, auto const & cleanup_fn)
        {
            while (!zombies.empty())
            {
                auto & [timeline_value, object] = zombies.back();

                if (timeline_value > gpu_timeline_value)
                {
                    break;
                }

                cleanup_fn(object);
                zombies.pop_back();
            }
        };
        check_and_cleanup_gpu_resources(this->main_queue_submits_zombies, [&, this](auto & command_lists)
                                        {
            for (ManagedPtr & cmd_list_mp : command_lists)
            {
                auto cmd_list = cmd_list_mp.as<ImplCommandList>();
                for (usize i = 0; i < cmd_list->deferred_destruction_count; ++i)
                {
                    auto [id, index] = cmd_list->deferred_destructions[i];
                    switch (index)
                    {
                        case DEFERRED_DESTRUCTION_BUFFER_INDEX: this->main_queue_buffer_zombies.push_front({main_queue_cpu_timeline,BufferId{id}}); break;
                        case DEFERRED_DESTRUCTION_IMAGE_INDEX: this->main_queue_image_zombies.push_front({main_queue_cpu_timeline, ImageId{id}}); break;
                        case DEFERRED_DESTRUCTION_IMAGE_VIEW_INDEX: this->main_queue_image_view_zombies.push_front({main_queue_cpu_timeline, ImageViewId{id}}); break;
                        case DEFERRED_DESTRUCTION_SAMPLER_INDEX: this->main_queue_sampler_zombies.push_front({main_queue_cpu_timeline, SamplerId{id}}); break;
                        default: DAXA_DBG_ASSERT_TRUE_M(false, "unreachable");
                    }
                }
            }
            command_lists.clear(); });
        check_and_cleanup_gpu_resources(this->main_queue_buffer_zombies, [&](auto id)
                                        { this->cleanup_buffer(id); });
        check_and_cleanup_gpu_resources(this->main_queue_image_view_zombies, [&](auto id)
                                        { this->cleanup_image_view(id); });
        check_and_cleanup_gpu_resources(this->main_queue_image_zombies, [&](auto id)
                                        { this->cleanup_image(id); });
        check_and_cleanup_gpu_resources(this->main_queue_sampler_zombies, [&](auto id)
                                        { this->cleanup_sampler(id); });
        {
            DAXA_ONLY_IF_THREADSAFETY(std::unique_lock lock{this->binary_semaphore_recyclable_list.mtx});
            check_and_cleanup_gpu_resources(this->main_queue_binary_semaphore_zombies, [&](auto & binary_semaphore)
                                            { 
                binary_semaphore->reset();
                this->binary_semaphore_recyclable_list.recyclables.push_back(std::move(binary_semaphore)); });
        }
        check_and_cleanup_gpu_resources(this->main_queue_compute_pipeline_zombies, [&](auto & compute_pipeline) {});
        check_and_cleanup_gpu_resources(this->main_queue_raster_pipeline_zombies, [&](auto & raster_pipeline) {});
        check_and_cleanup_gpu_resources(this->main_queue_timeline_semaphore_zombies, [&](auto & timeline_semaphore) {});
    }

    void ImplDevice::wait_idle()
    {
        vkQueueWaitIdle(this->main_queue_vk_queue);
        vkDeviceWaitIdle(this->vk_device);
    }

    auto ImplDevice::new_buffer(BufferInfo const & info) -> BufferId
    {
        auto [id, ret] = gpu_table.buffer_slots.new_slot();

        DAXA_DBG_ASSERT_TRUE_M(info.size > 0, "can not create buffers of size zero");

        ret.info = info;

        VkBufferUsageFlags usageFlags =
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
            VK_BUFFER_USAGE_TRANSFER_DST_BIT |
            VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT |
            VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT |
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
            VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
            VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT |
            VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_COUNTER_BUFFER_BIT_EXT |
            VK_BUFFER_USAGE_CONDITIONAL_RENDERING_BIT_EXT |
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
            VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
            VK_BUFFER_USAGE_SHADER_BINDING_TABLE_BIT_KHR;

        VkBufferCreateInfo vk_buffer_create_info{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .pNext = nullptr,
            .flags = {},
            .size = static_cast<VkDeviceSize>(info.size),
            .usage = usageFlags,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 1,
            .pQueueFamilyIndices = &this->main_queue_family_index,
        };

        VmaAllocationCreateInfo vma_allocation_create_info{
            .flags = static_cast<VmaAllocationCreateFlags>(info.memory_flags),
            .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
            .memoryTypeBits = std::numeric_limits<u32>::max(),
            .pool = nullptr,
            .pUserData = nullptr,
            .priority = 0.5f,
        };

        vmaCreateBuffer(this->vma_allocator, &vk_buffer_create_info, &vma_allocation_create_info, &ret.vk_buffer, &ret.vma_allocation, nullptr);

        if (this->impl_ctx.as<ImplContext>()->enable_debug_names && info.debug_name.size() > 0)
        {
            VkDebugUtilsObjectNameInfoEXT swapchain_image_view_name_info{
                .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
                .pNext = nullptr,
                .objectType = VK_OBJECT_TYPE_BUFFER,
                .objectHandle = reinterpret_cast<uint64_t>(ret.vk_buffer),
                .pObjectName = info.debug_name.c_str(),
            };
            vkSetDebugUtilsObjectNameEXT(vk_device, &swapchain_image_view_name_info);
        }

        write_descriptor_set_buffer(this->vk_device, this->gpu_table.vk_descriptor_set, ret.vk_buffer, 0, static_cast<VkDeviceSize>(info.size), id.index);

        return BufferId{id};
    }

    auto ImplDevice::validate_image_slice(ImageMipArraySlice const & slice, ImageId id) -> ImageMipArraySlice
    {
        if (slice.level_count == std::numeric_limits<u32>::max() || slice.level_count == 0)
        {
            auto & info = this->slot(id).info;
            return ImageMipArraySlice{
                .image_aspect = info.aspect,
                .base_mip_level = 0,
                .level_count = info.mip_level_count,
                .base_array_layer = 0,
                .layer_count = info.array_layer_count,
            };
        }
        else
        {
            return slice;
        }
    }

    auto ImplDevice::validate_image_slice(ImageMipArraySlice const & slice, ImageViewId id) -> ImageMipArraySlice
    {
        if (slice.level_count == std::numeric_limits<u32>::max() || slice.level_count == 0)
        {
            return this->slot(id).info.slice;
        }
        else
        {
            return slice;
        }
    }

    auto ImplDevice::new_swapchain_image(VkImage swapchain_image, VkFormat format, u32 index, ImageUsageFlags usage, const std::string & debug_name) -> ImageId
    {
        auto [id, image_slot] = gpu_table.image_slots.new_slot();

        ImplImageSlot ret;
        ret.vk_image = swapchain_image;
        VkImageViewCreateInfo view_ci{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .image = swapchain_image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = format,
            .components = {
                .r = VK_COMPONENT_SWIZZLE_IDENTITY,
                .g = VK_COMPONENT_SWIZZLE_IDENTITY,
                .b = VK_COMPONENT_SWIZZLE_IDENTITY,
                .a = VK_COMPONENT_SWIZZLE_IDENTITY,
            },
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        };
        ret.swapchain_image_index = static_cast<i32>(index);
        ret.info = ImageInfo{};
        ret.info.debug_name = debug_name;
        vkCreateImageView(vk_device, &view_ci, nullptr, &ret.view_slot.vk_image_view);

        if (this->impl_ctx.as<ImplContext>()->enable_debug_names && debug_name.size() > 0)
        {
            VkDebugUtilsObjectNameInfoEXT swapchain_image_name_info{
                .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
                .pNext = nullptr,
                .objectType = VK_OBJECT_TYPE_IMAGE,
                .objectHandle = reinterpret_cast<uint64_t>(ret.vk_image),
                .pObjectName = debug_name.c_str(),
            };
            vkSetDebugUtilsObjectNameEXT(vk_device, &swapchain_image_name_info);

            VkDebugUtilsObjectNameInfoEXT swapchain_image_view_name_info{
                .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
                .pNext = nullptr,
                .objectType = VK_OBJECT_TYPE_IMAGE_VIEW,
                .objectHandle = reinterpret_cast<uint64_t>(ret.view_slot.vk_image_view),
                .pObjectName = debug_name.c_str(),
            };
            vkSetDebugUtilsObjectNameEXT(vk_device, &swapchain_image_view_name_info);
        }

        write_descriptor_set_image(this->vk_device, this->gpu_table.vk_descriptor_set, ret.view_slot.vk_image_view, usage, id.index);

        image_slot = ret;

        return ImageId{id};
    }

    auto ImplDevice::new_image(ImageInfo const & info) -> ImageId
    {
        auto [id, image_slot_variant] = gpu_table.image_slots.new_slot();

        VkDevice vk_device = this->vk_device;

        ImplImageSlot ret = {};
        ret.info = info;
        ret.view_slot.info = ImageViewInfo{
            .type = static_cast<ImageViewType>(info.dimensions),
            .format = info.format,
            .image = {id},
            .slice = ImageMipArraySlice{
                .image_aspect = info.aspect,
                .base_mip_level = 0,
                .level_count = info.mip_level_count,
                .base_array_layer = 0,
                .layer_count = info.array_layer_count,
            },
            .debug_name = info.debug_name,
        };

        DAXA_DBG_ASSERT_TRUE_M(info.dimensions >= 1 && info.dimensions <= 3, "image dimensions must be a value between 1 to 3(inclusive)");
        DAXA_DBG_ASSERT_TRUE_M(std::popcount(info.sample_count) == 1 && info.sample_count <= 64, "image samples must be power of two and between 1 and 64(inclusive)");

        VkImageType vk_image_type = static_cast<VkImageType>(info.dimensions - 1);

        VkImageCreateInfo vk_image_create_info{
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .pNext = nullptr,
            .flags = {},
            .imageType = vk_image_type,
            .format = *reinterpret_cast<VkFormat const *>(&info.format),
            .extent = *reinterpret_cast<VkExtent3D const *>(&info.size),
            .mipLevels = info.mip_level_count,
            .arrayLayers = info.array_layer_count,
            .samples = static_cast<VkSampleCountFlagBits>(info.sample_count),
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = info.usage,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 1,
            .pQueueFamilyIndices = &this->main_queue_family_index,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };

        VmaAllocationCreateInfo vma_allocation_create_info{
            .flags = static_cast<VmaAllocationCreateFlags>(info.memory_flags),
            .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
            .memoryTypeBits = std::numeric_limits<u32>::max(),
            .pool = nullptr,
            .pUserData = nullptr,
            .priority = 0.5f,
        };

        vmaCreateImage(this->vma_allocator, &vk_image_create_info, &vma_allocation_create_info, &ret.vk_image, &ret.vma_allocation, nullptr);

        VkImageViewType vk_image_view_type;
        if (info.array_layer_count > 1)
        {
            DAXA_DBG_ASSERT_TRUE_M(info.dimensions >= 1 && info.dimensions <= 2, "image dimensions must be 1 or 2 if making an image array");
            vk_image_view_type = static_cast<VkImageViewType>(info.dimensions + 3);
        }
        else
        {
            vk_image_view_type = static_cast<VkImageViewType>(info.dimensions - 1);
        }

        VkImageViewCreateInfo vk_image_view_create_info{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .pNext = nullptr,
            .flags = {},
            .image = ret.vk_image,
            .viewType = vk_image_view_type,
            .format = *reinterpret_cast<VkFormat const *>(&info.format),
            .components = VkComponentMapping{
                .r = VK_COMPONENT_SWIZZLE_IDENTITY,
                .g = VK_COMPONENT_SWIZZLE_IDENTITY,
                .b = VK_COMPONENT_SWIZZLE_IDENTITY,
                .a = VK_COMPONENT_SWIZZLE_IDENTITY,
            },
            .subresourceRange = {
                .aspectMask = static_cast<VkImageAspectFlags>(info.aspect),
                .baseMipLevel = 0,
                .levelCount = info.mip_level_count,
                .baseArrayLayer = 0,
                .layerCount = info.array_layer_count,
            },
        };

        vkCreateImageView(vk_device, &vk_image_view_create_info, nullptr, &ret.view_slot.vk_image_view);

        if (this->impl_ctx.as<ImplContext>()->enable_debug_names && info.debug_name.size() > 0)
        {
            VkDebugUtilsObjectNameInfoEXT swapchain_image_name_info{
                .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
                .pNext = nullptr,
                .objectType = VK_OBJECT_TYPE_IMAGE,
                .objectHandle = reinterpret_cast<uint64_t>(ret.vk_image),
                .pObjectName = info.debug_name.c_str(),
            };
            vkSetDebugUtilsObjectNameEXT(vk_device, &swapchain_image_name_info);

            VkDebugUtilsObjectNameInfoEXT swapchain_image_view_name_info{
                .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
                .pNext = nullptr,
                .objectType = VK_OBJECT_TYPE_IMAGE_VIEW,
                .objectHandle = reinterpret_cast<uint64_t>(ret.view_slot.vk_image_view),
                .pObjectName = info.debug_name.c_str(),
            };
            vkSetDebugUtilsObjectNameEXT(vk_device, &swapchain_image_view_name_info);
        }

        write_descriptor_set_image(this->vk_device, this->gpu_table.vk_descriptor_set, ret.view_slot.vk_image_view, info.usage, id.index);

        image_slot_variant = ret;

        return ImageId{id};
    }

    auto ImplDevice::new_image_view(ImageViewInfo const & info) -> ImageViewId
    {
        auto [id, image_slot] = gpu_table.image_slots.new_slot();

        image_slot = {};

        VkDevice vk_device = this->vk_device;

        ImplImageSlot & parent_image_slot = slot(info.image);

        ImplImageViewSlot ret = {};
        ret.info = info;

        ImageMipArraySlice slice = this->validate_image_slice(info.slice, info.image);
        ret.info.slice = slice;

        VkImageViewCreateInfo vk_image_view_create_info{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .pNext = nullptr,
            .flags = {},
            .image = parent_image_slot.vk_image,
            .viewType = static_cast<VkImageViewType>(info.type),
            .format = *reinterpret_cast<VkFormat const *>(&info.format),
            .components = VkComponentMapping{
                .r = VK_COMPONENT_SWIZZLE_IDENTITY,
                .g = VK_COMPONENT_SWIZZLE_IDENTITY,
                .b = VK_COMPONENT_SWIZZLE_IDENTITY,
                .a = VK_COMPONENT_SWIZZLE_IDENTITY,
            },
            .subresourceRange = *reinterpret_cast<VkImageSubresourceRange const *>(&slice),
        };

        vkCreateImageView(vk_device, &vk_image_view_create_info, nullptr, &ret.vk_image_view);

        if (this->impl_ctx.as<ImplContext>()->enable_debug_names && info.debug_name.size() > 0)
        {
            VkDebugUtilsObjectNameInfoEXT name_info{
                .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
                .pNext = nullptr,
                .objectType = VK_OBJECT_TYPE_IMAGE_VIEW,
                .objectHandle = reinterpret_cast<uint64_t>(ret.vk_image_view),
                .pObjectName = info.debug_name.c_str(),
            };
            vkSetDebugUtilsObjectNameEXT(vk_device, &name_info);
        }

        write_descriptor_set_image(this->vk_device, this->gpu_table.vk_descriptor_set, ret.vk_image_view, parent_image_slot.info.usage, id.index);

        image_slot.view_slot = ret;

        return ImageViewId{id};
    }

    auto ImplDevice::new_sampler(SamplerInfo const & info) -> SamplerId
    {
        auto [id, ret] = gpu_table.sampler_slots.new_slot();

        ret.info = info;

        VkSamplerCreateInfo vk_sampler_create_info{
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .pNext = nullptr,
            .flags = {},
            .magFilter = static_cast<VkFilter>(info.magnification_filter),
            .minFilter = static_cast<VkFilter>(info.minification_filter),
            .mipmapMode = static_cast<VkSamplerMipmapMode>(info.mipmap_filter),
            .addressModeU = static_cast<VkSamplerAddressMode>(info.adress_mode_u),
            .addressModeV = static_cast<VkSamplerAddressMode>(info.adress_mode_v),
            .addressModeW = static_cast<VkSamplerAddressMode>(info.adress_mode_w),
            .mipLodBias = info.mip_lod_bias,
            .anisotropyEnable = info.enable_anisotropy,
            .maxAnisotropy = info.max_anisotropy,
            .compareEnable = info.enable_compare,
            .compareOp = static_cast<VkCompareOp>(info.compareOp),
            .minLod = info.min_lod,
            .maxLod = info.max_lod,
            .borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK,
            .unnormalizedCoordinates = info.enable_unnormalized_coordinates,
        };

        vkCreateSampler(this->vk_device, &vk_sampler_create_info, nullptr, &ret.vk_sampler);

        if (this->impl_ctx.as<ImplContext>()->enable_debug_names && info.debug_name.size() > 0)
        {
            VkDebugUtilsObjectNameInfoEXT swapchain_image_view_name_info{
                .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
                .pNext = nullptr,
                .objectType = VK_OBJECT_TYPE_SAMPLER,
                .objectHandle = reinterpret_cast<uint64_t>(ret.vk_sampler),
                .pObjectName = info.debug_name.c_str(),
            };
            vkSetDebugUtilsObjectNameEXT(vk_device, &swapchain_image_view_name_info);
        }

        write_descriptor_set_sampler(this->vk_device, this->gpu_table.vk_descriptor_set, ret.vk_sampler, id.index);

        return SamplerId{id};
    }

    void ImplDevice::cleanup_buffer(BufferId id)
    {
        ImplBufferSlot & buffer_slot = this->gpu_table.buffer_slots.dereference_id(id);

        write_descriptor_set_buffer(this->vk_device, this->gpu_table.vk_descriptor_set, VK_NULL_HANDLE, 0, VK_WHOLE_SIZE, id.index);

        vmaDestroyBuffer(this->vma_allocator, buffer_slot.vk_buffer, buffer_slot.vma_allocation);

        buffer_slot = {};

        gpu_table.buffer_slots.return_slot(id);
    }

    void ImplDevice::cleanup_image(ImageId id)
    {
        ImplImageSlot & image_slot = gpu_table.image_slots.dereference_id(id);

        write_descriptor_set_image(this->vk_device, this->gpu_table.vk_descriptor_set, VK_NULL_HANDLE, image_slot.info.usage, id.index);

        vkDestroyImageView(vk_device, image_slot.view_slot.vk_image_view, nullptr);

        if (image_slot.swapchain_image_index == NOT_OWNED_BY_SWAPCHAIN && image_slot.vma_allocation != nullptr)
        {
            vmaDestroyImage(this->vma_allocator, image_slot.vk_image, image_slot.vma_allocation);
        }

        image_slot = {};

        gpu_table.image_slots.return_slot(id);
    }

    void ImplDevice::cleanup_image_view(ImageViewId id)
    {
        DAXA_DBG_ASSERT_TRUE_M(gpu_table.image_slots.dereference_id(id).vk_image == VK_NULL_HANDLE, "can not destroy default image view of image");

        ImplImageViewSlot & image_slot = gpu_table.image_slots.dereference_id(id).view_slot;

        write_descriptor_set_image(this->vk_device, this->gpu_table.vk_descriptor_set, VK_NULL_HANDLE, slot(image_slot.info.image).info.usage, id.index);

        vkDestroyImageView(vk_device, image_slot.vk_image_view, nullptr);

        image_slot = {};

        gpu_table.image_slots.return_slot(id);
    }

    void ImplDevice::cleanup_sampler(SamplerId id)
    {
        ImplSamplerSlot & sampler_slot = this->gpu_table.sampler_slots.dereference_id(id);

        write_descriptor_set_sampler(this->vk_device, this->gpu_table.vk_descriptor_set, this->vk_dummy_sampler, id.index);

        vkDestroySampler(this->vk_device, sampler_slot.vk_sampler, nullptr);

        sampler_slot = {};

        gpu_table.sampler_slots.return_slot(id);
    }

    auto ImplDevice::managed_cleanup() -> bool
    {
        wait_idle();
        main_queue_collect_garbage();

        binary_semaphore_recyclable_list.clear();
        command_list_recyclable_list.clear();

        vmaDestroyAllocator(this->vma_allocator);
        this->gpu_table.cleanup(this->vk_device);
        vkDestroySampler(vk_device, this->vk_dummy_sampler, nullptr);
        vkDestroySemaphore(this->vk_device, this->vk_main_queue_gpu_timeline_semaphore, nullptr);
        vkDestroyDevice(this->vk_device, nullptr);

        return true;
    }

    void ImplDevice::zombiefy_buffer(BufferId id)
    {
        DAXA_ONLY_IF_THREADSAFETY(std::unique_lock lock{this->main_queue_zombies_mtx});
        u64 main_queue_cpu_timeline = DAXA_ATOMIC_FETCH(this->main_queue_cpu_timeline);
        this->main_queue_buffer_zombies.push_front({main_queue_cpu_timeline, id});
    }

    void ImplDevice::zombiefy_image(ImageId id)
    {
        DAXA_ONLY_IF_THREADSAFETY(std::unique_lock lock{this->main_queue_zombies_mtx});
        u64 main_queue_cpu_timeline = DAXA_ATOMIC_FETCH(this->main_queue_cpu_timeline);
        this->main_queue_image_zombies.push_front({main_queue_cpu_timeline, id});
    }

    void ImplDevice::zombiefy_image_view(ImageViewId id)
    {
        DAXA_ONLY_IF_THREADSAFETY(std::unique_lock lock{this->main_queue_zombies_mtx});
        u64 main_queue_cpu_timeline = DAXA_ATOMIC_FETCH(this->main_queue_cpu_timeline);
        this->main_queue_image_view_zombies.push_front({main_queue_cpu_timeline, id});
    }

    void ImplDevice::zombiefy_sampler(SamplerId id)
    {
        DAXA_ONLY_IF_THREADSAFETY(std::unique_lock lock{this->main_queue_zombies_mtx});
        u64 main_queue_cpu_timeline = DAXA_ATOMIC_FETCH(this->main_queue_cpu_timeline);
        this->main_queue_sampler_zombies.push_front({main_queue_cpu_timeline, id});
    }

    auto ImplDevice::slot(BufferId id) -> ImplBufferSlot &
    {
        return gpu_table.buffer_slots.dereference_id(id);
    }

    auto ImplDevice::slot(ImageId id) -> ImplImageSlot &
    {
        return gpu_table.image_slots.dereference_id(id);
    }

    auto ImplDevice::slot(ImageViewId id) -> ImplImageViewSlot &
    {
        return gpu_table.image_slots.dereference_id(id).view_slot;
    }

    auto ImplDevice::slot(SamplerId id) -> ImplSamplerSlot &
    {
        return gpu_table.sampler_slots.dereference_id(id);
    }

    auto ImplDevice::slot(BufferId id) const -> ImplBufferSlot const &
    {
        return gpu_table.buffer_slots.dereference_id(id);
    }

    auto ImplDevice::slot(ImageId id) const -> ImplImageSlot const &
    {
        return gpu_table.image_slots.dereference_id(id);
    }

    auto ImplDevice::slot(ImageViewId id) const -> ImplImageViewSlot const &
    {
        return gpu_table.image_slots.dereference_id(id).view_slot;
    }

    auto ImplDevice::slot(SamplerId id) const -> ImplSamplerSlot const &
    {
        return gpu_table.sampler_slots.dereference_id(id);
    }
} // namespace daxa
