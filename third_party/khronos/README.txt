Khronos OpenGL API headers (GL/glcorearb.h, KHR/khrplatform.h), MIT-style licence in each file.
Only the macOS build uses them: the system has no GL/glcorearb.h. The runtime loads every GL entry point at run time
(runtime/glfn.h), so nothing links against OpenGL; on macOS the OpenGL renderer is compiled but never started
(native Vulkan through MoltenVK draws).
