#include "perfetto.h"
#include "vperfetto.h"
#include "vperfetto-util.h"
#include "proto/perfetto_trace.pb.h"

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

#ifdef __cplusplus
#   define CC_LIKELY( exp )    (__builtin_expect( !!(exp), true ))
#   define CC_UNLIKELY( exp )  (__builtin_expect( !!(exp), false ))
#else
#   define CC_LIKELY( exp )    (__builtin_expect( !!(exp), 1 ))
#   define CC_UNLIKELY( exp )  (__builtin_expect( !!(exp), 0 ))
#endif

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

struct TraceCpuTimeSync {
    bool hasData() const { return cpuTime != 0 && clockTime != 0 && clockId != 0; }

    uint64_t cpuTime;
    double cpuCyclesPerNano;
    uint64_t clockTime;
    uint32_t clockId;
};

struct TraceProgress {
    std::vector<char> hostTrace;
    std::vector<char> guestTrace;
    std::vector<char> combinedTrace;
};

static TraceProgress sTraceProgress;

VPERFETTO_EXPORT void setTraceConfig(std::function<void(VirtualDeviceTraceConfig&)> f) {
    f(sTraceConfig);
}

VPERFETTO_EXPORT VirtualDeviceTraceConfig queryTraceConfig() {
    return sTraceConfig;
}

static void initPerfetto() {
    if (!sPerfettoInitialized) {
        ::perfetto::TracingInitArgs args;
        args.backends |= ::perfetto::kInProcessBackend;
        ::perfetto::Tracing::Initialize(args);
        ::perfetto::TrackEvent::Register();
        sPerfettoInitialized = true;
    }
}

VPERFETTO_EXPORT void initialize(const bool** tracingDisabledPtr) {
    initPerfetto();

    // An optimization to have faster queries of whether tracing is enabled.
    if (tracingDisabledPtr) {
        *tracingDisabledPtr = &sTraceConfig.tracingDisabled;
    }
}

static std::unique_ptr<::perfetto::TracingSession> sTracingSession;

bool useFilenameByEnv(const char* s) {
    return s && ("" != std::string(s));
}

VPERFETTO_EXPORT void enableTracing() {
    const char* hostFilenameByEnv = std::getenv("VPERFETTO_HOST_FILE");
    const char* guestFilenameByEnv = std::getenv("VPERFETTO_GUEST_FILE");
    const char* combinedFilenameByEnv = std::getenv("VPERFETTO_COMBINED_FILE");

    // The environment variables override 
    if (useFilenameByEnv(hostFilenameByEnv)) {
        fprintf(stderr, "%s: Using VPERFETTO_HOST_FILE [%s] for host file\n", __func__, hostFilenameByEnv);
        sTraceConfig.hostFilename = hostFilenameByEnv;
    }

    if (useFilenameByEnv(guestFilenameByEnv)) {
        fprintf(stderr, "%s: Using VPERFETTO_GUEST_FILE [%s] for guest file\n", __func__, guestFilenameByEnv);
        sTraceConfig.guestFilename = guestFilenameByEnv;
    }

    if (useFilenameByEnv(combinedFilenameByEnv)) {
        fprintf(stderr, "%s: Using VPERFETTO_COMBINED_FILE [%s] for combined file\n", __func__, combinedFilenameByEnv);
        sTraceConfig.combinedFilename = combinedFilenameByEnv;
    }

    // Don't enable tracing if host filename is null
    if (!sTraceConfig.hostFilename) return;

    // Don't enable it twice
    if (!sTraceConfig.tracingDisabled) return;

    // Don't enable if we are saving
    if (sTraceConfig.saving) return;

    // Ensure perfetto is actually initialized.
    initPerfetto();

    if (!sTracingSession) {
        fprintf(stderr, "%s: Tracing begins================================================================================\n", __func__);
        fprintf(stderr, "%s: Configuration:\n", __func__);
        fprintf(stderr, "%s: host filename: %s (possibly set via $VPERFETTO_HOST_FILE)\n", __func__, sTraceConfig.hostFilename);
        fprintf(stderr, "%s: guest filename: %s (possibly set via $VPERFETTO_GUEST_FILE)\n", __func__, sTraceConfig.guestFilename);
        fprintf(stderr, "%s: combined filename: %s (possibly set via $VPERFETTO_COMBINED_FILE)\n", __func__, sTraceConfig.combinedFilename);
        fprintf(stderr, "%s: guest time diff to add to host time: %lld\n", __func__, (long long)sTraceConfig.guestTimeDiff);

        auto desc = ::perfetto::ProcessTrack::Current().Serialize();
        desc.mutable_process()->set_process_name("VirtualMachineMonitorProcess");
        ::perfetto::TrackEvent::SetTrackDescriptor(::perfetto::ProcessTrack::Current(), desc);

        ::perfetto::TraceConfig cfg;
        ::perfetto::protos::gen::TrackEventConfig track_event_cfg;
        cfg.add_buffers()->set_size_kb(1024 * 100);  // Record up to 100 MiB.
        auto* ds_cfg = cfg.add_data_sources()->mutable_config();
        ds_cfg->set_name("track_event");
        ds_cfg->set_track_event_config_raw(track_event_cfg.SerializeAsString());

        // Disable service events in the host trace, because they interfere
        // with the guest's and we end up dropping packets on one side or the other.
        auto* builtin_ds_cfg = cfg.mutable_builtin_data_sources();
        builtin_ds_cfg->set_disable_service_events(true);

        sTracingSession = ::perfetto::Tracing::NewTrace();
        sTracingSession->Setup(cfg);
        sTracingSession->StartBlocking();
        sTraceConfig.tracingDisabled = false;
    }
}

