#pragma once

#include <unordered_map>
#include <mutex>
#include <fstream>
#include <map>
#include <deque>

#include <daxa/core.hpp>

#if defined(_WIN32)
#define VK_USE_PLATFORM_WIN32_KHR
#define VK_KHR_win32_surface
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "Windows.h"
#include "wrl/client.h"
using namespace Microsoft::WRL;
#include <dxcapi.h>
#else
#define VK_USE_PLATFORM_XLIB_KHR
#define VK_KHR_xlib_surface
#define SCARD_E_FILE_NOT_FOUND 0x80100024
#define SCARD_E_INVALID_PARAMETER 0x80100004
#include <dxc/dxcapi.h>
template <typename T>
using ComPtr = CComPtr<T>;
#endif

#include <volk.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <vk_mem_alloc.h>

namespace daxa
{
    static inline constexpr u32 MAX_PUSH_CONSTANT_WORD_SIZE = {32};
    static inline constexpr u32 MAX_PUSH_CONSTANT_BYTE_SIZE = {MAX_PUSH_CONSTANT_WORD_SIZE * 4};
    static inline constexpr u32 PIPELINE_LAYOUT_COUNT = {MAX_PUSH_CONSTANT_WORD_SIZE + 1};
    static inline constexpr char const * MAX_PUSH_CONSTANT_SIZE_ERROR = {"push constant size is limited to 128 bytes/ 32 device words"};
} // namespace daxa