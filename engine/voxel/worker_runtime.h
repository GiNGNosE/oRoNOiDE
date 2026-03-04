#pragma once

#include "voxel/dual_contouring.h"
#include "voxel/snapshot_registry.h"
#include "voxel/state_version.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>

namespace oro::voxel {

struct WorkerTelemetry {
    uint64_t remeshQueued = 0;
    uint64_t collisionQueued = 0;
    uint64_t remeshDropped = 0;
    uint64_t collisionDropped = 0;
    uint64_t remeshStale = 0;
    uint64_t collisionStale = 0;
};

struct RemeshJobResult {
    VersionToken token{};
    MeshPatchBatch patchBatch{};
    bool stale = false;
    bool skippedByEpoch = false;
    double computeMs = 0.0;
};

struct CollisionJobResult {
    VersionToken token{};
    bool ready = false;
    bool stale = false;
    bool skippedByEpoch = false;
    double computeMs = 0.0;
};

class AsyncWorkerRuntime {
public:
    AsyncWorkerRuntime();
    ~AsyncWorkerRuntime();

    void start();
    void stop();

    void setQueueCapacity(std::size_t capacity);
    void setLatestRequiredMeshVersion(uint64_t version);
    void setLatestRequiredCollisionVersion(uint64_t version);
    void setCurrentEpoch(uint64_t epoch);

    void enqueueRemesh(JobSnapshotHandle snapshot, VersionToken token);
    void enqueueCollision(JobSnapshotHandle snapshot, VersionToken token);

    std::optional<RemeshJobResult> popRemeshResult();
    std::optional<CollisionJobResult> popCollisionResult();
    WorkerTelemetry telemetry() const;

private:
    void remeshThreadMain();
    void collisionThreadMain();

    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_running = false;
    std::size_t m_queueCapacity = 8;

    struct JobEntry {
        JobSnapshotHandle snapshot;
        VersionToken token{};
    };
    std::deque<JobEntry> m_remeshQueue;
    std::deque<JobEntry> m_collisionQueue;
    std::deque<RemeshJobResult> m_remeshResults;
    std::deque<CollisionJobResult> m_collisionResults;

    std::thread m_remeshThread;
    std::thread m_collisionThread;

    std::atomic<uint64_t> m_latestRequiredMeshVersion{0};
    std::atomic<uint64_t> m_latestRequiredCollisionVersion{0};
    std::atomic<uint64_t> m_currentEpoch{0};
    WorkerTelemetry m_telemetry{};
    DualContouringMesher m_mesher;
};

}  // namespace oro::voxel