static void mutateTracePackets(::perfetto::protos::Trace& pbtrace,
    std::function<void(::perfetto::protos::TracePacket* packet)> mutator) {
    for (int i = 0; i < pbtrace.packet_size(); ++i) {
        auto* packet = pbtrace.mutable_packet(i);
        mutator(packet);
    }
}

static void iterateTraceTimestamps(
    ::perfetto::protos::Trace& pbtrace,
    std::function<uint64_t(uint64_t)> forEachTimestamp) {

    for (int i = 0; i < pbtrace.packet_size(); ++i) {
        auto* packet = pbtrace.mutable_packet(i);
        if (packet->has_timestamp()) {
            packet->set_timestamp(forEachTimestamp(packet->timestamp()));
        }

        if (packet->has_ftrace_events()) {
            auto* ftevts = packet->mutable_ftrace_events();
            for (int j = 0; j < ftevts->event_size(); ++j) {
                auto* ftev = ftevts->mutable_event(j);
                if (ftev->has_timestamp()) {
                    ftev->set_timestamp(forEachTimestamp(ftev->timestamp()));
                }
            }
        }
    }
}

static void iterateTraceTrackDescriptorUuids(
    ::perfetto::protos::Trace& pbtrace,
    std::function<uint64_t(uint64_t)> forEachUuid) {

    for (int i = 0; i < pbtrace.packet_size(); ++i) {
        auto* packet = pbtrace.mutable_packet(i);

        // trace packet defaults
        if (packet->has_trace_packet_defaults()) {
            auto* tpd = packet->mutable_trace_packet_defaults();
            if (tpd->has_track_event_defaults()) {
                auto* ted = tpd->mutable_track_event_defaults();
                if (ted->has_track_uuid()) {
                    ted->set_track_uuid(forEachUuid(ted->track_uuid()));
                }
            }
        }

        // individual track events
        if (packet->has_track_event()) {
            auto* te = packet->mutable_track_event();
            if (te->has_track_uuid()) {
                te->set_track_uuid(forEachUuid(te->track_uuid()));
            }
        }

        // track descriptors
        if (packet->has_track_descriptor()) {
            auto* trd = packet->mutable_track_descriptor();
            if (trd->has_uuid()) {
                trd->set_uuid(forEachUuid(trd->uuid()));
            }
            if (trd->has_parent_uuid()) {
                trd->set_parent_uuid(forEachUuid(trd->parent_uuid()));
            }
        }
    }
}

