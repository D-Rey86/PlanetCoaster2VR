#include "stereo.h"

#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "cb_math.h"
#include "cb_patcher.h"
#include "log.h"
#include "view_cb.h"
#include "depth_probe.h"
#include "ui_layer.h"
#include "vrs.h"
#include "xr_runtime.h"

using Microsoft::WRL::ComPtr;

namespace pc2vr::stereo {
namespace {

constexpr int kMaxCopies = 32;

bool SafeCopy(void* dst, const void* src, size_t n) {
    __try {
        memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The main view's camera-buffer copies for one run of the batch, with the game's own
// camera position (rows 84 and 85: current and previous).
struct Copies {
    uint8_t* cbs[kMaxCopies];
    int n = 0;
    float orig[kMaxCopies][2][4];
};

// Moves the camera of every copy by `offset` (game units). Rendering is
// camera-relative, so the matrices and frustum planes need no change.
void WriteOffset(const Copies& c, const float* offset) {
    for (int i = 0; i < c.n; ++i) {
        float pos[2][3];
        for (int r = 0; r < 2; ++r)
            for (int k = 0; k < 3; ++k) pos[r][k] = c.orig[i][r][k] + offset[k];
        SafeCopy(c.cbs[i] + viewcb::kRowCameraPos * 16, pos[0], 12);
        SafeCopy(c.cbs[i] + (viewcb::kRowCameraPos + 1) * 16, pos[1], 12);
    }
}

// ---- per-eye projection ----
// Each eye renders only its own field of view: the camera rows of every copy are
// rebuilt (cbmath::Rebuild, checked against captured buffers by cb_math_test) with
// that eye's asymmetric frustum, the engine camera's orientation and the eye's
// position. The game's temporal history (anti-aliasing, fog, clouds) is shared by
// the two runs, so what it holds is always the previous *run*: the other eye. Rows
// 8-15 (previous view-projection) and 85 (previous position) therefore describe that
// run, so the game reprojects its history correctly - to the game it is just a camera
// move. Measured: with each eye's own previous frame instead, the eyes' haze differed.
struct Foveation {
    uint32_t w = 0, h = 0;
    float frustum[4] = {};
    float center[2][2] = {};
    bool fullRate = false;  // a fixed screen: no foveation
    bool valid = false;
} g_fov;
void PrepareFoveation(ID3D12CommandQueue* queue, int eye) {
    vrs::PrepareEye(queue, eye, g_fov.w, g_fov.h, g_fov.frustum, g_fov.center[eye], g_fov.fullRate);
}
using EyeRows = std::vector<float>;  // kMaxCopies x kRows x 4, one per copy
float g_lastSecondOffset[3];    // last frame's second eye (its history position = last frame's camera + this)
float g_lastRun[8][4];
float g_lastRunPos[3];
bool g_haveLastRun = false;

void BuildEyeRows(const Copies& c, const float (*full)[cbmath::kRows][4], const float* offset, const float* tanFov, int eye,
                  EyeRows& out) {
    out.assign(static_cast<size_t>(c.n) * cbmath::kRows * 4, 0.f);
    for (int i = 0; i < c.n; ++i) {
        float (*rows)[4] = reinterpret_cast<float (*)[4]>(out.data() + static_cast<size_t>(i) * cbmath::kRows * 4);
        memcpy(rows, full[i], sizeof(float) * cbmath::kRows * 4);
        cbmath::ParsedView v = cbmath::Parse(full[i]);
        v.pos = {v.pos.x + offset[0], v.pos.y + offset[1], v.pos.z + offset[2]};
        const cbmath::Fov f{tanFov[0], tanFov[1], tanFov[2], tanFov[3]};
        cbmath::Rebuild(v, v.basis, f, g_haveLastRun ? g_lastRun : nullptr, *reinterpret_cast<cbmath::Rows*>(rows));
        for (int k = 0; k < 3; ++k) {
            rows[viewcb::kRowCameraPos][k] = full[i][viewcb::kRowCameraPos][k] + offset[k];
            rows[viewcb::kRowCameraPos + 1][k] =
                g_haveLastRun ? g_lastRunPos[k] : full[i][viewcb::kRowCameraPos + 1][k] + offset[k];
        }
    }
    (void)eye;
    if (c.n) {  // this run becomes the next run's history
        memcpy(g_lastRun, out.data(), sizeof(g_lastRun));
        for (int k = 0; k < 3; ++k) g_lastRunPos[k] = out[viewcb::kRowCameraPos * 4 + k];
        g_haveLastRun = true;
    }
}

// Same-projection stereo (the engine's own frustum for both eyes): only the position
// moves. The temporal history (anti-aliasing / upscaling and other accumulated
// effects) is not per eye: the game alternates its history images by frame, so both
// runs of a frame read what the last run of the previous frame wrote. For the first
// eye that is last frame's second eye; for the second eye, its own last frame. The
// previous-camera rows (position 85, view-projection 8-15, jitter 98.xy; measured
// layout: 97.zw current jitter, 98.xy previous) describe exactly that, so the game
// reprojects its history from where it was really rendered.
// Measured 2026-10-03 on the simulated headset with wide eye separation: describing
// the second eye's history as the first eye of the same frame smeared the second eye
// in every temporal mode (TAA: sharpness 39/47 vs 70/73; DLSS: heavy sideways smear,
// sharpness 41 vs 52/37 and clean; Upscale TAA: visibly softer); described as its own
// last frame, both eyes were clean in all three.
void BuildSameProjectionRows(const Copies& c, const float (*full)[cbmath::kRows][4], const float* offset, int eye,
                             EyeRows& out) {
    out.assign(static_cast<size_t>(c.n) * cbmath::kRows * 4, 0.f);
    for (int i = 0; i < c.n; ++i) {
        float (*rows)[4] = reinterpret_cast<float (*)[4]>(out.data() + static_cast<size_t>(i) * cbmath::kRows * 4);
        memcpy(rows, full[i], sizeof(float) * cbmath::kRows * 4);
        for (int k = 0; k < 3; ++k) rows[viewcb::kRowCameraPos][k] = full[i][viewcb::kRowCameraPos][k] + offset[k];
        // Side planes 79-82 hold dot(normal, camera position): move them with the eye, or
        // GPU culling clips the edges once the eyes are far apart (zoom-linked depth).
        for (int p = 79; p <= 82; ++p)
            rows[p][3] = full[i][p][3] + rows[p][0] * offset[0] + rows[p][1] * offset[1] + rows[p][2] * offset[2];
        if (eye == 1) {  // its own last frame
            for (int k = 0; k < 3; ++k)
                rows[viewcb::kRowCameraPos + 1][k] = full[i][viewcb::kRowCameraPos + 1][k] + (g_haveLastRun ? g_lastSecondOffset[k] : offset[k]);
        } else {
            for (int k = 0; k < 3; ++k)
                rows[viewcb::kRowCameraPos + 1][k] = full[i][viewcb::kRowCameraPos + 1][k] + (g_haveLastRun ? g_lastSecondOffset[k] : offset[k]);
        }
    }
}

// Rows written: 0-15 (view-projection, current and previous), 79-92 (frustum planes,
// position, basis, corner rays), 95 and 99 (tan spans, FOV); the rest keep each
// copy's own values.
void WriteEyeRows(const Copies& c, const EyeRows& rows) {
    for (int i = 0; i < c.n; ++i) {
        const float (*r)[4] = reinterpret_cast<const float (*)[4]>(rows.data() + static_cast<size_t>(i) * cbmath::kRows * 4);
        // Only the rows written are checked: other rows of the buffer can legitimately
        // hold non-finite values, and checking them skipped every write.
        bool finite = true;
        auto check = [&](int row, int rows) {
            for (int k = 0; k < rows * 4 && finite; ++k) finite = std::isfinite(r[row + k / 4][k % 4]);
        };
        check(0, 16);
        check(79, 14);
        check(95, 1);
        check(99, 1);
        if (!finite) {
            static int skipped = 0;
            if (++skipped <= 5) LOG("stereo: non-finite eye rows for copy %d; copy left as the game wrote it", i);
            continue;  // never hand the GPU a NaN camera (it hung once)
        }
        SafeCopy(c.cbs[i], r[0], 16 * 16);
        SafeCopy(c.cbs[i] + 79 * 16, r[79], 14 * 16);
        SafeCopy(c.cbs[i] + 95 * 16, r[95], 16);
        SafeCopy(c.cbs[i] + 98 * 16, r[98], 8);  // previous jitter
        SafeCopy(c.cbs[i] + 99 * 16, r[99], 16);
    }
}

// ---- both eyes queued; a helper thread moves the camera between them ----
// The GPU signals `done` after the first eye and waits on `go` before the second;
// the helper waits for `done`, rewrites the view CBs (upload memory, read by the GPU
// when it executes), then signals `go` from the CPU. The game's submit thread never
// blocks. The helper always signals `go`, even on a timeout, so the queue can never
// be left waiting.
struct Job {
    Copies copies;
    float offset[3];
    EyeRows rows;  // per-eye projection: the second eye's camera rows (else empty: offset only)
    uint64_t doneValue = 0, goValue = 0;
};
ComPtr<ID3D12Fence> g_done, g_go;
uint64_t g_doneValue = 0, g_goValue = 0;
HANDLE g_doneEvent = nullptr;
std::mutex g_jobMutex;
std::condition_variable g_jobCv;
std::deque<Job> g_jobs;

void HelperMain() {
    for (;;) {
        Job j;
        {
            std::unique_lock<std::mutex> lock(g_jobMutex);
            g_jobCv.wait(lock, [] { return !g_jobs.empty(); });
            j = std::move(g_jobs.front());
            g_jobs.pop_front();
        }
        const ULONGLONG deadline = GetTickCount64() + 500;
        bool ok = true;
        while (g_done->GetCompletedValue() < j.doneValue) {
            if (GetTickCount64() > deadline) {
                ok = false;
                break;
            }
            g_done->SetEventOnCompletion(j.doneValue, g_doneEvent);
            WaitForSingleObject(g_doneEvent, 20);
        }
        if (ok && !j.rows.empty())
            WriteEyeRows(j.copies, j.rows);
        else if (ok)
            WriteOffset(j.copies, j.offset);
        g_go->Signal(j.goValue);
    }
}

bool InitHelper(ID3D12CommandQueue* queue) {
    if (g_go) return true;
    ComPtr<ID3D12Device> device;
    if (FAILED(queue->GetDevice(IID_PPV_ARGS(&device))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_done))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_go))))
        return false;
    g_doneEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    std::thread(HelperMain).detach();
    return true;
}

