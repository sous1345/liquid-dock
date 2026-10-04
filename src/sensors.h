#pragma once
#include "common.h"
#include "sensor_shared.h"
#include <mutex>
#include <thread>
#include <atomic>

// System monitor data. Everything that needs no admin rights is sampled by the dock itself on a
// background thread; the CPU temperature comes from the optional elevated helper (PawnIO driver).
class SensorClient {
public:
    static constexpr int kHist = 60;
    enum Metric { CPU = 0, GPU, RAM, SSD, Count };

    void Start();                            // launch the local sampler
    void Tick(double now);
    bool Live() const { return live; }
    bool CpuTempAvailable() const { return live && (d.flags & LD_HAS_CPU_TEMP); }
    const LDSensorBlock& Data() const { return d; }
    float Temp(Metric m) const;             // NAN when unknown
    float Threshold(Metric m) const { return m == CPU ? 90.f : m == GPU ? 83.f : m == SSD ? 70.f : 1e9f; }
    int Severity(Metric m) const;           // 0 normal, 1 warm (within 10 °C), 2 hot
    const float* History(Metric m) const { return hist[m]; }
    int histHead = 0, histCount = 0;
    int version = 0;                         // bumps on every new sample

    bool alertPending = false;               // consumed by the primary dock

    // CPU temperature (opt-in): UI thread, right after a user click
    void EnableCpuTemp(HWND owner);
    static bool PawnIOInstalled();

private:
    void EnsureHelper();                     // worker thread: start the helper's scheduled task (silent)
    bool LaunchInstall(HWND owner);          // one UAC prompt
    void ReadHelper(double now);

    // local sampler
    std::thread sampler;
    std::mutex mu;
    LDSensorBlock localBlk{};
    std::atomic<bool> localNew{ false };

    // helper
    HANDLE map = nullptr;
    const LDSensorBlock* view = nullptr;
    LDSensorBlock helperBlk{};
    bool helperLive = false;
    uint32_t lastSeq = 0;
    double lastHelperSample = 0, lastHelperTry = -1e9, lastPoll = 0;

    LDSensorBlock d{};
    bool live = false;
    float hist[Count][kHist] = {};
    bool armed[Count] = { true, true, true, true };
    double lastAlert[Count] = { -1e9, -1e9, -1e9, -1e9 };
};

extern SensorClient g_sensors;