// A higher-order function to conveniently iterate over all sequence ids and pids.
// TODO: What other Ids are important?
// This also takes care of changing UUIDs if a process or thread descriptor gets its id modified.
static void iterateTraceIds(
    ::perfetto::protos::Trace& pbtrace,
    std::function<uint32_t(uint32_t)> forEachTrustedUid,
    std::function<uint32_t(uint32_t)> forEachSequenceId,
    std::function<int32_t(int32_t)> forEachPid,
    std::function<int32_t(int32_t)> forEachTid,
    std::function<int32_t(int32_t)> forEachCpu) {

    bool needRemapUuids = false;
    std::unordered_map<uint64_t, uint64_t> uuidMap;

    std::function<int32_t(int32_t, uint64_t)> transformPidWithUuidRemapTracking =
        [&needRemapUuids, &uuidMap, forEachPid](int32_t prev, uint64_t prevUuid) {
        int32_t next = forEachPid(prev);
        if (prev == next) return next;
        if (uuidMap.find(prevUuid) != uuidMap.end()) return next;
        uuidMap[prevUuid] = ::perfetto::base::GenUuidv4Lsb();
        needRemapUuids = true;
        return next;
    };

    for (int i = 0; i < pbtrace.packet_size(); ++i) {
        auto* packet = pbtrace.mutable_packet(i);

        if (packet->has_trusted_uid()) {
            packet->set_trusted_uid(
                forEachTrustedUid(packet->trusted_uid()));
        }

        if (packet->has_trusted_packet_sequence_id()) {
            packet->set_trusted_packet_sequence_id(
                forEachSequenceId(packet->trusted_packet_sequence_id()));
        }

        if (packet->has_ftrace_events()) {
            auto* ftevts = packet->mutable_ftrace_events();

            if (ftevts->has_cpu()) {
                ftevts->set_cpu(
                    forEachCpu(ftevts->cpu()));
            }

            for (int j = 0; j < ftevts->event_size(); ++j) {
                auto* ftev = ftevts->mutable_event(j);

                if (ftev->has_pid()) {
                    ftev->set_pid(
                        forEachPid(ftev->pid()));
                }

                if (ftev->has_sched_switch()) {
                    auto* sw = ftev->mutable_sched_switch();
                    if (sw->has_prev_pid() && (sw->prev_pid() != 0)) {
                        sw->set_prev_pid(forEachPid(sw->prev_pid()));
                    }
                    if (sw->has_next_pid() && (sw->next_pid() != 0)) {
                        sw->set_next_pid(forEachPid(sw->next_pid()));
                    }
                }

                if (ftev->has_sched_wakeup()) {
                    auto* wakeup = ftev->mutable_sched_wakeup();
                    if (wakeup->has_pid() && (wakeup->pid() != 0)) {
                        wakeup->set_pid(forEachPid(wakeup->pid()));
                    }
                }

                if (ftev->has_sched_blocked_reason()) {
                    auto* blockedreason = ftev->mutable_sched_blocked_reason();
                    if (blockedreason->has_pid() && (blockedreason->pid() != 0)) {
                        blockedreason->set_pid(forEachPid(blockedreason->pid()));
                    }
                }

                if (ftev->has_sched_waking()) {
                    auto* waking = ftev->mutable_sched_waking();
                    if (waking->has_pid() && (waking->pid() != 0)) {
                        waking->set_pid(forEachPid(waking->pid()));
                    }
                }

                if (ftev->has_sched_wakeup_new()) {
                    auto* evt = ftev->mutable_sched_wakeup_new();
                    if (evt->has_pid() && (evt->pid() != 0)) {
                        evt->set_pid(forEachPid(evt->pid()));
                    }
                }

                if (ftev->has_sched_process_exec()) {
                    auto* evt = ftev->mutable_sched_process_exec();
                    if (evt->has_pid() && (evt->pid() != 0)) {
                        evt->set_pid(forEachPid(evt->pid()));
                    }
                    if (evt->has_old_pid() && (evt->old_pid() != 0)) {
                        evt->set_old_pid(forEachPid(evt->old_pid()));
                    }
                }

                if (ftev->has_sched_process_exit()) {
                    auto* evt = ftev->mutable_sched_process_exit();
                    if (evt->has_pid() && (evt->pid() != 0)) {
                        evt->set_pid(forEachPid(evt->pid()));
                    }
                    if (evt->has_tgid() && (evt->tgid() != 0)) {
                        evt->set_tgid(forEachPid(evt->tgid()));
                    }
                }

                if (ftev->has_sched_process_fork()) {
                    auto* evt = ftev->mutable_sched_process_fork();
                    if (evt->has_parent_pid() && (evt->parent_pid() != 0)) {
                        evt->set_parent_pid(forEachPid(evt->parent_pid()));
                    }
                    if (evt->has_child_pid() && (evt->child_pid() != 0)) {
                        evt->set_child_pid(forEachPid(evt->child_pid()));
                    }
                }

                if (ftev->has_sched_process_free()) {
                    auto* evt = ftev->mutable_sched_process_free();
                    if (evt->has_pid() && (evt->pid() != 0)) {
                        evt->set_pid(forEachPid(evt->pid()));
                    }
                }

                if (ftev->has_sched_process_hang()) {
                    auto* evt = ftev->mutable_sched_process_hang();
                    if (evt->has_pid() && (evt->pid() != 0)) {
                        evt->set_pid(forEachPid(evt->pid()));
                    }
                }

                if (ftev->has_sched_process_wait()) {
                    auto* evt = ftev->mutable_sched_process_wait();
                    if (evt->has_pid() && (evt->pid() != 0)) {
                        evt->set_pid(forEachPid(evt->pid()));
                    }
                }
            }
        }

        if (packet->has_track_descriptor()) {
            auto* trd = packet->mutable_track_descriptor();

            if (trd->has_process()) {
                auto* p = trd->mutable_process();
                if (p->has_pid()) {
                    int32_t prevPid = p->pid();
                    p->set_pid(
                        transformPidWithUuidRemapTracking(p->pid(), trd->uuid()));
                }
            }

            if (trd->has_thread()) {
                auto* t = trd->mutable_thread();
                if (t->has_pid()) {
                    t->set_pid(
                        transformPidWithUuidRemapTracking(t->pid(), trd->uuid()));
                }
            }
        }

        if (packet->has_process_tree()) {
            auto* pt = packet->mutable_process_tree();
            for (int j = 0; j < pt->processes_size(); ++j) {
                auto* p = pt->mutable_processes(j);
                if (p->has_pid()) {
                    p->set_pid(forEachPid(p->pid()));
                }
            }

            for (int j = 0; j < pt->threads_size(); ++j) {
                auto* t = pt->mutable_threads(j);
                if (t->has_tid()) {
                    t->set_tid(forEachTid(t->tid()));
                }
                if (t->has_tgid()) {
                    t->set_tgid(forEachPid(t->tgid()));
                }
            }
        }
    }

    if (needRemapUuids) {
        fprintf(stderr, "%s: need to remap uuids...\n", __func__);
        iterateTraceTrackDescriptorUuids(
            pbtrace,
            [&uuidMap](uint64_t uuid) {
            if (uuidMap.find(uuid) == uuidMap.end()) {
                fprintf(stderr, "%s: Warning: did not catch uuid %llu from previous trace. Was this a parent_uuid? If so, note that the trace was generated like this in the first place (with missing parent_uuid for a track descriptor)\n", __func__, (unsigned long long)uuid);
            }
            return uuidMap[uuid];
        });
    }
}