// Execute the existing world-marker/HUD split without profiling submissions.
void ExecuteEyeBatch(ID3D12CommandQueue* queue, uint32_t count, ID3D12CommandList* const* lists,
                     ExecuteFn execute, int eye, int split) {
    const int markers = split > 0 ? ui::MarkerSplit() : -1;
    const bool withMarkers = markers > 0 && markers < split;
    if (split <= 0) {
        execute(queue, count, lists);
        return;
    }
    uint32_t done = 0;
    if (withMarkers) {
        execute(queue, static_cast<uint32_t>(markers), lists);
        ui::SaveMarkers(queue, eye, execute);
        done = static_cast<uint32_t>(markers);
    }
    execute(queue, static_cast<uint32_t>(split) - done, lists + done);
    ui::Separate(queue, eye, execute);
    execute(queue, count - static_cast<uint32_t>(split), lists + split);
}

// Runs the batch for the first eye (camera already moved), then queues the second
// run behind the helper's camera move. afterEach(eye) records work right after each
// run (the eye capture).
template <typename AfterEach>
void RunTwice(ID3D12CommandQueue* queue, uint32_t count, ID3D12CommandList* const* lists, ExecuteFn execute,
              const Copies& c, const float* secondOffset, AfterEach afterEach, EyeRows* secondRows = nullptr,
              void (*beforeEach)(ID3D12CommandQueue*, int) = nullptr, int uiSplit = -1) {
    Job j;
    j.copies = c;
    memcpy(j.offset, secondOffset, sizeof(j.offset));
    if (secondRows) j.rows = std::move(*secondRows);
    j.doneValue = ++g_doneValue;
    j.goValue = ++g_goValue;
    if (beforeEach) beforeEach(queue, 0);
    ExecuteEyeBatch(queue, count, lists, execute, 0, uiSplit);
    afterEach(0);
    queue->Signal(g_done.Get(), j.doneValue);
    queue->Wait(g_go.Get(), j.goValue);
    {
        std::lock_guard<std::mutex> lock(g_jobMutex);
        g_jobs.push_back(std::move(j));
    }
    g_jobCv.notify_one();
    if (beforeEach) beforeEach(queue, 1);
    ExecuteEyeBatch(queue, count, lists, execute, 1, uiSplit);
    afterEach(1);
}

}  // namespace

