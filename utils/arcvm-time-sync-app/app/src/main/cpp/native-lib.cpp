// Copyright 2021 The Android Open Source Project
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
#include <jni.h>
#include <string>
#include <thread>
#include <sys/types.h>
#include <time.h>

#if defined(_M_IA64) || defined(_M_IX86) || defined(__ia64__) ||      \
    defined(__i386__) || defined(__amd64__) || defined(__x86_64__) || \
    defined(_M_AMD64)
#define HAS_RDTSC
#ifdef _MSC_VER
#include <intrin.h>
#else
#include <x86intrin.h>
#endif
#endif
#if defined(__aarch64__)
#define HAS_CNTCVT
#endif

#include "perfetto.h"

PERFETTO_DEFINE_CATEGORIES(
    perfetto::Category("cros")
        .SetDescription("Chrome OS guest time sync events"));

PERFETTO_TRACK_EVENT_STATIC_STORAGE();

static std::once_flag perfetto_once;
static std::thread* bg_thread = nullptr;

static inline uint64_t get_cpu_ticks() {
#if defined(HAS_RDTSC)
    return __rdtsc();
#elif defined(HAS_CNTCVT)
    uint64_t vct;
    asm volatile("mrs %0, cntvct_el0" : "=r"(vct));
    return vct;
#else
    return 0;
#endif
}

static inline uint64_t get_timestamp_ns(clockid_t cid) {
    struct timespec ts = {};
    clock_gettime(cid, &ts);
    return static_cast<uint64_t>(ts.tv_sec * 1000000000LL + ts.tv_nsec);
}

void perfetto_annotate_time_sync(const perfetto::EventContext& perfetto) {
    uint64_t boot_time = get_timestamp_ns(CLOCK_BOOTTIME);
    uint64_t cpu_time = get_cpu_ticks();
    uint64_t monotonic_time = get_timestamp_ns(CLOCK_MONOTONIC);
    // Read again to avoid cache miss overhead.
    boot_time = get_timestamp_ns(CLOCK_BOOTTIME);
    cpu_time = get_cpu_ticks();
    monotonic_time = get_timestamp_ns(CLOCK_MONOTONIC);

    auto* dbg = perfetto.event()->add_debug_annotations();
    dbg->set_name("clock_sync_boottime");
    dbg->set_uint_value(boot_time);
    dbg = perfetto.event()->add_debug_annotations();
    dbg->set_name("clock_sync_monotonic");
    dbg->set_uint_value(monotonic_time);
    dbg = perfetto.event()->add_debug_annotations();
    dbg->set_name("clock_sync_cputime");
    dbg->set_uint_value(cpu_time);
}

void tick_forever() {
    for(;;) {
        usleep(100000);
        TRACE_EVENT(
                "cros", "guest_clock_sync",
                [&](perfetto::EventContext p) { perfetto_annotate_time_sync(p); });
    }
}

void init_perfetto()
{
    std::call_once(perfetto_once, [](){
        perfetto::TracingInitArgs args;
        args.backends |= perfetto::kSystemBackend;
        perfetto::Tracing::Initialize(args);
        perfetto::TrackEvent::Register();
        bg_thread = new std::thread(tick_forever);
    });
}

extern "C" JNIEXPORT void JNICALL
Java_com_google_perfettoguesttimesync_TimeTrace_perfettoInit(
        JNIEnv* env,
        jobject /* this */) {
    init_perfetto();
}