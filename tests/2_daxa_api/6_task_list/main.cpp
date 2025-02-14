#include <0_common/window.hpp>
#include <iostream>
#include <thread>

#include <daxa/utils/task_list.hpp>

#define APPNAME "Daxa API Sample: TaskList"
#define APPNAME_PREFIX(x) ("[" APPNAME "] " x)

struct AppContext
{
    daxa::Context daxa_ctx = daxa::create_context({
        .enable_validation = true,
    });
    daxa::Device device = daxa_ctx.create_device({
        .debug_name = APPNAME_PREFIX("device"),
    });
};

namespace tests
{
    using namespace daxa::types;

    void simplest()
    {
        AppContext app = {};
        auto task_list = daxa::TaskList({
            .device = app.device,
            .debug_name = APPNAME_PREFIX("task_list (simplest)"),
        });
    }

    void execution()
    {
        AppContext app = {};
        auto task_list = daxa::TaskList({
            .device = app.device,
            .debug_name = APPNAME_PREFIX("task_list (execution)"),
        });

        // This is pointless, but done to show how the task list executes
        task_list.add_task({
            .task = [&](daxa::TaskInterface &)
            {
                std::cout << "Hello, ";
            },
            .debug_name = APPNAME_PREFIX("task 1 (execution)"),
        });
        task_list.add_task({
            .task = [&](daxa::TaskInterface &)
            {
                std::cout << "World!" << std::endl;
            },
            .debug_name = APPNAME_PREFIX("task 2 (execution)"),
        });

        task_list.compile();

        task_list.execute();
    }

    void image_upload()
    {
        AppContext app = {};
        auto task_list = daxa::TaskList({
            .device = app.device,
            .debug_name = APPNAME_PREFIX("task_list (image_upload)"),
        });

        auto buffer = app.device.create_buffer({
            .size = 4,
            .debug_name = APPNAME_PREFIX("buffer (image_upload)"),
        });
        auto image = app.device.create_image({
            .size = {1, 1, 1},
            .usage = daxa::ImageUsageFlagBits::TRANSFER_DST | daxa::ImageUsageFlagBits::SHADER_READ_WRITE,
            .debug_name = APPNAME_PREFIX("image (image_upload)"),
        });

        auto task_image = task_list.create_task_image({
            .fetch_callback = [=]()
            { return image; },
            .debug_name = APPNAME_PREFIX("task_image (image_upload)"),
        });

        auto upload_buffer = task_list.create_task_buffer({
            .fetch_callback = [=]()
            { return buffer; },
            .debug_name = APPNAME_PREFIX("upload_buffer (image_upload)"),
        });

        task_list.add_task({
            .resources = {
                .buffers = {{upload_buffer, daxa::TaskBufferAccess::TRANSFER_READ}},
                .images = {{task_image, daxa::TaskImageAccess::TRANSFER_WRITE}},
            },
            .task = [](daxa::TaskInterface &)
            {
                // TODO: Implement this task!
            },
            .debug_name = APPNAME_PREFIX("upload task (image_upload)"),
        });

        task_list.compile();

        task_list.execute();

        app.device.destroy_buffer(buffer);
        app.device.destroy_image(image);
    }

