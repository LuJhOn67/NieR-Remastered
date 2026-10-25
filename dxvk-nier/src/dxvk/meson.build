dxvk_shaders = files([
  'shaders/dxvk_cursor_vert.vert',
  'shaders/dxvk_cursor_frag.frag',

  'shaders/dxvk_dummy_frag.frag',

  'shaders/dxvk_mipgen.comp',
  'shaders/dxvk_nier_mvec.comp',
  'shaders/dxvk_nier_rtao.comp',

  'shaders/dxvk_present_frag.frag',
  'shaders/dxvk_present_frag_blit.frag',
  'shaders/dxvk_present_frag_ms.frag',
  'shaders/dxvk_present_frag_ms_blit.frag',
  'shaders/dxvk_present_vert.vert',

  'hud/shaders/hud_chunk_frag_background.frag',
  'hud/shaders/hud_chunk_frag_visualize.frag',
  'hud/shaders/hud_chunk_vert_background.vert',
  'hud/shaders/hud_chunk_vert_visualize.vert',

  'hud/shaders/hud_frame_time_eval.comp',

  'hud/shaders/hud_graph_frag.frag',
  'hud/shaders/hud_graph_vert.vert',

  'hud/shaders/hud_text_frag.frag',
  'hud/shaders/hud_text_vert.vert',
])

dxvk_src = [
  'dxvk_access.cpp',
  'dxvk_adapter.cpp',
  'dxvk_allocator.cpp',
  'dxvk_barrier.cpp',
  'dxvk_buffer.cpp',
  'dxvk_cmdlist.cpp',
  'dxvk_compute.cpp',
  'dxvk_constant_state.cpp',
  'dxvk_context.cpp',
  'dxvk_cs.cpp',
  'dxvk_descriptor_heap.cpp',
  'dxvk_descriptor_info.cpp',
  'dxvk_descriptor_pool.cpp',
  'dxvk_descriptor_worker.cpp',
  'dxvk_device.cpp',
  'dxvk_device_filter.cpp',
  'dxvk_device_info.cpp',
  'dxvk_fence.cpp',
  'dxvk_format.cpp',
  'dxvk_framebuffer.cpp',
  'dxvk_gpu_event.cpp',
  'dxvk_gpu_query.cpp',
  'dxvk_graphics.cpp',
  'dxvk_hang.cpp',
  'dxvk_image.cpp',
  'dxvk_implicit_resolve.cpp',
  'dxvk_instance.cpp',
  'dxvk_latency_builtin.cpp',
  'dxvk_latency_reflex.cpp',
  'dxvk_memory.cpp',
  'dxvk_meta_blit.cpp',
  'dxvk_meta_clear.cpp',
  'dxvk_meta_copy.cpp',
  'dxvk_meta_mipgen.cpp',
  'dxvk_meta_resolve.cpp',
  'dxvk_nier_dlss.cpp',
  'dxvk_nier_rt.cpp',
  'dxvk_nier_settings.cpp',
  'dxvk_options.cpp',
  'dxvk_pipelayout.cpp',
  'dxvk_pipemanager.cpp',
  'dxvk_platform_exts.cpp',
  'dxvk_presenter.cpp',
  'dxvk_queue.cpp',
  'dxvk_sampler.cpp',
  'dxvk_shader.cpp',
  'dxvk_shader_builtin.cpp',
  'dxvk_shader_cache.cpp',
  'dxvk_shader_io.cpp',
  'dxvk_shader_ir.cpp',
  'dxvk_shader_key.cpp',
  'dxvk_shader_spirv.cpp',
  'dxvk_signal.cpp',
  'dxvk_sparse.cpp',
  'dxvk_staging.cpp',
  'dxvk_stats.cpp',
  'dxvk_swapchain_blitter.cpp',
  'dxvk_unbound.cpp',
  'dxvk_util.cpp',

  'hud/dxvk_hud.cpp',
  'hud/dxvk_hud_font.cpp',
  'hud/dxvk_hud_item.cpp',
  'hud/dxvk_hud_renderer.cpp',
]

if platform == 'windows'
  dxvk_src += [
    'dxvk_openvr.cpp',
    'dxvk_openxr.cpp',
  ]
endif

dxvk_extra_deps = [ dependency('threads') ]
if platform == 'linux'
  dxvk_extra_deps += [ cpp.find_library('dl', required: false) ]
endif

# NieR mod: DLSS through the NVIDIA NGX SDK (tools/dlss-sdk), only if present
ngx_sdk_dir = import('fs').parent(meson.project_source_root()) / 'tools' / 'dlss-sdk'
ngx_lib = cpp.find_library('nvsdk_ngx_s',
  dirs     : [ ngx_sdk_dir / 'lib' / 'Windows_x86_64' / 'x64' ],
  required : false)

if platform == 'windows' and ngx_lib.found()
  dxvk_src += [ 'dxvk_nier_ngx.cpp' ]
  dxvk_extra_deps += [ declare_dependency(
    dependencies        : [ ngx_lib ],
    include_directories : include_directories(ngx_sdk_dir / 'include', is_system : true),
    compile_args        : [ '-DDXVK_NIER_NGX=1' ],
  ) ]
endif

dxvk_lib = static_library('dxvk', dxvk_src, glsl_generator.process(dxvk_shaders), dxvk_version,
  link_with           : [ util_lib, spirv_lib, wsi_lib ],
  dependencies        : [ dxbc_spirv_dep, vkcommon_dep ] + dxvk_extra_deps,
  include_directories : [ dxvk_include_path ],
)

dxvk_dep = declare_dependency(
  link_with           : [ dxvk_lib ],
  include_directories : [ dxvk_include_path ],
)
