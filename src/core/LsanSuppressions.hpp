#pragma once

// Include exactly once per executable (main translation unit). LeakSanitizer reads these suppressions
// automatically. They cover leaks inside third-party GPU drivers and system libraries that allocate
// process-lifetime state we cannot free (observed: NVIDIA GL driver during the RHI probe).
// Our own code is never suppressed.
#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(leak_sanitizer)
extern "C" const char* __lsan_default_suppressions();  // NOLINT(bugprone-reserved-identifier)
extern "C" const char* __lsan_default_suppressions()   // NOLINT(bugprone-reserved-identifier)
{
    return "leak:libnvidia-glcore.so\n"
           "leak:libnvidia-eglcore.so\n"
           "leak:libGLX_nvidia.so\n"
           "leak:libEGL_nvidia.so\n"
           "leak:libgallium\n"
           "leak:libdrm\n"
           "leak:libvulkan\n"
           "leak:libfontconfig.so\n"
           "leak:libdbus-1.so\n";
}
#endif
#endif
