// #define

#include "impl_swapchain.hpp"
#include "impl_device.hpp"

namespace daxa
{
    Swapchain::Swapchain(ManagedPtr impl) : ManagedPtr(std::move(impl)) {}

    ImageId Swapchain::acquire_next_image()
    {
        auto & impl = *as<ImplSwapchain>();
        VkResult err;
        do
        {
            err = vkAcquireNextImageKHR(impl.impl_device.as<ImplDevice>()->vk_device, impl.vk_swapchain, UINT64_MAX, nullptr, impl.acquisition_fence, &impl.current_image_index);
            if (err == VK_ERROR_OUT_OF_DATE_KHR)
            {
                impl.recreate();
            }
            else if (err == VK_ERROR_SURFACE_LOST_KHR)
            {
                impl.recreate_surface();
                impl.recreate();
            }
            else if (err == VK_SUBOPTIMAL_KHR)
            {
                break;
            }
            else if (err != VK_SUCCESS)
            {
                throw std::runtime_error("Unexpected swapchain error");
            }
        } while (err != VK_SUCCESS);
        vkWaitForFences(impl.impl_device.as<ImplDevice>()->vk_device, 1, &impl.acquisition_fence, VK_TRUE, UINT64_MAX);
        vkResetFences(impl.impl_device.as<ImplDevice>()->vk_device, 1, &impl.acquisition_fence);
        return impl.image_resources[impl.current_image_index];
    }

    auto Swapchain::get_format() const -> Format
    {
        auto & impl = *as<ImplSwapchain>();
        return static_cast<Format>(impl.vk_surface_format.format);
    }

    void Swapchain::resize(u32 width, u32 height)
    {
        auto & impl = *as<ImplSwapchain>();
        impl.info.width = width;
        impl.info.height = height;
        impl.recreate();
    }

    void ImplSwapchain::recreate_surface()
    {
        if (this->vk_surface)
        {
            vkDestroySurfaceKHR(this->impl_device.as<ImplDevice>()->impl_ctx.as<ImplContext>()->vk_instance, this->vk_surface, nullptr);
        }
#if defined(_WIN32)
        VkWin32SurfaceCreateInfoKHR surface_ci{
            .sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
            .pNext = nullptr,
            .flags = 0,
            .hinstance = GetModuleHandleA(nullptr),
            .hwnd = info.native_window,
        };
        {
            auto func = (PFN_vkCreateWin32SurfaceKHR)vkGetInstanceProcAddr(impl_device.as<ImplDevice>()->impl_ctx.as<ImplContext>()->vk_instance, "vkCreateWin32SurfaceKHR");
            func(impl_device.as<ImplDevice>()->impl_ctx.as<ImplContext>()->vk_instance, &surface_ci, nullptr, &this->vk_surface);
        }
#elif defined(__linux__)
        VkXlibSurfaceCreateInfoKHR surface_ci{
            .sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR,
            .pNext = nullptr,
            .flags = 0,
            .dpy = XOpenDisplay(nullptr),
            .window = this->info.native_window,
        };
        {
            auto func = (PFN_vkCreateXlibSurfaceKHR)vkGetInstanceProcAddr(impl_device.as<ImplDevice>()->impl_ctx.as<ImplContext>()->vk_instance, "vkCreateXlibSurfaceKHR");
            func(impl_device.as<ImplDevice>()->impl_ctx.as<ImplContext>()->vk_instance, &surface_ci, nullptr, &this->vk_surface);
        }
#endif
    }

    ImplSwapchain::ImplSwapchain(ManagedWeakPtr a_impl_device, SwapchainInfo const & a_info)
        : impl_device{a_impl_device}, info{a_info}
    {
        recreate_surface();

        u32 format_count = 0;

        vkGetPhysicalDeviceSurfaceFormatsKHR(impl_device.as<ImplDevice>()->vk_physical_device, this->vk_surface, &format_count, nullptr);
        std::vector<VkSurfaceFormatKHR> surface_formats;
        surface_formats.resize(format_count);
        vkGetPhysicalDeviceSurfaceFormatsKHR(impl_device.as<ImplDevice>()->vk_physical_device, this->vk_surface, &format_count, surface_formats.data());
        DAXA_DBG_ASSERT_TRUE_M(format_count > 0, "No formats found");

        auto format_comparator = [&](auto const & a, auto const & b) -> bool
        {
            return a_info.surface_format_selector(static_cast<Format>(a.format)) <
                   a_info.surface_format_selector(static_cast<Format>(b.format));
        };
        auto best_format = std::max_element(surface_formats.begin(), surface_formats.end(), format_comparator);
        DAXA_DBG_ASSERT_TRUE_M(best_format != surface_formats.end(), "No viable formats found");
        this->vk_surface_format = *best_format;

        VkFenceCreateInfo fence_ci = {
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
        };
        vkCreateFence(impl_device.as<ImplDevice>()->vk_device, &fence_ci, nullptr, &this->acquisition_fence);

        recreate();
    }

