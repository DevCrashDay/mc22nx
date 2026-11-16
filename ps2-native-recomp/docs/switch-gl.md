# Switch OpenGL 4.3 GS Backend — `wgpu*` Inventory & Mapping

Inventory of all `wgpu*` APIs used in `runtime/src/gs/gpu/gpu.cpp` and `host/window.cpp` (`PS2_GPU`).

## 1. Instance, Adapter, Device, Queue, Surface
- `wgpuCreateInstance`, `wgpuInstanceRelease`, `wgpuInstanceProcessEvents`, `wgpuInstanceRequestAdapter`, `wgpuInstanceCreateSurface`
- `wgpuAdapterGetInfo`, `wgpuAdapterInfoFreeMembers`, `wgpuAdapterGetLimits`, `wgpuAdapterHasFeature`, `wgpuAdapterRequestDevice`, `wgpuAdapterRelease`
- `wgpuDeviceGetQueue`, `wgpuDevicePoll`, `wgpuDeviceRelease`
- `wgpuSurfaceGetCapabilities`, `wgpuSurfaceCapabilitiesFreeMembers`, `wgpuSurfaceConfigure`, `wgpuSurfaceGetCurrentTexture`, `wgpuSurfacePresent`, `wgpuSurfaceRelease`
- **GL mapping**: SDL2 `SDL_GL_CreateContext` (PC: glad + GL 4.3 core; Switch: SDL2/EGL + GL 4.3 core via libnx Mesa), `SDL_GL_SetSwapInterval`, `SDL_GL_SwapWindow`, `glGetIntegerv` (`GL_MAX_SHADER_STORAGE_BLOCK_SIZE`, `GL_MAX_COMPUTE_WORK_GROUP_INVOCATIONS`, `GL_MAX_COMPUTE_WORK_GROUP_SIZE`).

## 2. Buffers & Readback
- `wgpuDeviceCreateBuffer`, `wgpuBufferRelease`
- `wgpuQueueWriteBuffer`
- `wgpuBufferMapAsync`, `wgpuBufferGetConstMappedRange`, `wgpuBufferUnmap`
- `wgpuCommandEncoderCopyBufferToBuffer`
- **GL mapping**: `glGenBuffers` / `glDeleteBuffers`, `glBindBuffer`, `glBufferData` (`GL_DYNAMIC_DRAW` / `GL_STREAM_READ`), `glBufferSubData`, `glCopyBufferSubData`, `glMapBufferRange` (`GL_MAP_READ_BIT`) + `glUnmapBuffer` (after `glFinish` or `glFenceSync`).

## 3. Textures & Views (Blit / Overlay / Readback)
- `wgpuDeviceCreateTexture`, `wgpuTextureRelease`
- `wgpuTextureCreateView`, `wgpuTextureViewRelease`
- `wgpuCommandEncoderCopyTextureToBuffer`
- **GL mapping**: `glGenTextures` / `glDeleteTextures`, `glTexStorage2D` (`GL_RGBA8`), `glBindFramebuffer` + `glReadPixels` (`GL_RGBA`, `GL_UNSIGNED_BYTE`) for offscreen PNG/PPM readback.

## 4. Shaders, Layouts, Bind Groups, Pipelines
- `wgpuDeviceCreateShaderModule`, `wgpuShaderModuleRelease`
- `wgpuDeviceCreateBindGroupLayout`, `wgpuBindGroupLayoutRelease`
- `wgpuDeviceCreatePipelineLayout`, `wgpuPipelineLayoutRelease`
- `wgpuDeviceCreateBindGroup`, `wgpuBindGroupRelease`
- `wgpuDeviceCreateComputePipeline` (with `WGPUConstantEntry` override constants in `gpu.cpp`)
- `wgpuDeviceCreateRenderPipeline` (fullscreen quad / `blit.wgsl` + optional overlay blend)
- **GL mapping**: WGSL → GLSL 430 via `naga`; override constants injected as `#define` prefix before `glCompileShader`; `glCreateProgram` + `glLinkProgram`; bind groups mapped to UBO/SSBO/texture binding indices (`glBindBufferBase(GL_SHADER_STORAGE_BUFFER / GL_UNIFORM_BUFFER, slot, buf)`).

## 5. Command Recording, Compute/Render Passes, Submit, Queries
- `wgpuDeviceCreateCommandEncoder`, `wgpuCommandEncoderFinish`, `wgpuCommandEncoderRelease`, `wgpuCommandBufferRelease`
- `wgpuCommandEncoderBeginComputePass`, `wgpuComputePassEncoderSetPipeline`, `wgpuComputePassEncoderSetBindGroup`, `wgpuComputePassEncoderDispatchWorkgroups`, `wgpuComputePassEncoderEnd`, `wgpuComputePassEncoderRelease`
- `wgpuCommandEncoderBeginRenderPass`, `wgpuRenderPassEncoderSetPipeline`, `wgpuRenderPassEncoderSetBindGroup`, `wgpuRenderPassEncoderSetViewport`, `wgpuRenderPassEncoderDraw`, `wgpuRenderPassEncoderEnd`, `wgpuRenderPassEncoderRelease`
- `wgpuQueueSubmit`, `wgpuQueueOnSubmittedWorkDone`
- `wgpuDeviceCreateQuerySet`, `wgpuCommandEncoderResolveQuerySet`
- **GL mapping**: Immediate-mode GL calls: `glUseProgram`, `glDispatchCompute`, `glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT)`, `glViewport`, `glDrawArrays(GL_TRIANGLES, 0, 3)` with empty VAO, `glFinish` for `wait_idle`; `GL_ARB_timer_query` (`glQueryCounter`) or off.
