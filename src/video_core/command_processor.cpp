// Copyright 2014 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <array>
#include <cstddef>
#include <future>
#include <memory>
#include <utility>
#include "common/assert.h"
#include "common/logging/log.h"
#include "common/thread_pool.h"
#include "common/vector_math.h"
#include "core/hle/service/gsp/gsp.h"
#include "core/hw/gpu.h"
#include "core/memory.h"
#include "core/settings.h"
#include "video_core/command_processor.h"
#include "video_core/debug_utils/debug_utils.h"
#include "video_core/pica_state.h"
#include "video_core/pica_types.h"
#include "video_core/primitive_assembly.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/regs.h"
#include "video_core/regs_pipeline.h"
#include "video_core/regs_texturing.h"
#include "video_core/renderer_base.h"
#include "video_core/shader/shader.h"
#include "video_core/vertex_loader.h"
#include "video_core/video_core.h"

namespace Pica::CommandProcessor {

// Expand a 4-bit mask to 4-byte mask, e.g. 0b0101 -> 0x00FF00FF
static const u32 expand_bits_to_bytes[] = {
    0x00000000, 0x000000ff, 0x0000ff00, 0x0000ffff, 0x00ff0000, 0x00ff00ff, 0x00ffff00, 0x00ffffff,
    0xff000000, 0xff0000ff, 0xff00ff00, 0xff00ffff, 0xffff0000, 0xffff00ff, 0xffffff00, 0xffffffff,
};

static const char* GetShaderSetupTypeName(Shader::ShaderSetup& setup) {
    if (&setup == &g_state.vs) {
        return "vertex shader";
    }
    if (&setup == &g_state.gs) {
        return "geometry shader";
    }
    return "unknown shader";
}

static void WriteUniformBoolReg(Shader::ShaderSetup& setup, u32 value) {
    for (unsigned i = 0; i < setup.uniforms.b.size(); ++i)
        setup.uniforms.b[i] = (value & (1 << i)) != 0;
}

static void WriteUniformIntReg(Shader::ShaderSetup& setup, unsigned index,
                               const Common::Vec4<u8>& values) {
    ASSERT(index < setup.uniforms.i.size());
    setup.uniforms.i[index] = values;
    LOG_TRACE(HW_GPU, "Set {} integer uniform {} to {:02x} {:02x} {:02x} {:02x}",
              GetShaderSetupTypeName(setup), index, values.x, values.y, values.z, values.w);
}

static void WriteUniformFloatReg(ShaderRegs& config, Shader::ShaderSetup& setup,
                                 int& float_regs_counter, u32 uniform_write_buffer[4], u32 value) {
    auto& uniform_setup = config.uniform_setup;

    // TODO: Does actual hardware indeed keep an intermediate buffer or does
    //       it directly write the values?
    uniform_write_buffer[float_regs_counter++] = value;

    // Uniforms are written in a packed format such that four float24 values are encoded in
    // three 32-bit numbers. We write to internal memory once a full such vector is
    // written.
    if ((float_regs_counter >= 4 && uniform_setup.IsFloat32()) ||
        (float_regs_counter >= 3 && !uniform_setup.IsFloat32())) {
        float_regs_counter = 0;

        auto& uniform = setup.uniforms.f[uniform_setup.index];

        if (uniform_setup.index >= 96) {
            LOG_ERROR(HW_GPU, "Invalid {} float uniform index {}", GetShaderSetupTypeName(setup),
                      (int)uniform_setup.index);
        } else {

            // NOTE: The destination component order indeed is "backwards"
            if (uniform_setup.IsFloat32()) {
                for (auto i : {0, 1, 2, 3})
                    uniform[3 - i] = float24::FromFloat32(*(float*)(&uniform_write_buffer[i]));
            } else {
                // TODO: Untested
                uniform.w = float24::FromRaw(uniform_write_buffer[0] >> 8);
                uniform.z = float24::FromRaw(((uniform_write_buffer[0] & 0xFF) << 16) |
                                             ((uniform_write_buffer[1] >> 16) & 0xFFFF));
                uniform.y = float24::FromRaw(((uniform_write_buffer[1] & 0xFFFF) << 8) |
                                             ((uniform_write_buffer[2] >> 24) & 0xFF));
                uniform.x = float24::FromRaw(uniform_write_buffer[2] & 0xFFFFFF);
            }

            LOG_TRACE(HW_GPU, "Set {} float uniform {:x} to ({} {} {} {})",
                      GetShaderSetupTypeName(setup), (int)uniform_setup.index,
                      uniform.x.ToFloat32(), uniform.y.ToFloat32(), uniform.z.ToFloat32(),
                      uniform.w.ToFloat32());

            // TODO: Verify that this actually modifies the register!
            uniform_setup.index.Assign(uniform_setup.index + 1);
        }
    }
}

static void WritePicaReg(u32 id, u32 value, u32 mask) {
    auto& regs = g_state.regs;

    if (id >= Regs::NUM_REGS) {
        LOG_ERROR(
            HW_GPU,
            "Commandlist tried to write to invalid register 0x{:03X} (value: {:08X}, mask: {:X})",
            id, value, mask);
        return;
    }

    // TODO: Figure out how register masking acts on e.g. vs.uniform_setup.set_value
    u32 old_value = regs.reg_array[id];

    const u32 write_mask = expand_bits_to_bytes[mask];

    regs.reg_array[id] = (old_value & ~write_mask) | (value & write_mask);

    // Double check for is_pica_tracing to avoid call overhead
    if (DebugUtils::IsPicaTracing()) {
        DebugUtils::OnPicaRegWrite({(u16)id, (u16)mask, regs.reg_array[id]});
    }

    if (g_debug_context)
        g_debug_context->OnEvent(DebugContext::Event::PicaCommandLoaded,
                                 reinterpret_cast<void*>(&id));

    switch (id) {
    // Trigger IRQ
    case PICA_REG_INDEX(trigger_irq):
        Service::GSP::SignalInterrupt(Service::GSP::InterruptId::P3D);
        break;

    case PICA_REG_INDEX(pipeline.triangle_topology):
        g_state.primitive_assembler.Reconfigure(regs.pipeline.triangle_topology);
        break;

    case PICA_REG_INDEX(pipeline.restart_primitive):
        g_state.primitive_assembler.Reset();
        break;

    case PICA_REG_INDEX(pipeline.vs_default_attributes_setup.index):
        g_state.immediate.current_attribute = 0;
        g_state.immediate.reset_geometry_pipeline = true;
        g_state.default_attr_counter = 0;
        break;

    // Load default vertex input attributes
    case PICA_REG_INDEX(pipeline.vs_default_attributes_setup.set_value[0]):
    case PICA_REG_INDEX(pipeline.vs_default_attributes_setup.set_value[1]):
    case PICA_REG_INDEX(pipeline.vs_default_attributes_setup.set_value[2]): {
        // TODO: Does actual hardware indeed keep an intermediate buffer or does
        //       it directly write the values?
        g_state.default_attr_write_buffer[g_state.default_attr_counter++] = value;

        // Default attributes are written in a packed format such that four float24 values are
        // encoded in
        // three 32-bit numbers. We write to internal memory once a full such vector is
        // written.
        if (g_state.default_attr_counter >= 3) {
            g_state.default_attr_counter = 0;

            auto& setup = regs.pipeline.vs_default_attributes_setup;

            if (setup.index >= 16) {
                LOG_ERROR(HW_GPU, "Invalid VS default attribute index {}", (int)setup.index);
                break;
            }

            Common::Vec4<float24> attribute;

            // NOTE: The destination component order indeed is "backwards"
            attribute.w = float24::FromRaw(g_state.default_attr_write_buffer[0] >> 8);
            attribute.z = float24::FromRaw(((g_state.default_attr_write_buffer[0] & 0xFF) << 16) |
                                           ((g_state.default_attr_write_buffer[1] >> 16) & 0xFFFF));
            attribute.y = float24::FromRaw(((g_state.default_attr_write_buffer[1] & 0xFFFF) << 8) |
                                           ((g_state.default_attr_write_buffer[2] >> 24) & 0xFF));
            attribute.x = float24::FromRaw(g_state.default_attr_write_buffer[2] & 0xFFFFFF);

            LOG_TRACE(HW_GPU, "Set default VS attribute {:x} to ({} {} {} {})", (int)setup.index,
                      attribute.x.ToFloat32(), attribute.y.ToFloat32(), attribute.z.ToFloat32(),
                      attribute.w.ToFloat32());

            // TODO: Verify that this actually modifies the register!
            if (setup.index < 15) {
                g_state.input_default_attributes.attr[setup.index] = attribute;
                setup.index++;
            } else {
                // Put each attribute into an immediate input buffer.  When all specified immediate
                // attributes are present, the Vertex Shader is invoked and everything is sent to
                // the primitive assembler.

                auto& immediate_input = g_state.immediate.input_vertex;
                auto& immediate_attribute_id = g_state.immediate.current_attribute;

                immediate_input.attr[immediate_attribute_id] = attribute;

                if (immediate_attribute_id < regs.pipeline.max_input_attrib_index) {
                    immediate_attribute_id += 1;
                } else {
                    immediate_attribute_id = 0;

                    Shader::OutputVertex::ValidateSemantics(regs.rasterizer);

                    auto* shader_engine = Shader::GetEngine();
                    shader_engine->SetupBatch(g_state.vs, regs.vs.main_offset);

                    // Send to vertex shader
                    if (g_debug_context)
                        g_debug_context->OnEvent(DebugContext::Event::VertexShaderInvocation,
                                                 static_cast<void*>(&immediate_input));
                    Shader::UnitState shader_unit;
                    Shader::AttributeBuffer output{};

                    shader_unit.LoadInput(regs.vs, immediate_input);
                    shader_engine->Run(g_state.vs, shader_unit);
                    shader_unit.WriteOutput(regs.vs, output);

                    // Send to geometry pipeline
                    if (g_state.immediate.reset_geometry_pipeline) {
                        g_state.geometry_pipeline.Reconfigure();
                        g_state.immediate.reset_geometry_pipeline = false;
                    }
                    ASSERT(!g_state.geometry_pipeline.NeedIndexInput());
                    g_state.geometry_pipeline.Setup(shader_engine);
                    g_state.geometry_pipeline.SubmitVertex(output);

                    // TODO: If drawing after every immediate mode triangle kills performance,
                    // change it to flush triangles whenever a drawing config register changes
                    // See: https://github.com/citra-emu/citra/pull/2866#issuecomment-327011550
                    VideoCore::g_renderer->Rasterizer()->DrawTriangles();
                    if (g_debug_context) {
                        g_debug_context->OnEvent(DebugContext::Event::FinishedPrimitiveBatch,
                                                 nullptr);
                    }
                }
            }
        }
        break;
    }

    case PICA_REG_INDEX(pipeline.gpu_mode):
        // This register likely just enables vertex processing and doesn't need any special handling
        break;

    case PICA_REG_INDEX(pipeline.command_buffer.trigger[0]):
    case PICA_REG_INDEX(pipeline.command_buffer.trigger[1]): {
        unsigned index =
            static_cast<unsigned>(id - PICA_REG_INDEX(pipeline.command_buffer.trigger[0]));
        u32* head_ptr = (u32*)VideoCore::g_memory->GetPhysicalPointer(
            regs.pipeline.command_buffer.GetPhysicalAddress(index));
        g_state.cmd_list.head_ptr = g_state.cmd_list.current_ptr = head_ptr;
        g_state.cmd_list.length = regs.pipeline.command_buffer.GetSize(index) / sizeof(u32);
        break;
    }

    // It seems like these trigger vertex rendering
    case PICA_REG_INDEX(pipeline.trigger_draw):
    case PICA_REG_INDEX(pipeline.trigger_draw_indexed): {
#if PICA_LOG_TEV
        DebugUtils::DumpTevStageConfig(regs.GetTevStages());
#endif
        if (g_debug_context)
            g_debug_context->OnEvent(DebugContext::Event::IncomingPrimitiveBatch, nullptr);

        PrimitiveAssembler<Shader::OutputVertex>& primitive_assembler = g_state.primitive_assembler;

        bool accelerate_draw = VideoCore::g_hw_shader_enabled && primitive_assembler.IsEmpty();

        if (regs.pipeline.use_gs == PipelineRegs::UseGS::No) {
            auto topology = primitive_assembler.GetTopology();
            if (topology == PipelineRegs::TriangleTopology::Shader ||
                topology == PipelineRegs::TriangleTopology::List) {
                accelerate_draw = accelerate_draw && (regs.pipeline.num_vertices % 3) == 0;
            }
            // TODO (wwylele): for Strip/Fan topology, if the primitive assember is not restarted
            // after this draw call, the buffered vertex from this draw should "leak" to the next
            // draw, in which case we should buffer the vertex into the software primitive assember,
            // or disable accelerate draw completely. However, there is not game found yet that does
            // this, so this is left unimplemented for now. Revisit this when an issue is found in
            // games.
        } else {
            accelerate_draw = false;
        }

        const bool is_indexed = (id == PICA_REG_INDEX(pipeline.trigger_draw_indexed));

        if (accelerate_draw &&
            VideoCore::g_renderer->Rasterizer()->AccelerateDrawBatch(is_indexed)) {
            if (g_debug_context) {
                g_debug_context->OnEvent(DebugContext::Event::FinishedPrimitiveBatch, nullptr);
            }
            break;
        }

        // Processes information about internal vertex attributes to figure out how a vertex is
        // loaded. Later, these can be compiled and cached.
        const u32 base_address = regs.pipeline.vertex_attributes.GetPhysicalBaseAddress();
        VertexLoader loader(regs.pipeline);
        Shader::OutputVertex::ValidateSemantics(regs.rasterizer);

        // Multithreaded vertex cache. Each thread will lock the vertex that its processing and add
        // the data to that batch
        struct CachedVertex {
            CachedVertex() {}
            explicit CachedVertex(const CachedVertex&) {}
            union {
                Shader::AttributeBuffer output_attr; // GS used
                Shader::OutputVertex output_vertex;  // No GS
            };
            std::atomic<u32> batch{0};
            std::atomic_flag lock ATOMIC_FLAG_INIT;
        };
        static std::vector<CachedVertex> vs_output(0x10000);

        if (!is_indexed && vs_output.size() < regs.pipeline.num_vertices) {
            vs_output.resize(regs.pipeline.num_vertices);
        }

        // Used to invalidate data from the previous batch without clearing it
        static u32 batch_id = std::numeric_limits<u32>::max();
        ++batch_id;
        // reset cache when id overflows for safety
        if (batch_id == 0) {
            ++batch_id;
            for (auto& entry : vs_output)
                entry.batch = 0;
        }

        // Load vertices
        const auto& index_info = regs.pipeline.index_array;
        const u8* index_address_8 =
            VideoCore::g_memory->GetPhysicalPointer(base_address + index_info.offset);
        const u16* index_address_16 = reinterpret_cast<const u16*>(index_address_8);
        bool index_u16 = index_info.format != 0;

        const auto VertexIndex = [&](unsigned int index) {
            // Indexed rendering doesn't use the start offset
            return is_indexed ? (index_u16 ? index_address_16[index] : index_address_8[index])
                              : (index + regs.pipeline.vertex_offset);
        };

        Pica::Shader::ShaderEngine* shader_engine = Shader::GetEngine();
        Shader::UnitState shader_unit;

        shader_engine->SetupBatch(g_state.vs, regs.vs.main_offset);

        const bool use_gs = regs.pipeline.use_gs == PipelineRegs::UseGS::Yes;

        const auto VSUnitLoop = [&](u32 thread_id, auto num_threads) {
            constexpr bool single_thread =
                std::is_same<std::integral_constant<u32, 1>, decltype(num_threads)>();
            Shader::UnitState shader_unit;

            for (unsigned int index = thread_id; index < regs.pipeline.num_vertices;
                 index += num_threads) {
                const unsigned int vertex = VertexIndex(index);
                CachedVertex& cached_vertex = vs_output[is_indexed ? vertex : index];

                if (is_indexed) {
                    if (!single_thread) {
                        // Try locking this vertex
                        if (cached_vertex.lock.test_and_set(std::memory_order_acquire)) {
                            // Another thread is processing this vertex
                            continue;
                        } else if (cached_vertex.batch.load(std::memory_order_acquire) ==
                                   batch_id) {
                            // Vertex is not being processed and is from the correct batch so unlock
                            cached_vertex.lock.clear(std::memory_order_release);
                            continue;
                        }
                    } else if (cached_vertex.batch.load(std::memory_order_relaxed) == batch_id) {
                        continue;
                    }
                }

                Shader::AttributeBuffer attribute_buffer;
                Shader::AttributeBuffer& output_attr =
                    use_gs ? cached_vertex.output_attr : attribute_buffer;

                // Initialize data for the current vertex
                loader.LoadVertex(base_address, index, vertex, attribute_buffer);

                // Send to vertex shader
                if (g_debug_context) {
                    g_debug_context->OnEvent(DebugContext::Event::VertexShaderInvocation,
                                             &attribute_buffer);
                }
                shader_unit.LoadInput(regs.vs, attribute_buffer);
                shader_engine->Run(g_state.vs, shader_unit);
                shader_unit.WriteOutput(regs.vs, output_attr);
                if (!use_gs) {
                    cached_vertex.output_vertex =
                        Shader::OutputVertex::FromAttributeBuffer(regs.rasterizer, output_attr);
                }
                if (!single_thread) {
                    cached_vertex.batch.store(batch_id, std::memory_order_release);

                    if (is_indexed) {
                        cached_vertex.lock.clear(std::memory_order_release);
                    }
                } else if (is_indexed) {
                    cached_vertex.batch.store(batch_id, std::memory_order_relaxed);
                }
            }
        };

        Common::ThreadPool& thread_pool = Common::ThreadPool::GetPool();
        std::vector<std::future<void>> futures;

        const u32 vs_threads =
            std::min(regs.pipeline.num_vertices / Settings::values.min_vertices_per_thread,
                     std::thread::hardware_concurrency() - 1);

        if (!vs_threads) {
            VSUnitLoop(0, std::integral_constant<u32, 1>{});
        } else {
            for (unsigned int thread_id = 0; thread_id < vs_threads; ++thread_id) {
                futures.emplace_back(thread_pool.Push(VSUnitLoop, thread_id, vs_threads));
            }
        }

        g_state.geometry_pipeline.Reconfigure();
        g_state.geometry_pipeline.Setup(shader_engine);
        if (g_state.geometry_pipeline.NeedIndexInput()) {
            ASSERT(is_indexed);
        }

        const auto AddTriangle =
            [rasterizer = VideoCore::g_renderer->Rasterizer()](
                const Pica::Shader::OutputVertex& v0, const Pica::Shader::OutputVertex& v1,
                const Pica::Shader::OutputVertex& v2) { rasterizer->AddTriangle(v0, v1, v2); };

        for (u32 index = 0; index < regs.pipeline.num_vertices; ++index) {
            const u32 vertex = VertexIndex(index);
            CachedVertex& cached_vertex = vs_output[is_indexed ? vertex : index];

            if (use_gs && is_indexed && g_state.geometry_pipeline.NeedIndexInput()) {
                g_state.geometry_pipeline.SubmitIndex(vertex);
                continue;
            }

            // Synchronize threads
            if (vs_threads) {
                while (cached_vertex.batch.load(std::memory_order_acquire) != batch_id) {
                    std::this_thread::yield();
                }
            }

            if (use_gs) {
                // Send to geometry pipeline
                g_state.geometry_pipeline.SubmitVertex(cached_vertex.output_attr);
            } else {
                primitive_assembler.SubmitVertex(cached_vertex.output_vertex, AddTriangle);
            }
        }

        for (std::future<void>& future : futures) {
            future.get();
        }

        VideoCore::g_renderer->Rasterizer()->DrawTriangles();
        if (g_debug_context) {
            g_debug_context->OnEvent(DebugContext::Event::FinishedPrimitiveBatch, nullptr);
        }

        break;
    }

    case PICA_REG_INDEX(gs.bool_uniforms):
        WriteUniformBoolReg(g_state.gs, g_state.regs.gs.bool_uniforms.Value());
        break;

    case PICA_REG_INDEX(gs.int_uniforms[0]):
    case PICA_REG_INDEX(gs.int_uniforms[1]):
    case PICA_REG_INDEX(gs.int_uniforms[2]):
    case PICA_REG_INDEX(gs.int_uniforms[3]): {
        const unsigned index = (id - PICA_REG_INDEX(gs.int_uniforms[0]));
        const auto values = regs.gs.int_uniforms[index];
        WriteUniformIntReg(g_state.gs, index,
                           Common::Vec4<u8>(values.x, values.y, values.z, values.w));
        break;
    }

    case PICA_REG_INDEX(gs.uniform_setup.set_value[0]):
    case PICA_REG_INDEX(gs.uniform_setup.set_value[1]):
    case PICA_REG_INDEX(gs.uniform_setup.set_value[2]):
    case PICA_REG_INDEX(gs.uniform_setup.set_value[3]):
    case PICA_REG_INDEX(gs.uniform_setup.set_value[4]):
    case PICA_REG_INDEX(gs.uniform_setup.set_value[5]):
    case PICA_REG_INDEX(gs.uniform_setup.set_value[6]):
    case PICA_REG_INDEX(gs.uniform_setup.set_value[7]): {
        WriteUniformFloatReg(g_state.regs.gs, g_state.gs, g_state.gs_float_regs_counter,
                             g_state.gs_uniform_write_buffer, value);
        break;
    }

    case PICA_REG_INDEX(gs.program.set_word[0]):
    case PICA_REG_INDEX(gs.program.set_word[1]):
    case PICA_REG_INDEX(gs.program.set_word[2]):
    case PICA_REG_INDEX(gs.program.set_word[3]):
    case PICA_REG_INDEX(gs.program.set_word[4]):
    case PICA_REG_INDEX(gs.program.set_word[5]):
    case PICA_REG_INDEX(gs.program.set_word[6]):
    case PICA_REG_INDEX(gs.program.set_word[7]): {
        u32& offset = g_state.regs.gs.program.offset;
        if (offset >= 4096) {
            LOG_ERROR(HW_GPU, "Invalid GS program offset {}", offset);
        } else {
            g_state.gs.program_code[offset] = value;
            g_state.gs.MarkProgramCodeDirty();
            offset++;
        }
        break;
    }

    case PICA_REG_INDEX(gs.swizzle_patterns.set_word[0]):
    case PICA_REG_INDEX(gs.swizzle_patterns.set_word[1]):
    case PICA_REG_INDEX(gs.swizzle_patterns.set_word[2]):
    case PICA_REG_INDEX(gs.swizzle_patterns.set_word[3]):
    case PICA_REG_INDEX(gs.swizzle_patterns.set_word[4]):
    case PICA_REG_INDEX(gs.swizzle_patterns.set_word[5]):
    case PICA_REG_INDEX(gs.swizzle_patterns.set_word[6]):
    case PICA_REG_INDEX(gs.swizzle_patterns.set_word[7]): {
        u32& offset = g_state.regs.gs.swizzle_patterns.offset;
        if (offset >= g_state.gs.swizzle_data.size()) {
            LOG_ERROR(HW_GPU, "Invalid GS swizzle pattern offset {}", offset);
        } else {
            g_state.gs.swizzle_data[offset] = value;
            g_state.gs.MarkSwizzleDataDirty();
            offset++;
        }
        break;
    }

    case PICA_REG_INDEX(vs.bool_uniforms):
        // TODO (wwylele): does regs.pipeline.gs_unit_exclusive_configuration affect this?
        WriteUniformBoolReg(g_state.vs, g_state.regs.vs.bool_uniforms.Value());
        break;

    case PICA_REG_INDEX(vs.int_uniforms[0]):
    case PICA_REG_INDEX(vs.int_uniforms[1]):
    case PICA_REG_INDEX(vs.int_uniforms[2]):
    case PICA_REG_INDEX(vs.int_uniforms[3]): {
        // TODO (wwylele): does regs.pipeline.gs_unit_exclusive_configuration affect this?
        unsigned index = (id - PICA_REG_INDEX(vs.int_uniforms[0]));
        auto values = regs.vs.int_uniforms[index];
        WriteUniformIntReg(g_state.vs, index,
                           Common::Vec4<u8>(values.x, values.y, values.z, values.w));
        break;
    }

    case PICA_REG_INDEX(vs.uniform_setup.set_value[0]):
    case PICA_REG_INDEX(vs.uniform_setup.set_value[1]):
    case PICA_REG_INDEX(vs.uniform_setup.set_value[2]):
    case PICA_REG_INDEX(vs.uniform_setup.set_value[3]):
    case PICA_REG_INDEX(vs.uniform_setup.set_value[4]):
    case PICA_REG_INDEX(vs.uniform_setup.set_value[5]):
    case PICA_REG_INDEX(vs.uniform_setup.set_value[6]):
    case PICA_REG_INDEX(vs.uniform_setup.set_value[7]): {
        // TODO (wwylele): does regs.pipeline.gs_unit_exclusive_configuration affect this?
        WriteUniformFloatReg(g_state.regs.vs, g_state.vs, g_state.vs_float_regs_counter,
                             g_state.vs_uniform_write_buffer, value);
        break;
    }

    case PICA_REG_INDEX(vs.program.set_word[0]):
    case PICA_REG_INDEX(vs.program.set_word[1]):
    case PICA_REG_INDEX(vs.program.set_word[2]):
    case PICA_REG_INDEX(vs.program.set_word[3]):
    case PICA_REG_INDEX(vs.program.set_word[4]):
    case PICA_REG_INDEX(vs.program.set_word[5]):
    case PICA_REG_INDEX(vs.program.set_word[6]):
    case PICA_REG_INDEX(vs.program.set_word[7]): {
        u32& offset = g_state.regs.vs.program.offset;
        if (offset >= 512) {
            LOG_ERROR(HW_GPU, "Invalid VS program offset {}", offset);
        } else {
            g_state.vs.program_code[offset] = value;
            g_state.vs.MarkProgramCodeDirty();
            if (!g_state.regs.pipeline.gs_unit_exclusive_configuration) {
                g_state.gs.program_code[offset] = value;
                g_state.gs.MarkProgramCodeDirty();
            }
            offset++;
        }
        break;
    }

    case PICA_REG_INDEX(vs.swizzle_patterns.set_word[0]):
    case PICA_REG_INDEX(vs.swizzle_patterns.set_word[1]):
    case PICA_REG_INDEX(vs.swizzle_patterns.set_word[2]):
    case PICA_REG_INDEX(vs.swizzle_patterns.set_word[3]):
    case PICA_REG_INDEX(vs.swizzle_patterns.set_word[4]):
    case PICA_REG_INDEX(vs.swizzle_patterns.set_word[5]):
    case PICA_REG_INDEX(vs.swizzle_patterns.set_word[6]):
    case PICA_REG_INDEX(vs.swizzle_patterns.set_word[7]): {
        u32& offset = g_state.regs.vs.swizzle_patterns.offset;
        if (offset >= g_state.vs.swizzle_data.size()) {
            LOG_ERROR(HW_GPU, "Invalid VS swizzle pattern offset {}", offset);
        } else {
            g_state.vs.swizzle_data[offset] = value;
            g_state.vs.MarkSwizzleDataDirty();
            if (!g_state.regs.pipeline.gs_unit_exclusive_configuration) {
                g_state.gs.swizzle_data[offset] = value;
                g_state.gs.MarkSwizzleDataDirty();
            }
            offset++;
        }
        break;
    }

    case PICA_REG_INDEX(lighting.lut_data[0]):
    case PICA_REG_INDEX(lighting.lut_data[1]):
    case PICA_REG_INDEX(lighting.lut_data[2]):
    case PICA_REG_INDEX(lighting.lut_data[3]):
    case PICA_REG_INDEX(lighting.lut_data[4]):
    case PICA_REG_INDEX(lighting.lut_data[5]):
    case PICA_REG_INDEX(lighting.lut_data[6]):
    case PICA_REG_INDEX(lighting.lut_data[7]): {
        auto& lut_config = regs.lighting.lut_config;

        ASSERT_MSG(lut_config.index < 256, "lut_config.index exceeded maximum value of 255!");

        g_state.lighting.luts[lut_config.type][lut_config.index].raw = value;
        lut_config.index.Assign(lut_config.index + 1);
        break;
    }

    case PICA_REG_INDEX(texturing.fog_lut_data[0]):
    case PICA_REG_INDEX(texturing.fog_lut_data[1]):
    case PICA_REG_INDEX(texturing.fog_lut_data[2]):
    case PICA_REG_INDEX(texturing.fog_lut_data[3]):
    case PICA_REG_INDEX(texturing.fog_lut_data[4]):
    case PICA_REG_INDEX(texturing.fog_lut_data[5]):
    case PICA_REG_INDEX(texturing.fog_lut_data[6]):
    case PICA_REG_INDEX(texturing.fog_lut_data[7]): {
        g_state.fog.lut[regs.texturing.fog_lut_offset % 128].raw = value;
        regs.texturing.fog_lut_offset.Assign(regs.texturing.fog_lut_offset + 1);
        break;
    }

    case PICA_REG_INDEX(texturing.proctex_lut_data[0]):
    case PICA_REG_INDEX(texturing.proctex_lut_data[1]):
    case PICA_REG_INDEX(texturing.proctex_lut_data[2]):
    case PICA_REG_INDEX(texturing.proctex_lut_data[3]):
    case PICA_REG_INDEX(texturing.proctex_lut_data[4]):
    case PICA_REG_INDEX(texturing.proctex_lut_data[5]):
    case PICA_REG_INDEX(texturing.proctex_lut_data[6]):
    case PICA_REG_INDEX(texturing.proctex_lut_data[7]): {
        auto& index = regs.texturing.proctex_lut_config.index;
        auto& pt = g_state.proctex;

        switch (regs.texturing.proctex_lut_config.ref_table.Value()) {
        case TexturingRegs::ProcTexLutTable::Noise:
            pt.noise_table[index % pt.noise_table.size()].raw = value;
            break;
        case TexturingRegs::ProcTexLutTable::ColorMap:
            pt.color_map_table[index % pt.color_map_table.size()].raw = value;
            break;
        case TexturingRegs::ProcTexLutTable::AlphaMap:
            pt.alpha_map_table[index % pt.alpha_map_table.size()].raw = value;
            break;
        case TexturingRegs::ProcTexLutTable::Color:
            pt.color_table[index % pt.color_table.size()].raw = value;
            break;
        case TexturingRegs::ProcTexLutTable::ColorDiff:
            pt.color_diff_table[index % pt.color_diff_table.size()].raw = value;
            break;
        }
        index.Assign(index + 1);
        break;
    }
    default:
        break;
    }

    VideoCore::g_renderer->Rasterizer()->NotifyPicaRegisterChanged(id);

    if (g_debug_context)
        g_debug_context->OnEvent(DebugContext::Event::PicaCommandProcessed,
                                 reinterpret_cast<void*>(&id));
} // namespace CommandProcessor

void ProcessCommandList(const u32* list, u32 size) {
    g_state.cmd_list.head_ptr = g_state.cmd_list.current_ptr = list;
    g_state.cmd_list.length = size / sizeof(u32);

    while (g_state.cmd_list.current_ptr < g_state.cmd_list.head_ptr + g_state.cmd_list.length) {

        // Align read pointer to 8 bytes
        if ((g_state.cmd_list.head_ptr - g_state.cmd_list.current_ptr) % 2 != 0)
            ++g_state.cmd_list.current_ptr;

        u32 value = *g_state.cmd_list.current_ptr++;
        const CommandHeader header = {*g_state.cmd_list.current_ptr++};

        WritePicaReg(header.cmd_id, value, header.parameter_mask);

        for (unsigned i = 0; i < header.extra_data_length; ++i) {
            u32 cmd = header.cmd_id + (header.group_commands ? i + 1 : 0);
            WritePicaReg(cmd, *g_state.cmd_list.current_ptr++, header.parameter_mask);
        }
    }
}

} // namespace Pica::CommandProcessor