    ImplSwapchain::~ImplSwapchain()
    {
        cleanup();
        vkDestroyFence(impl_device.as<ImplDevice>()->vk_device, this->acquisition_fence, nullptr);

        vkDestroySwapchainKHR(this->impl_device.as<ImplDevice>()->vk_device, this->vk_swapchain, nullptr);
        vkDestroySurfaceKHR(impl_device.as<ImplDevice>()->impl_ctx.as<ImplContext>()->vk_instance, this->vk_surface, nullptr);
    }

    void ImplSwapchain::recreate()
    {
        // compare this->info with new info
        // we need to pass in the old swapchain handle to create a new one

        VkSurfaceCapabilitiesKHR surface_capabilities;
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(this->impl_device.as<ImplDevice>()->vk_physical_device, this->vk_surface, &surface_capabilities);
        info.width = surface_capabilities.currentExtent.width;
        info.height = surface_capabilities.currentExtent.height;

        auto old_swapchain = this->vk_swapchain;

        cleanup();

        ImageUsageFlags usage = info.image_usage | ImageUsageFlagBits::COLOR_ATTACHMENT;

        VkSwapchainCreateInfoKHR swapchain_create_info{
            .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
            .pNext = nullptr,
            .flags = 0,
            .surface = this->vk_surface,
            .minImageCount = 3,
            .imageFormat = this->vk_surface_format.format,
            .imageColorSpace = this->vk_surface_format.colorSpace,
            .imageExtent = VkExtent2D{info.width, info.height},
            .imageArrayLayers = 1,
            .imageUsage = usage,
            .imageSharingMode = VkSharingMode::VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 1,
            .pQueueFamilyIndices = &this->impl_device.as<ImplDevice>()->main_queue_family_index,
            .preTransform = static_cast<VkSurfaceTransformFlagBitsKHR>(info.present_operation),
            .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
            .presentMode = static_cast<VkPresentModeKHR>(info.present_mode),
            .clipped = VK_TRUE,
            .oldSwapchain = old_swapchain,
        };

        vkCreateSwapchainKHR(
            this->impl_device.as<ImplDevice>()->vk_device,
            &swapchain_create_info,
            nullptr,
            &this->vk_swapchain);

        if (old_swapchain != VK_NULL_HANDLE)
        {
            vkDestroySwapchainKHR(impl_device.as<ImplDevice>()->vk_device, old_swapchain, nullptr);
        }

        u32 image_count;
        std::vector<VkImage> swapchain_images;
        vkGetSwapchainImagesKHR(impl_device.as<ImplDevice>()->vk_device, vk_swapchain, &image_count, nullptr);
        swapchain_images.resize(image_count);
        vkGetSwapchainImagesKHR(impl_device.as<ImplDevice>()->vk_device, vk_swapchain, &image_count, swapchain_images.data());
        this->image_resources.resize(image_count);
        for (u32 i = 0; i < image_resources.size(); i++)
        {
            std::string image_name = this->info.debug_name + " Image #" + std::to_string(i);
            this->image_resources[i] = this->impl_device.as<ImplDevice>()->new_swapchain_image(swapchain_images[i], vk_surface_format.format, i, usage, image_name);
        }

        if (this->impl_device.as<ImplDevice>()->impl_ctx.as<ImplContext>()->enable_debug_names && this->info.debug_name.size() > 0)
        {
            VkDebugUtilsObjectNameInfoEXT swapchain_name_info{
                .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
                .pNext = nullptr,
                .objectType = VK_OBJECT_TYPE_SWAPCHAIN_KHR,
                .objectHandle = reinterpret_cast<uint64_t>(this->vk_swapchain),
                .pObjectName = this->info.debug_name.c_str(),
            };
            vkSetDebugUtilsObjectNameEXT(impl_device.as<ImplDevice>()->vk_device, &swapchain_name_info);

            std::string fence_name = this->info.debug_name + " fence";
            VkDebugUtilsObjectNameInfoEXT fence_name_info{
                .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
                .pNext = nullptr,
                .objectType = VK_OBJECT_TYPE_FENCE,
                .objectHandle = reinterpret_cast<uint64_t>(this->acquisition_fence),
                .pObjectName = fence_name.c_str(),
            };
            vkSetDebugUtilsObjectNameEXT(impl_device.as<ImplDevice>()->vk_device, &fence_name_info);
        }
    }

    void ImplSwapchain::cleanup()
    {
        for (usize i = 0; i < image_resources.size(); i++)
        {
            this->impl_device.as<ImplDevice>()->zombiefy_image(image_resources[i]);
        }
        image_resources.clear();
    }
} // namespace daxa
