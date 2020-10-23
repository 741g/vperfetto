#include "perfetto.h"
#include "vperfetto-min.h"
#include "vperfetto.h"

#include <string>
#include <thread>
#include <fstream>
#include <unordered_map>

PERFETTO_DEFINE_CATEGORIES(
    ::perfetto::Category("gfx")
        .SetDescription("Events from the graphics subsystem"));
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

static void initPerfetto() {
    if (!sPerfettoInitialized) {
        ::perfetto::TracingInitArgs args;
        args.backends |= ::perfetto::kInProcessBackend;
        ::perfetto::Tracing::Initialize(args);
        ::perfetto::TrackEvent::Register();
        sPerfettoInitialized = true;
    }
}

bool useFilenameByEnv(const char* s) {
    return s && ("" != std::string(s));
}

VPERFETTO_EXPORT void vperfetto_min_startTracing(const char* filename) {
    sTraceConfig.hostFilename = filename;

    // Ensure perfetto is actually initialized.
    initPerfetto();

    if (!sTracingSession) {
        fprintf(stderr, "%s: Tracing begins================================================================================\n", __func__);
        fprintf(stderr, "%s: Configuration:\n", __func__);
        fprintf(stderr, "%s: host filename: %s (possibly set via $VPERFETTO_HOST_FILE)\n", __func__, sTraceConfig.hostFilename);

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
        sTraceConfig.tracingDisabled = false;
    }
}

VPERFETTO_EXPORT void vperfetto_min_endTracing() {
    if (sTracingSession) {
        sTraceConfig.tracingDisabled = true;

        // Don't disable again if we are saving.
        if (sTraceConfig.saving) return;
        sTraceConfig.saving = true;

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
    }
}

VPERFETTO_EXPORT void vperfetto_min_beginTrackEvent(const char* eventName) {
    TRACE_EVENT_BEGIN("gfx", ::perfetto::StaticString{eventName});
}

VPERFETTO_EXPORT void vperfetto_min_endTrackEvent() {
    TRACE_EVENT_END("gfx");
}

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