static void sCalcMaxIds(
    const std::vector<char>& trace,
    uint32_t* maxTrustedUidOut,
    uint32_t* maxSequenceIdOut,
    uint32_t* maxPidOut,
    uint32_t* maxTidOut,
    uint32_t* maxCpuOut) {

    uint32_t maxSequenceId = 0;
    uint32_t maxPid = 0;
    uint32_t maxTrustedUid = 0;
    uint32_t maxTid = 0;
    uint32_t maxCpu = 0;

    *maxSequenceIdOut = maxSequenceId;
    *maxPidOut = maxPid;
    *maxTrustedUidOut = maxTrustedUid;
    *maxTidOut = maxTid;
    *maxCpuOut = maxCpu;;

    ::perfetto::protos::Trace pbtrace;
    std::string traceStr(trace.begin(), trace.end());

    if (!pbtrace.ParseFromString(traceStr)) {
        fprintf(stderr, "%s: Failed to parse protobuf as a string\n", __func__);
        return;
    }

    iterateTraceIds(pbtrace,
        [&maxTrustedUid](uint32_t uid) {
            if (uid > maxTrustedUid) {
                maxTrustedUid = uid;
            }
            return uid;
        },
        [&maxSequenceId](uint32_t seqid) {
            if (seqid > maxSequenceId) {
                maxSequenceId = seqid;
            }
            return seqid;
        },
        [&maxPid](uint32_t pid) {
            if (pid > maxPid) {
                maxPid = pid;
            }
            return pid;
        },
        [&maxTid](uint32_t tid) {
            if (tid > maxTid) {
                maxTid = tid;
            }
            return tid;
        },
        [&maxCpu](uint32_t cpu) {
            if (cpu > maxCpu) {
                maxCpu = cpu;
            }
            return cpu;
        });

    fprintf(stderr, "%s: trace's max trusted uid %u seq %u pid %u\n", __func__, maxTrustedUid, maxSequenceId, maxPid);

    *maxTrustedUidOut = maxTrustedUid;
    *maxSequenceIdOut = maxSequenceId;
    *maxPidOut = maxPid;
    *maxTidOut = maxTid;
    *maxCpuOut = maxCpu;;
}