    void output_graph()
    {
        AppContext app = {};
        auto task_list = daxa::TaskList({
            .device = app.device,
            .debug_name = APPNAME_PREFIX("task_list (output_graph)"),
        });

        auto task_buffer1 = task_list.create_task_buffer({
            .last_access = {daxa::PipelineStageFlagBits::HOST, daxa::AccessTypeFlagBits::WRITE},
            .debug_name = APPNAME_PREFIX("task_buffer1 (output_graph)"),
        });
        auto task_buffer2 = task_list.create_task_buffer({
            .last_access = {daxa::PipelineStageFlagBits::HOST, daxa::AccessTypeFlagBits::WRITE},
            .debug_name = APPNAME_PREFIX("task_buffer2 (output_graph)"),
        });
        auto task_buffer3 = task_list.create_task_buffer({
            .last_access = {daxa::PipelineStageFlagBits::HOST, daxa::AccessTypeFlagBits::WRITE},
            .debug_name = APPNAME_PREFIX("task_buffer3 (output_graph)"),
        });

        task_list.add_task({
            .resources = {.buffers = {{task_buffer1, daxa::TaskBufferAccess::SHADER_WRITE_ONLY}, {task_buffer2, daxa::TaskBufferAccess::SHADER_READ_ONLY}}},
            .task = [](daxa::TaskInterface &) {},
            .debug_name = APPNAME_PREFIX("task 1 (output_graph)"),
        });

        task_list.add_task({
            .resources = {.buffers = {{task_buffer2, daxa::TaskBufferAccess::SHADER_WRITE_ONLY}}},
            .task = [](daxa::TaskInterface &) {},
            .debug_name = APPNAME_PREFIX("task 2 (output_graph)"),
        });

        task_list.add_task({
            .resources = {.buffers = {{task_buffer2, daxa::TaskBufferAccess::SHADER_WRITE_ONLY}, {task_buffer3, daxa::TaskBufferAccess::SHADER_WRITE_ONLY}}},
            .task = [](daxa::TaskInterface &) {},
            .debug_name = APPNAME_PREFIX("task 3 (output_graph)"),
        });

        task_list.add_task({
            .resources = {.buffers = {{task_buffer3, daxa::TaskBufferAccess::SHADER_READ_ONLY}}},
            .task = [](daxa::TaskInterface &) {},
            .debug_name = APPNAME_PREFIX("task 4 (output_graph)"),
        });

        task_list.compile();
        task_list.output_graphviz();
    }

    void compute()
    {
        AppContext app = {};
        auto task_list = daxa::TaskList({
            .device = app.device,
            .debug_name = APPNAME_PREFIX("task_list (compute)"),
        });

        // std::array<f32, 32> data;
        // for (usize i = 0; i < data.size(); ++i)
        //     data[i] = static_cast<f32>(i);

        // // print the data first
        // for (usize i = 0; i < data.size(); ++i)
        //     std::cout << data[i] << ", ";
        // std::cout << std::endl;

        // auto pipeline_compiler = app.device.create_pipeline_compiler({
        //     .root_paths = {"include"}, // for daxa/daxa.hlsl
        //     .debug_name = "TaskList Pipeline Compiler",
        // });

        // struct Push
        // {
        //     daxa::BufferId data_buffer_id;
        // };
        // // clang-format off
        // auto pipeline = pipeline_compiler.create_compute_pipeline({
        //     .shader_info = {
        //         .source = daxa::ShaderCode{.string = R"(
        //             #include "daxa/daxa.hlsl"
        //             struct DataBuffer
        //             {
        //                 float data[32];
        //                 void compute(uint tid)
        //                 {
        //                     data[tid] = data[tid] * 2.0f;
        //                 }
        //             };
        //             DAXA_DEFINE_GET_STRUCTURED_BUFFER(DataBuffer);
        //             struct Push
        //             {
        //                 daxa::BufferId data_buffer_id;
        //             };
        //             [[vk::push_constant]] Push push;
        //             [numthreads(32, 1, 1)] void main(uint tid : SV_DISPATCHTHREADID)
        //             {
        //                 StructuredBuffer<DataBuffer> data_buffer = daxa::get_StructuredBuffer<DataBuffer>(push.data_buffer_id);
        //                 data_buffer.compute(tid);
        //             }
        //         )"},
        //     },
        //     .push_constant_size = sizeof(Push),
        // }).value();
        // // clang-format on

        // auto data_buffer = app.device.create_buffer({.size = sizeof(data)});
        // auto staging_buffer = app.device.create_buffer({
        //     .memory_flags = daxa::MemoryFlagBits::HOST_ACCESS_RANDOM,
        //     .size = sizeof(data),
        // });

        // // upload data
        // task_list.add_task({
        //     .resources = {
        //         .buffers = {
        //             { staging_buffer, daxa::TaskBufferAccess::TRANSFER_WRITE },
        //         },
        //     },
        //     .task = [&](daxa::TaskInterface &)
        //     {
        //         auto & mapped_data = *app.device.map_memory_as<decltype(data)>(staging_buffer);
        //         mapped_data = data;
        //         app.device.unmap_memory(staging_buffer);
        //         // transfer staging_buffer to data_buffer

        //     },
        // });

        // // run GPU algorithm
        // task_list.add_task({
        //     .task = [&](daxa::TaskInterface & inter)
        //     {
        //         auto cmd_list = inter.get_command_list();
        //         cmd_list.set_pipeline(pipeline);
        //         cmd_list.push_constant(Push{
        //             .data_buffer_id = data_buffer,
        //         });
        //         cmd_list.dispatch(1, 1, 1);
        //     },
        // });

        // // download data
        // task_list.add_task({
        //     .task = [&](daxa::TaskInterface &)
        //     {
        //         // transfer data_buffer to staging_buffer
        //         data = *app.device.map_memory_as<decltype(data)>(staging_buffer);
        //         app.device.unmap_memory(staging_buffer);
        //     },
        // });

        // task_list.compile();
        // task_list.execute();

        // // output the transformed data!
        // for (usize i = 0; i < data.size(); ++i)
        //     std::cout << data[i] << ", ";
        // std::cout << std::endl;

        // app.device.destroy_buffer(staging_buffer);
        // app.device.destroy_buffer(data_buffer);
    }

