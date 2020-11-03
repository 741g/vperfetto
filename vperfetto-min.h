// Copyright 2020 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef VPERFETTO_EXPORT
    #ifdef _MSC_VER
        #define VPERFETTO_EXPORT extern "C" __declspec(dllexport)
    #else // _MSC_VER
        #ifdef __cplusplus
            #define VPERFETTO_EXPORT extern "C" __attribute__((visibility("default")))
        #else
            #define VPERFETTO_EXPORT __attribute__((visibility("default")))
        #endif
    #endif // !_MSC_VER
#endif // !VPERFETTO_EXPORT

// Categories that vperfetto_min is capable of tracking.
#define VPERFETTO_LIST_CATEGORIES(f) \
    f(OpenGL, "OpenGL(ES) calls") \
    f(Vulkan, "Vulkan calls") \
    f(EGL, "EGL calls") \
    f(Driver, "Driver internals") \
    f(VMM, "VMM internals") \
    f(gfx, "General graphics events that don't fall under the above categories") \

// Start tracing. This is meant to be triggered when tracing starts in the guest. Use your favorite transport,
// virtio-gpu, pipe, virtual perfetto, etc etc. Just somehow wire it up :)
enum vperfetto_init_flags {
    VPERFETTO_INIT_FLAG_USE_INPROCESS_BACKEND = 1 << 0,
    VPERFETTO_INIT_FLAG_USE_SYSTEM_BACKEND = 1 << 1,
};

struct vperfetto_min_config {
    enum vperfetto_init_flags init_flags;
    const char* filename;
};

VPERFETTO_EXPORT void vperfetto_min_startTracing(const struct vperfetto_min_config* config);

// End tracing. This is meant to be triggerd when tracing ends in the guest. Again, use your favorite transport.
// This will also trigger trace saving. It is assumed that at around roughly this time, the host/guest also send over the finished trace from the guest to the host to the path specified in VPERFETTO_GUEST_FILE or traceconfig.guestFilename, such as via `adb pull /data/local/traces/guestfile.trace`.
// After waiting for a while, the guest/host traces are post processed and catted together into VPERFETTO_COMBINED_FILE.
VPERFETTO_EXPORT void vperfetto_min_endTracing();

// Start/end a particular track event on the host. By default, every such event is in the 'gfx' category.
VPERFETTO_EXPORT void vperfetto_min_beginTrackEvent(const char* eventName);
VPERFETTO_EXPORT void vperfetto_min_endTrackEvent();

// Start/end a particular track event in a particular category.
#define DEFINE_CATEGORY_TRACK_EVENT_DECLARATION(name, desc) \
    VPERFETTO_EXPORT void vperfetto_min_beginTrackEvent_##name(const char* eventName); \
    VPERFETTO_EXPORT void vperfetto_min_endTrackEvent_##name(); \

VPERFETTO_LIST_CATEGORIES(DEFINE_CATEGORY_TRACK_EVENT_DECLARATION)