// Transforms addonTrace timestamps into mainTrace space and merges with mainTrace.
static std::vector<char> constructCombinedTrace(
    const std::vector<char>& mainTrace,
    const std::vector<char>& addonTrace,
    int64_t mainTimeDiff) {

    // Calculate the max seqid/pid/tid in the main
    uint32_t maxMainTrustedUid = 0;
    uint32_t maxMainSequenceId = 0;
    uint32_t maxMainPid = 0;
    uint32_t maxMainTid = 0;
    uint32_t maxMainCpu = 0;

    sCalcMaxIds(mainTrace, &maxMainTrustedUid, &maxMainSequenceId, &maxMainPid, &maxMainTid, &maxMainCpu);

    ::perfetto::protos::Trace addon_pbtrace;
    std::string traceStr(addonTrace.begin(), addonTrace.end());

    if (!addon_pbtrace.ParseFromString(traceStr)) {
        fprintf(stderr, "%s: Failed to parse protobuf as a string\n", __func__);
        return {};
    }

    fprintf(stderr, "%s: postprocessing trace with main time diff of %lld, and offseting by main max seqid %u, max pid %u\n", __func__,
            (long long)mainTimeDiff,
            maxMainSequenceId,
            maxMainPid);

    mutateTracePackets(addon_pbtrace,
        [](auto* packet) {
            bool needReplace = false;
            if (packet->has_clock_snapshot()) {
                packet->clear_clock_snapshot();
            }
            if (packet->has_service_event()) {
                packet->clear_service_event();
            }
            if (needReplace) {
                auto* te = packet->mutable_track_event();
                te->set_type((::perfetto::protos::TrackEvent_Type)(0));
            }
        });

    iterateTraceTimestamps(addon_pbtrace,
        [mainTimeDiff](uint64_t ts) {
            return ts + mainTimeDiff;
        });

    iterateTraceIds(addon_pbtrace,
        [maxMainTrustedUid](uint32_t uid) {
            return uid + maxMainTrustedUid;
        },
        [maxMainSequenceId](uint32_t seqid) {
            return seqid + maxMainSequenceId;
        },
        [maxMainPid](int32_t pid) {
            if (pid == 0) return 0;
            return (int32_t)(pid + maxMainPid);
        },
        [maxMainTid](int32_t tid) {
            if (tid == 0) return 0;
            return (int32_t)(tid + maxMainTid);
        },
        [maxMainCpu](int32_t cpu) {
            return cpu + maxMainCpu + 1;
        });

    std::string traceAfter;
    addon_pbtrace.SerializeToString(&traceAfter);

    std::vector<char> combined;
    combined.resize(mainTrace.size() + traceAfter.size());
    memcpy(combined.data(), mainTrace.data(), mainTrace.size());
    memcpy(combined.data() + mainTrace.size(), traceAfter.data(), traceAfter.size());
    // memcpy(combined.data(), traceAfter.data(), traceAfter.size());
    // memcpy(combined.data() + traceAfter.size(), mainTrace.data(), mainTrace.size());
    // combined.resize(traceAfter.size());
    // memcpy(combined.data(), traceAfter.data(), traceAfter.size());

    return combined;
}

void asyncTraceSaveFunc() {
    fprintf(stderr, "%s: Saving combined trace async...\n", __func__);

    static const int kWaitSecondsPerIteration = 1;
    static const int kMaxIters = 20;
    static const int kMinItersForGuestFileSize = 2;

    const char* hostFilename = sTraceConfig.hostFilename;
    const char* guestFilename = sTraceConfig.guestFilename;
    const char* combinedFilename = sTraceConfig.combinedFilename;

    std::streampos currGuestSize = 0;
    int numGoodGuestFileSizeIters = 0;
    bool good = false;

    for (int i = 0; i < kMaxIters; ++i) {
        fprintf(stderr, "%s: Waiting for 1 second...\n", __func__);
        std::this_thread::sleep_for(std::chrono::seconds(kWaitSecondsPerIteration));
        fprintf(stderr, "%s: Querying file size of guest trace...\n", __func__);
        std::ifstream guestFile(guestFilename, std::ios::in | std::ios::binary | std::ios::ate);
        std::streampos size = guestFile.tellg();

        if (!size) {
            fprintf(stderr, "%s: No size, try again\n", __func__);
            continue;
        }

        if (size != currGuestSize) {
            fprintf(stderr, "%s: Sized changed (%llu to %llu), try again\n", __func__,
                    (unsigned long long)currGuestSize, (unsigned long long)size);
            currGuestSize = size;
            continue;
        }

        ++numGoodGuestFileSizeIters;

        if (numGoodGuestFileSizeIters == kMinItersForGuestFileSize) {
            fprintf(stderr, "%s: size is stable, continue saving\n", __func__);
            good = true;
            break;
        }
    }

    if (!good) {
        fprintf(stderr, "%s: Timed out when waiting for guest file to stabilize, skipping combined trace saving.\n", __func__);
        sTraceProgress.hostTrace.clear();
        sTraceProgress.guestTrace.clear();
        sTraceProgress.combinedTrace.clear();
        sTraceConfig.saving = false;
        return;
    }

    std::ifstream guestFile(guestFilename, std::ios::binary | std::ios::ate);
    std::ifstream::pos_type end = guestFile.tellg();
    guestFile.seekg(0, std::ios::beg);
    sTraceProgress.guestTrace.resize(end);
    guestFile.read(sTraceProgress.guestTrace.data(), end);
    guestFile.close();

    sTraceProgress.combinedTrace =
        constructCombinedTrace(sTraceProgress.guestTrace, sTraceProgress.hostTrace, sTraceConfig.guestTimeDiff);

    std::ofstream hostFile(hostFilename, std::ios::out | std::ios::binary);
    hostFile.write(sTraceProgress.hostTrace.data(), sTraceProgress.hostTrace.size());
    hostFile.close();
    sTraceProgress.hostTrace.clear();
    sTraceProgress.guestTrace.clear();

    std::ofstream combinedFile(combinedFilename, std::ios::out | std::ios::binary);
    combinedFile.write(sTraceProgress.combinedTrace.data(), sTraceProgress.combinedTrace.size());
    combinedFile.close();
    sTraceProgress.combinedTrace.clear();

    fprintf(stderr, "%s: Wrote host trace (%s)\n", __func__, hostFilename);
    fprintf(stderr, "%s: Wrote combined trace (%s)\n", __func__, combinedFilename);

    sTraceConfig.saving = false;
}

