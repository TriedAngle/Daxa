#pragma once

#include <daxa/utils/imgui.hpp>
#include <deque>

namespace daxa
{
    struct ImplImGuiRenderer final : ManagedSharedState
    {
        ImGuiRendererInfo info;
        RasterPipeline raster_pipeline;
        BufferId vbuffer, staging_vbuffer;
        BufferId ibuffer, staging_ibuffer;
        SamplerId sampler;
        ImageId font_sheet;

        void recreate_vbuffer(usize vbuffer_new_size);
        void recreate_ibuffer(usize ibuffer_new_size);
        void record_commands(ImDrawData * draw_data, CommandList & cmd_list, ImageId target_image, u32 size_x, u32 size_y);
        auto managed_cleanup() -> bool override;

        ImplImGuiRenderer(ImGuiRendererInfo const & info);
        virtual ~ImplImGuiRenderer() override final;
    };
} // namespace daxa