bool Active() { return xr::StereoEnabled(); }

bool OnExecute(ID3D12CommandQueue* queue, uint32_t count, ID3D12CommandList* const* lists, ExecuteFn execute) {
    if (count < 8 || queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) return false;
    Copies c;
    const float (*cached[kMaxCopies])[4] = {};
    c.n = patcher::MainViewCopies(count, lists, c.cbs, cached, kMaxCopies);
    if (c.n < 0) {  // patcher did not read this submission (XR not running): read it here
        c.n = patcher::MainViewCbs(count, lists, c.cbs, kMaxCopies);
        for (int i = 0; i < c.n; ++i) cached[i] = nullptr;
    }
    if (c.n == 0) return false;  // not the main render batch
    for (int i = 0; i < c.n; ++i) {
        if (cached[i])
            memcpy(c.orig[i], cached[i][viewcb::kRowCameraPos], sizeof(c.orig[i]));
        else if (!SafeCopy(c.orig[i], c.cbs[i] + viewcb::kRowCameraPos * 16, sizeof(c.orig[i])))
            return false;
    }
    // True stereo: each eye's offset from the head, on the engine camera's axes.
    float local[2][3], scale = 1.f, tanFov[2][4];
    bool perEye = false;
    if (!xr::StereoEyesForFrame(local, scale, tanFov, perEye) || !InitHelper(queue)) {
        static uint64_t noPose = 0;
        if (++noPose % 300 == 1) LOG("stereo: no head pose for this batch (%llu so far); mono", noPose);
        return false;
    }
    float off[2][3];  // each eye's offset in game space (head roll and tilt included; see xr_runtime)
    for (int e = 0; e < 2; ++e)
        for (int k = 0; k < 3; ++k) off[e][k] = scale * local[e][k];
    // The texture this batch composites into; unknown in the first frames, until the
    // game's copy of it into the back buffer has been seen.
    int compositeList = -1;
    ID3D12Resource* image = xr::BatchFrameImage(count, lists, &compositeList);
    if (!image) {  // run once, as the game does; the headset shows the mono image
        static uint64_t noImage = 0;
        if (++noImage % 300 == 1)
            LOG("stereo: no frame image identified for this batch (%llu so far); mono", noImage);
        return false;
    }
    // The game's UI goes on its own panel: taken out of the image before the list that
    // composites the frame (ui_layer). -1: it stays in the image.
    const int uiSplit = ui::PlanBatch(count, lists, image, compositeList);
    bool captured = true;
    if (perEye) {
        static float full[kMaxCopies][cbmath::kRows][4];
        for (int i = 0; i < c.n; ++i) {
            if (cached[i])
                memcpy(full[i], cached[i], sizeof(full[i]));
            else if (!SafeCopy(full[i], c.cbs[i], sizeof(full[i])))
                return false;
        }
        // Only this submit thread writes the first eye. Retain its allocation; the
        // second eye transfers its own storage to the fence-waiting helper job.
        static thread_local EyeRows first;
        EyeRows second;
        BuildEyeRows(c, full, off[0], tanFov[0], 0, first);
        BuildEyeRows(c, full, off[1], tanFov[1], 1, second);
        WriteEyeRows(c, first);
        RunTwice(queue, count, lists, execute, c, off[1],
                 [&](int eye) { captured = captured && xr::CaptureEye(eye, queue, image); }, &second, nullptr, uiSplit);
    } else {
        static float full[kMaxCopies][cbmath::kRows][4];
        for (int i = 0; i < c.n; ++i) {
            if (cached[i])
                memcpy(full[i], cached[i], sizeof(full[i]));
            else if (!SafeCopy(full[i], c.cbs[i], sizeof(full[i])))
                return false;
        }
        // Foveated rendering: the scene size (smallest view copy), the engine frustum and
        // each eye's centre of view, for the shading-rate image filled before each run.
        {
            int smallest = 0;
            for (int i = 1; i < c.n; ++i)
                if (cached[i] && cached[smallest] &&
                    cached[i][viewcb::kRowRenderSize][0] * cached[i][viewcb::kRowRenderSize][1] <
                        cached[smallest][viewcb::kRowRenderSize][0] * cached[smallest][viewcb::kRowRenderSize][1])
                    smallest = i;
            const cbmath::ParsedView pv = cbmath::Parse(full[smallest]);
            g_fov.w = static_cast<uint32_t>(full[smallest][viewcb::kRowRenderSize][0]);
            g_fov.h = static_cast<uint32_t>(full[smallest][viewcb::kRowRenderSize][1]);
            g_fov.frustum[0] = pv.fov.tanLeft;
            g_fov.frustum[1] = pv.fov.tanRight;
            g_fov.frustum[2] = pv.fov.tanUp;
            g_fov.frustum[3] = pv.fov.tanDown;
            g_fov.valid = xr::EyeCentersTan(g_fov.center, g_fov.fullRate);
        }
        static thread_local EyeRows first;
        EyeRows second;
        BuildSameProjectionRows(c, full, off[0], 0, first);
        BuildSameProjectionRows(c, full, off[1], 1, second);
        memcpy(g_lastSecondOffset, off[1], sizeof(g_lastSecondOffset));
        g_haveLastRun = true;
        WriteEyeRows(c, first);
        RunTwice(queue, count, lists, execute, c, off[1],
                 [&](int eye) { captured = captured && xr::CaptureEye(eye, queue, image); }, &second,
                 vrs::Enabled() && g_fov.valid ? &PrepareFoveation : nullptr, uiSplit);
    }
    ui::FinishBatch(count, lists);
    {
        // Nearest visible distance for zoom-linked depth (xr). The depth buffer is
        // at the render size, the smallest of the main view's copies (with DLSS the
        // largest copy is the output size).
        int smallest = 0;
        for (int i = 1; i < c.n; ++i)
            if (cached[i] && cached[smallest] &&
                cached[i][viewcb::kRowRenderSize][0] * cached[i][viewcb::kRowRenderSize][1] <
                    cached[smallest][viewcb::kRowRenderSize][0] * cached[smallest][viewcb::kRowRenderSize][1])
                smallest = i;
        if (cached[smallest]) {
            cbmath::Rows rows;
            memcpy(rows, cached[smallest], sizeof(rows));
            depth::Measure(queue, count, lists, static_cast<uint32_t>(rows[viewcb::kRowRenderSize][0]),
                           static_cast<uint32_t>(rows[viewcb::kRowRenderSize][1]), cbmath::Parse(rows).nearZ);
        }
    }
    if (captured) xr::EyesCaptured(queue);

    return true;

}

}  // namespace pc2vr::stereo