VPERFETTO_EXPORT void disableTracing() {
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
        fprintf(stderr, "%s: guest filename: %s\n", __func__, sTraceConfig.guestFilename);
        fprintf(stderr, "%s: combined filename: %s\n", __func__, sTraceConfig.combinedFilename);

        sTracingSession.reset();

        if (!sTraceConfig.guestFilename || !sTraceConfig.combinedFilename) {
            fprintf(stderr, "%s: skipping guest combined trace, "
                            "either guest file name (%p) not specified or "
                            "combined file name (%p) not specified\n", __func__,
                    sTraceConfig.guestFilename,
                    sTraceConfig.combinedFilename);
            fprintf(stderr, "%s: saving only host trace\n", __func__);
            const char* hostFilename = sTraceConfig.hostFilename;
            {
                std::ofstream hostFile(sTraceConfig.hostFilename, std::ios::out | std::ios::binary);
                hostFile.write(sTraceProgress.hostTrace.data(), sTraceProgress.hostTrace.size());
            }
            fprintf(stderr, "%s: saving only host trace (done)\n", __func__);
            sTraceConfig.saving = false;
            return;
        }

        std::thread saveThread(asyncTraceSaveFunc);
        saveThread.detach();
    }
}

VPERFETTO_EXPORT void beginTrace(const char* eventName) {
    TRACE_EVENT_BEGIN("gfx", ::perfetto::StaticString{eventName});
}

VPERFETTO_EXPORT void endTrace() {
    TRACE_EVENT_END("gfx");
}

VPERFETTO_EXPORT void traceCounter(const char* name, int64_t value) {
    // TODO: What this really needs until its supported in the official sdk:
    // a. a static global to track uuids and names for counters
    // b. track objects generated dynamically
    // c. setting the descriptor of these track objects
    // if (CC_LIKELY(sTraceConfig.tracingDisabled)) return;
    // TRACE_COUNTER("gfx", ::perfetto::StaticString{name}, value);
}

VPERFETTO_EXPORT void setGuestTime(uint64_t t) {
    vperfetto::setTraceConfig([t](vperfetto::VirtualDeviceTraceConfig& config) {
        // can only be set before tracing
        if (!config.tracingDisabled) {
            return;
        }
        fprintf(stderr, "vperfetto::setGuestTime: to %llu\n", __func__, (unsigned long long)t);
        config.guestStartTime = t;
        config.hostStartTime = (uint64_t)(::perfetto::base::GetWallTimeNs().count());
        config.guestTimeDiff = getSignedDifference(config.guestStartTime, config.hostStartTime);
    });
}

VPERFETTO_EXPORT uint64_t bootTimeNs() {
    return (uint64_t)(::perfetto::base::GetBootTimeNs().count());
}

VPERFETTO_EXPORT void sleepUs(unsigned interval) {
    ::perfetto::base::SleepMicroseconds(interval);
}

VPERFETTO_EXPORT void waitSavingDone() {
    fprintf(stderr, "%s: waiting for trace saving to be done...\n", __func__);
    while (sTraceConfig.saving) {
        sleepUs(1000000);
    }
    fprintf(stderr, "%s: waiting for trace saving to be done...(done)\n", __func__);
}

uint64_t getTraceStartTime(const std::vector<char>& trace) {
    ::perfetto::protos::Trace pbtrace;
    std::string traceStr(trace.begin(), trace.end());
    if (!pbtrace.ParseFromString(traceStr)) {
        fprintf(stderr, "%s: error: could not parse host trace. return 0\n", __func__);
        return 0;
    }

    for (int i = 0; i < pbtrace.packet_size(); ++i) {
        auto* packet = pbtrace.mutable_packet(i);
        if (packet->has_timestamp()) {
            fprintf(stderr, "%s: first packet with timestamp %llu, using this as corresponding boot time\n", __func__, packet->timestamp());
            return packet->timestamp();
        }
    }

    fprintf(stderr, "%s: did not find any timestamps in trace, return 0\n", __func__);
    return 0;
}