    void drawing()
    {
        struct App : AppWindow<App>
        {
            daxa::Context daxa_ctx = daxa::create_context({
                .enable_validation = true,
            });
            daxa::Device device = daxa_ctx.create_device({
                .debug_name = APPNAME_PREFIX("device (drawing)"),
            });

            daxa::Swapchain swapchain = device.create_swapchain({
                .native_window = get_native_handle(),
                .width = size_x,
                .height = size_y,
                .image_usage = daxa::ImageUsageFlagBits::TRANSFER_DST,
                .debug_name = APPNAME_PREFIX("swapchain (drawing)"),
            });

            daxa::PipelineCompiler pipeline_compiler = device.create_pipeline_compiler({
                .root_paths = {
                    "tests/2_daxa_api/6_task_list/shaders",
                    "include",
                },
                .debug_name = APPNAME_PREFIX("pipeline_compiler (drawing)"),
            });
            // clang-format off
            daxa::RasterPipeline raster_pipeline = pipeline_compiler.create_raster_pipeline({
                .vertex_shader_info = {.source = daxa::ShaderFile{"vert.hlsl"}},
                .fragment_shader_info = {.source = daxa::ShaderFile{"frag.hlsl"}},
                .color_attachments = {{.format = swapchain.get_format()}},
                .raster = {},
                .debug_name = APPNAME_PREFIX("raster_pipeline (drawing)"),
            }).value();
            // clang-format on

            daxa::TaskList task_list = record_task_list();

            daxa::ImageId render_image = create_render_image(size_x, size_y);
            daxa::TaskImageId task_render_image;

            daxa::ImageId swapchain_image;
            daxa::TaskImageId task_swapchain_image;

            App() : AppWindow<App>("Daxa API: Swapchain (clearcolor)")
            {
                record_task_list();
            }

            auto record_task_list() -> daxa::TaskList
            {
                daxa::TaskList new_task_list = daxa::TaskList({
                    .device = device,
                    .debug_name = APPNAME_PREFIX("task_list (drawing)"),
                });
                task_swapchain_image = new_task_list.create_task_image({
                    .fetch_callback = [this]()
                    { return swapchain_image; },
                    .debug_name = APPNAME_PREFIX("task_swapchain_image (drawing)"),
                });
                task_render_image = new_task_list.create_task_image({
                    .fetch_callback = [this]()
                    { return render_image; },
                    .debug_name = APPNAME_PREFIX("task_render_image (drawing)"),
                });

                new_task_list.add_clear_image({
                    .clear_value = {std::array<f32, 4>{1, 0, 1, 1}},
                    .dst_image = task_render_image,
                    .dst_slice = {},
                    .debug_name = APPNAME_PREFIX("Clear render_image Task (drawing)"),
                });
                new_task_list.add_task({
                    .resources = {
                        .images = {
                            {task_render_image, daxa::TaskImageAccess::FRAGMENT_SHADER_WRITE_ONLY},
                        },
                    },
                    .task = [this](daxa::TaskInterface interf)
                    {
                        auto cmd_list = interf.get_command_list();
                        cmd_list.begin_renderpass({
                            .color_attachments = {{.image_view = render_image.default_view()}},
                            .render_area = {.x = 0, .y = 0, .width = size_x, .height = size_y},
                        });
                        cmd_list.set_pipeline(raster_pipeline);
                        cmd_list.draw({.vertex_count = 3});
                        cmd_list.end_renderpass();
                    },
                    .debug_name = APPNAME_PREFIX("Draw to render_image Task (drawing)"),
                });

                new_task_list.add_copy_image_to_image({
                    .src_image = task_render_image,
                    .dst_image = task_swapchain_image,
                    .extent = {size_x, size_y, 1},
                });
                new_task_list.compile();

                return new_task_list;
            }

            ~App()
            {
                device.destroy_image(render_image);
            }

            bool update()
            {
                glfwPollEvents();
                if (glfwWindowShouldClose(glfw_window_ptr))
                {
                    return true;
                }

                if (!minimized)
                {
                    draw();
                }
                else
                {
                    using namespace std::literals;
                    std::this_thread::sleep_for(1ms);
                }

                return false;
            }

            void draw()
            {
                if (pipeline_compiler.check_if_sources_changed(raster_pipeline))
                {
                    auto new_pipeline = pipeline_compiler.recreate_raster_pipeline(raster_pipeline);
                    if (new_pipeline.is_ok())
                    {
                        raster_pipeline = new_pipeline.value();
                    }
                    else
                    {
                        std::cout << new_pipeline.message() << std::endl;
                    }
                }
                swapchain_image = swapchain.acquire_next_image();
                task_list.execute();
                auto command_lists = task_list.command_lists();
                auto cmd_list = device.create_command_list({});
                cmd_list.pipeline_barrier_image_transition({
                    .awaited_pipeline_access = task_list.last_access(task_swapchain_image),
                    .before_layout = task_list.last_layout(task_swapchain_image),
                    .after_layout = daxa::ImageLayout::PRESENT_SRC,
                    .image_id = swapchain_image,
                });
                cmd_list.complete();
                command_lists.push_back(cmd_list);
                auto binary_semaphore = device.create_binary_semaphore({});
                device.submit_commands({
                    .command_lists = command_lists,
                    .signal_binary_semaphores = {binary_semaphore},
                });
                device.present_frame({
                    .wait_binary_semaphores = {binary_semaphore},
                    .swapchain = swapchain,
                });
            }

            void on_mouse_move(f32, f32)
            {
            }

            void on_key(int, int)
            {
            }

            auto create_render_image(u32 sx, u32 sy) -> daxa::ImageId
            {
                return device.create_image(daxa::ImageInfo{
                    .format = swapchain.get_format(),
                    .size = {sx, sy, 1},
                    .usage = daxa::ImageUsageFlagBits::COLOR_ATTACHMENT | daxa::ImageUsageFlagBits::TRANSFER_SRC | daxa::ImageUsageFlagBits::TRANSFER_DST,
                    .debug_name = APPNAME_PREFIX("render_image (drawing)"),
                });
            }

            void on_resize(u32 sx, u32 sy)
            {
                size_x = sx;
                size_y = sy;
                minimized = (sx == 0 || sy == 0);

                if (!minimized)
                {
                    task_list = record_task_list();
                    device.destroy_image(render_image);
                    render_image = create_render_image(sx, sy);
                    swapchain.resize(size_x, size_y);
                    draw();
                }
            }
        };

        App app = {};
        while (true)
        {
            if (app.update())
                break;
        }
    }
} // namespace tests

int main()
{
    tests::simplest();
    tests::image_upload();
    tests::execution();
    tests::output_graph();
    // tests::compute();
    // tests::drawing();
}
