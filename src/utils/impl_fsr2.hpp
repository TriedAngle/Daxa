#pragma once

#include <daxa/utils/fsr2.hpp>
#include <deque>

#include "../impl_core.hpp"
#include "../impl_device.hpp"

#include <ffx-fsr2-api/ffx_fsr2.h>
#include <ffx-fsr2-api/vk/ffx_fsr2_vk.h>

namespace daxa
{
    struct ImplFsr2Context final : ManagedSharedState
    {
        UpscaleContextInfo info;

        FfxFsr2Context fsr2_context = {};
        FfxFsr2ContextDescription fsr2_context_description = {};

        void * scratch_buffer = {};
        bool initialized = {};

        auto managed_cleanup() -> bool override;
        void resize(UpscaleSizeInfo const & info);
        void upscale(CommandList & command_list, UpscaleInfo const & info);

        void create_resizable_resources();
        void destroy_resizable_resources();

        ImplFsr2Context(UpscaleContextInfo const & info);
        virtual ~ImplFsr2Context() override final;
    };
} // namespace daxa