static bool getTraceCpuTimeSync(const std::vector<char>& trace, TraceCpuTimeSync* retCpuTime,
                                uint32_t needed_clock) {
    ::perfetto::protos::Trace pbtrace;
    std::string traceStr(trace.begin(), trace.end());
    if (!pbtrace.ParseFromString(traceStr)) {
        fprintf(stderr, "%s: error: could not parse host trace. return 0\n", __func__);
        return false;
    }

    TraceCpuTimeSync first = {0};
    TraceCpuTimeSync last = {0};
    for (int i = 0; i < pbtrace.packet_size(); ++i) {
        TraceCpuTimeSync found = {0};
        auto* packet = pbtrace.mutable_packet(i);
        if (packet->has_clock_snapshot() && packet->clock_snapshot().clocks_size() == 2) {
            fprintf(stderr, "%s: found cpu clock_snapshot\n", __func__);
            auto snapshot = packet->clock_snapshot();
            int cpuClock, regClock;
            if (snapshot.clocks(0).clock_id() == 64) {
                cpuClock = 0;
                regClock = 1;
            } else {
                cpuClock = 1;
                regClock = 0;
            }
            if (snapshot.clocks(cpuClock).clock_id() != 64) {
                fprintf(stderr, "%s: warning: skipping cpu clock_id not 64 (found %u and %u)\n", __func__,
                        snapshot.clocks(cpuClock).clock_id(), snapshot.clocks(regClock).clock_id());
                continue;
            }
            found.clockId = snapshot.clocks(regClock).clock_id();
            found.clockTime = snapshot.clocks(regClock).timestamp();
            found.cpuTime = snapshot.clocks(cpuClock).timestamp();
        }
        uint32_t boottime_clockid = static_cast<uint32_t>(::perfetto::protos::pbzero::BuiltinClock::BUILTIN_CLOCK_BOOTTIME);
        uint32_t monotonic_clockid = static_cast<uint32_t>(::perfetto::protos::pbzero::BuiltinClock::BUILTIN_CLOCK_MONOTONIC);
        if (packet->has_track_event() && packet->track_event().debug_annotations_size() > 0) {
            for (int d = 0; d < packet->track_event().debug_annotations_size(); ++d) {
                const auto& data = packet->track_event().debug_annotations(d);
                if (data.name() == "clock_sync_boottime" &&
                    (needed_clock == 0 || needed_clock == boottime_clockid)) {
                    found.clockId = boottime_clockid;
                    found.clockTime = data.uint_value();
                } else if (data.name() == "clock_sync_monotonic" &&
                    needed_clock == monotonic_clockid) {
                    found.clockId = monotonic_clockid;
                    found.clockTime = data.uint_value();
                } else if (data.name() == "clock_sync_cputime") {
                    found.cpuTime = data.uint_value();
                }
            }
        }
        if (found.hasData()) {
            last = found;
            if (!first.hasData()) {
                first = last;
            }
        }
    }

    if (first.cpuTime != 0 && last.cpuTime != 0 && last.cpuTime > first.cpuTime) {
        fprintf(stderr, "%s: found cpu time sync spanning %.2f seconds\n", __func__,
            (double)(last.clockTime - first.clockTime) / 1000000000.0);
        double elapsedCycles = (double)(last.cpuTime - first.cpuTime);
        last.cpuCyclesPerNano = elapsedCycles / (double)(last.clockTime - first.clockTime);
        *retCpuTime = last;
        return true;
    }

    fprintf(stderr, "%s: did not find 2 or more CPU time snapshots\n", __func__);
    return false;
}

static int64_t deriveGuestTimeDiffWithGuestAbsoluteTime(
    const std::vector<char>& hostTrace, uint64_t guestBootTimeNs) {

    fprintf(stderr, "%s: Deriving guest time diff from host trace and guest abs time of %llu ns\n", __func__,
            (unsigned long long)guestBootTimeNs);

    uint64_t hostStartTimeNs = getTraceStartTime(hostTrace);

    int64_t diff = getSignedDifference(guestBootTimeNs, hostStartTimeNs);

    fprintf(stderr, "%s: time diff: %lld\n", __func__, (long long)diff);
    return diff;
}

