#include "perfetto.h"
#include "vperfetto-min.h"
#include "vperfetto.h"

#include <chrono>
#include <string>
#include <thread>
#include <fstream>
#include <unordered_map>

#define DEFINE_PERFETTO_CATEGORY(name, description) \
    ::perfetto::Category(#name).SetDescription(description),

PERFETTO_DEFINE_CATEGORIES(
    VPERFETTO_LIST_CATEGORIES(DEFINE_PERFETTO_CATEGORY)
    ::perfetto::Category("misc")
    .SetDescription("General events that aren't graphics and don't fall under the above categories"));

PERFETTO_TRACK_EVENT_STATIC_STORAGE();

#define TRACE_COUNTER(category, name, value)  \
    PERFETTO_INTERNAL_TRACK_EVENT(                  \
            category, name, \
            ::perfetto::protos::pbzero::TrackEvent::TYPE_COUNTER, [&](::perfetto::EventContext ctx){ \
            ctx.event()->set_counter_value(value); \
            })

#   define CC_LIKELY( exp )    (__builtin_expect( !!(exp), true ))
#   define CC_UNLIKELY( exp )  (__builtin_expect( !!(exp), false ))

namespace vperfetto {

static bool sPerfettoInitialized = false;

static VirtualDeviceTraceConfig sTraceConfig = {
    .initialized = false,
    .tracingDisabled = true,
    .packetsWritten = 0,
    .sequenceIdWritten = 0,
    .currentInterningId = 1,
    .currentThreadId = 1,
    .hostFilename = "vmm.trace",
    .guestFilename = nullptr,
    .combinedFilename = nullptr,
    .hostStartTime = 0,
    .guestStartTime = 0,
    .guestTimeDiff = 0,
    .perThreadStorageMb = 1,
};

struct TraceProgress {
    std::vector<char> hostTrace;
    std::vector<char> guestTrace;
    std::vector<char> combinedTrace;
};

static TraceProgress sTraceProgress;

static std::unique_ptr<::perfetto::TracingSession> sTracingSession;

static bool validateConfig(const vperfetto_min_config* config) {

    if (!config->init_flags) {
        fprintf(stderr, "%s: Error: No init flags specified. Need 0x%x to activate in-process backend, 0x%x to activate system backend.\n", __func__,
                VPERFETTO_INIT_FLAG_USE_INPROCESS_BACKEND,
                VPERFETTO_INIT_FLAG_USE_SYSTEM_BACKEND);
        return false;
    }

    if (!(config->init_flags & VPERFETTO_INIT_FLAG_USE_SYSTEM_BACKEND)) {
        if (!config->filename ||
            !strcmp("", config->filename)) {
            fprintf(stderr, "%s: Error: Filename for vperfetto not specified while system backend was requested.\n", __func__);
            return false;
        }
    }

    return true;
}

static void initPerfetto(const vperfetto_min_config* config) {
    if (!sPerfettoInitialized) {
        ::perfetto::TracingInitArgs args;

        if (config->init_flags & VPERFETTO_INIT_FLAG_USE_INPROCESS_BACKEND) {
            args.backends |= ::perfetto::kInProcessBackend;
        }
        if (config->init_flags & VPERFETTO_INIT_FLAG_USE_SYSTEM_BACKEND) {
            args.backends |= ::perfetto::kSystemBackend;
        }

        args.shmem_size_hint_kb = config->shmem_size_hint_kb;

        ::perfetto::Tracing::Initialize(args);
        ::perfetto::TrackEvent::Register();
        sPerfettoInitialized = true;

        if (config->init_flags & VPERFETTO_INIT_FLAG_USE_SYSTEM_BACKEND) {
            // When using the sdk with the system backend, we actually need tp
            // spin a bit on acknowledgement that our category was enabled.
            // Do that here.

            PERFETTO_LOG("Waiting for tracing to start...");
            while (!TRACE_EVENT_CATEGORY_ENABLED("gfx")) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            PERFETTO_LOG("Tracing started");
        }
    }
}

bool useFilenameByEnv(const char* s) {
    return s && ("" != std::string(s));
}

VPERFETTO_EXPORT void vperfetto_min_startTracing(const vperfetto_min_config* config) {
    if (!validateConfig(config)) {
        fprintf(stderr, "%s: Not enabling tracing, config was invalid.\n", __func__);
        return;
    }

    sTraceConfig.hostFilename = config->filename;

    // Ensure perfetto is actually initialized.
    initPerfetto(config);

    if (sTraceConfig.tracingDisabled) {
        fprintf(stderr, "%s: Tracing begins================================================================================\n", __func__);
        fprintf(stderr, "%s: Configuration:\n", __func__);
        fprintf(stderr, "%s: host filename: %s (possibly set via $VPERFETTO_HOST_FILE)\n", __func__, sTraceConfig.hostFilename);


        if (!(config->init_flags & VPERFETTO_INIT_FLAG_USE_SYSTEM_BACKEND)) {
            auto desc = ::perfetto::ProcessTrack::Current().Serialize();
            desc.mutable_process()->set_process_name("VirtualMachineMonitorProcess");
            ::perfetto::TrackEvent::SetTrackDescriptor(::perfetto::ProcessTrack::Current(), desc);

            ::perfetto::TraceConfig cfg;
            ::perfetto::protos::gen::TrackEventConfig track_event_cfg;

            // TODO: Should this be another parameter?
            cfg.add_buffers()->set_size_kb(1024 * 100);  // Record up to 100 MiB.
            auto* ds_cfg = cfg.add_data_sources()->mutable_config();
            ds_cfg->set_name("track_event");
            ds_cfg->set_track_event_config_raw(track_event_cfg.SerializeAsString());

            sTracingSession = ::perfetto::Tracing::NewTrace();
            sTracingSession->Setup(cfg);
            sTracingSession->StartBlocking();
        }
        sTraceConfig.tracingDisabled = false;
    }
}

VPERFETTO_EXPORT void vperfetto_min_endTracing() {
    if (!sTraceConfig.tracingDisabled) {
        sTraceConfig.tracingDisabled = true;

        // Don't disable again if we are saving.
        if (sTraceConfig.saving) return;
        sTraceConfig.saving = true;

        if (sTracingSession) {
            sTracingSession->StopBlocking();
            sTraceProgress.hostTrace = sTracingSession->ReadTraceBlocking();

            fprintf(stderr, "%s: Tracing ended================================================================================\n", __func__);
            fprintf(stderr, "%s: Saving trace to disk. Configuration:\n", __func__);
            fprintf(stderr, "%s: host filename: %s\n", __func__, sTraceConfig.hostFilename);

            sTracingSession.reset();

            const char* hostFilename = sTraceConfig.hostFilename;
            {
                std::ofstream hostFile(sTraceConfig.hostFilename, std::ios::out | std::ios::binary);
                hostFile.write(sTraceProgress.hostTrace.data(), sTraceProgress.hostTrace.size());
            }
            sTraceConfig.saving = false;
        } else {
            fprintf(stderr, "%s: Tracing ended================================================================================\n", __func__);
            fprintf(stderr, "%s: No tracing session (assuming system backend), not saving a separate file\n", __func__);
            ::perfetto::TrackEvent::Flush();
        }
    }
}

VPERFETTO_EXPORT void vperfetto_min_beginTrackEvent(const char* eventName) {
    TRACE_EVENT_BEGIN("gfx", ::perfetto::StaticString{eventName});
}

VPERFETTO_EXPORT void vperfetto_min_endTrackEvent() {
    TRACE_EVENT_END("gfx");
}

// Start/end a particular track event in a particular category.
#define DEFINE_CATEGORY_TRACK_EVENT_DEFINITION(name, desc) \
    VPERFETTO_EXPORT void vperfetto_min_beginTrackEvent_##name(const char* eventName) { \
        TRACE_EVENT_BEGIN(#name, ::perfetto::StaticString{eventName}); \
    } \
    VPERFETTO_EXPORT void vperfetto_min_endTrackEvent_##name() { \
        TRACE_EVENT_END(#name); \
    } \

VPERFETTO_LIST_CATEGORIES(DEFINE_CATEGORY_TRACK_EVENT_DEFINITION)

VPERFETTO_EXPORT void vperfetto_min_traceCounter(const char* name, int64_t value) {
    // TODO: Do stuff. This isn't supported in the SDK currently.
    // What this really needs until its supported in the official sdk:
    // a. a static global to track uuids and names for counters
    // b. track objects generated dynamically
    // c. setting the descriptor of these track objects
    // if (CC_LIKELY(sTraceConfig.tracingDisabled)) return;
    // TRACE_COUNTER("gfx", ::perfetto::StaticString{name}, value);
}

} // namespace vperfetto