static int64_t deriveGuestTimeDiff(
    const std::vector<char>& guestTrace,
    const std::vector<char>& hostTrace,
    int64_t tscOffset) {

    fprintf(stderr, "%s: Deriving guest time diff from guest and host traces\n", __func__);

    // First check for CPU time sync data in both traces.
    TraceCpuTimeSync hostSync, guestSync;
    fprintf(stderr, "%s: Looking for HOST clock sync...\n", __func__);
    bool hasHostSync = getTraceCpuTimeSync(hostTrace, &hostSync, 0);
    fprintf(stderr, "%s: Looking for GUEST clock sync...\n", __func__);
    bool hasGuestSync = getTraceCpuTimeSync(guestTrace, &guestSync, hostSync.clockId);
    bool sameClock = hasHostSync && hasGuestSync && hostSync.clockId == guestSync.clockId;
    if (hasHostSync && hasGuestSync && sameClock) {
        // Transform guest cpuTime to host:
        guestSync.cpuTime -= tscOffset;
        fprintf(stderr, "%s: CPU cycles/nanos: host %f, guest %f\n", __func__, hostSync.cpuCyclesPerNano,
                guestSync.cpuCyclesPerNano);

        // Guest and host frequency measurement should match.
        double diffGuestHostFreq = abs(hostSync.cpuCyclesPerNano / guestSync.cpuCyclesPerNano - 1.0);
        if (diffGuestHostFreq > 0.0001)
            fprintf(stderr, "%s: Warning: guest and host CPU timer frequencies off by %0.4f %%\n",
                __func__, 100.0 * diffGuestHostFreq);

        double cyclesPerNano = hostSync.cpuCyclesPerNano;
        double cyclesDelta = (double)getSignedDifference(hostSync.cpuTime, guestSync.cpuTime);
        int64_t offsetNs = (int64_t)(cyclesDelta / cyclesPerNano);
        double offsetSec = (double)offsetNs / 1000000000.0;
        fprintf(stderr, "%s: CPU sync begin trace offset %f seconds\n", __func__, offsetSec);
        if (offsetSec > 10.0)
            fprintf(stderr, "%s: WARNING: CPU sync begin trace offset is too big\n", __func__);
        return guestSync.clockTime + offsetNs - hostSync.clockTime;

    } else if (hasHostSync && hasGuestSync) {
        fprintf(stderr, "%s: CPU time sync failed because mismatched clocks (host %u, guest %u)\n", __func__,
            hostSync.clockId, guestSync.clockId);
    }

    uint64_t guestStartTimeNs = getTraceStartTime(guestTrace);
    uint64_t hostStartTimeNs = getTraceStartTime(hostTrace);

    int64_t diff = getSignedDifference(guestStartTimeNs, hostStartTimeNs);

    fprintf(stderr, "%s: time diff: %lld (guest %llu - host %llu) (host + diff = %llu)\n", __func__,
        (long long)diff, (unsigned long long)guestStartTimeNs, (unsigned long long)hostStartTimeNs,
        (unsigned long long)(hostStartTimeNs + diff));
    return diff;
}

VPERFETTO_EXPORT void combineTraces(const TraceCombineConfig* config) {
    std::vector<char> guestTrace;
    std::vector<char> hostTrace;

    std::ifstream guestFile(config->guestFile, std::ios::binary | std::ios::ate);
    std::ifstream::pos_type end = guestFile.tellg();
    guestFile.seekg(0, std::ios::beg);
    guestTrace.resize(end);
    guestFile.read(guestTrace.data(), end);
    guestFile.close();

    std::ifstream hostFile(config->hostFile, std::ios::binary | std::ios::ate);
    end = hostFile.tellg();
    hostFile.seekg(0, std::ios::beg);
    hostTrace.resize(end);
    hostFile.read(hostTrace.data(), end);
    hostFile.close();

    int64_t guestTimeDiff;
    if (config->useGuestAbsoluteTime) {
        guestTimeDiff = deriveGuestTimeDiffWithGuestAbsoluteTime(hostTrace, config->guestClockBootTimeNs);
    } else if (config->useGuestTimeDiff) {
        guestTimeDiff = config->guestClockTimeDiffNs;
    } else {
        guestTimeDiff = deriveGuestTimeDiff(guestTrace, hostTrace, config->guestTscOffset);
    }

    std::vector<char> combinedTrace;
    if (config->mergeGuestIntoHost)
        combinedTrace = constructCombinedTrace(hostTrace, guestTrace, -guestTimeDiff);
    else
        combinedTrace = constructCombinedTrace(guestTrace, hostTrace, guestTimeDiff);

    std::ofstream combinedFile(config->combinedFile, std::ios::out | std::ios::binary);
    combinedFile.write(combinedTrace.data(), combinedTrace.size());
    combinedFile.close();
}

} // namespace vperfetto
