// tlperf-demo - generate a synthetic tlperf.data with realistic per-thread call
// stacks (functions, nesting, start/end in microseconds).
//
// This exercises the *manual* (explicit timestamp) API so the output is
// deterministic and reproducible. It simulates a small multi-threaded server:
//   * main     - init -> event-loop (dispatch + signal handlers) -> shutdown
//   * worker N - process jobs with nested deserialize/validate/execute/persist
//   * network  - accept connections, read frames, dispatch to workers
//   * io       - disk read/write/fsync
//   * timer    - periodic scheduled tasks
//
// The resulting file is meant to be analyzed with tlperf-server.
//
// The *live* (real clock) sampling API is shown in the README; a tiny live
// example is included at the bottom under -l.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "tlperf.h"

static void genSimulated(const std::string& outPath, int nWorkers) {
    tlperf::Sampler& S = tlperf::Sampler::instance();
    S.setOutput(outPath);

    std::mt19937 rng(0xC0FFEEu);  // deterministic
    auto urand = [&](int lo, int hi) { return lo + static_cast<int>((rng() % (hi - lo + 1))); };

    const int NWORKERS = nWorkers;
    const int TOTAL_MS = 60;   // 60 ms trace
    const int base = 1000000;  // start at t = 1 s (absolute, us)

    auto begin = [&](const char* f, uint64_t t, uint64_t tid, const char* tname) { S.begin(f, base + t, tid, tname); };
    auto end = [&](const char* f, uint64_t t, uint64_t tid) { S.end(f, base + t, tid); };

    // ---- main thread (tid 0) --------------------------------------------
    const uint64_t mtid = 0;
    auto b0 = [&](const char* f, uint64_t t) { begin(f, t, mtid, "main"); };
    auto e0 = [&](const char* f, uint64_t t) { end(f, t, mtid); };

    b0("main", 0);
    b0("init", 0);
    b0("loadConfig", 200);
    e0("loadConfig", 900);
    b0("openSockets", 1000);
    e0("openSockets", 2600);
    e0("init", 3000);

    // event loop over the whole trace
    b0("eventLoop", 3000);
    for (int ms = 3; ms < TOTAL_MS - 2; ms += 4) {
        int t0 = ms * 1000;
        b0("dispatch", t0);
        int n = urand(1, 4);
        for (int i = 0; i < n; ++i) {
            int a = t0 + i * 400;
            b0("handleSignal", a);
            e0("handleSignal", a + urand(30, 180));
        }
        e0("dispatch", t0 + n * 400 + urand(20, 120));
    }
    e0("eventLoop", (TOTAL_MS - 2) * 1000);

    b0("shutdown", (TOTAL_MS - 2) * 1000);
    b0("closeSockets", (TOTAL_MS - 2) * 1000 + 200);
    e0("closeSockets", (TOTAL_MS - 1) * 1000);
    e0("shutdown", (TOTAL_MS - 1) * 1000 + 300);
    e0("main", (TOTAL_MS - 1) * 1000 + 400);

    // ---- workers ---------------------------------------------------------
    for (int w = 0; w < NWORKERS; ++w) {
        uint64_t tid = 100 + w;
        std::string tname = "worker" + std::to_string(w);
        const bool hot = (w == 2);  // one worker has a heavy compute phase
        begin("workerMain", 3000 + w * 700, tid, tname.c_str());
        const uint64_t traceEnd = (uint64_t)(TOTAL_MS - 2) * 1000;
        uint64_t js = 3500 + w * 700;
        while (js < traceEnd) {
            // Compute the job's intervals bottom-up so every child fits inside
            // its parent (proper nesting), independent of the random values.
            uint64_t a = js + 40;  // deserialize
            uint64_t rf = a + 20;  //   readFrame
            uint64_t R = rf + (uint64_t)urand(30, 90);
            uint64_t D = std::max(a + (uint64_t)urand(120, 240), R);
            uint64_t b = D + (uint64_t)urand(10, 40);  // validate
            uint64_t V = b + (uint64_t)urand(20, 60);
            uint64_t c = V + (uint64_t)urand(10, 40);  // execute
            uint64_t cm = c + 20;                      //   compute
            uint64_t C = cm + (hot ? (uint64_t)urand(400, 800) : (uint64_t)urand(60, 180));
            uint64_t tr = C + (uint64_t)urand(10, 50);  //   transform
            uint64_t T = tr + (uint64_t)urand(100, 250);
            uint64_t E = std::max(C, T) + (uint64_t)urand(10, 40);
            uint64_t d = E + (uint64_t)urand(10, 40);  // persist
            uint64_t fl = d + 30;                      //   flush
            uint64_t F = fl + (uint64_t)urand(40, 120);
            uint64_t P = std::max(d + (uint64_t)urand(160, 240), F);
            uint64_t J = P + (uint64_t)urand(20, 80);
            if (J > traceEnd)
                break;

            // Emit events in proper LIFO (parent-before-child) order.
            begin("processJob", js, tid, tname.c_str());
            begin("deserialize", a, tid, tname.c_str());
            begin("readFrame", rf, tid, tname.c_str());
            end("readFrame", R, tid);
            end("deserialize", D, tid);
            begin("validate", b, tid, tname.c_str());
            end("validate", V, tid);
            begin("execute", c, tid, tname.c_str());
            begin("compute", cm, tid, tname.c_str());
            end("compute", C, tid);
            begin("transform", tr, tid, tname.c_str());
            end("transform", T, tid);
            end("execute", E, tid);
            begin("persist", d, tid, tname.c_str());
            begin("flush", fl, tid, tname.c_str());
            end("flush", F, tid);
            end("persist", P, tid);
            end("processJob", J, tid);

            js = J + (uint64_t)urand(200, 600);
        }
        end("workerMain", (TOTAL_MS - 2) * 1000, tid);
    }

    // ---- network thread --------------------------------------------------
    {
        uint64_t tid = 200;
        begin("netMain", 2500, tid, "network");
        begin("acceptLoop", 2600, tid, "network");
        for (int ms = 4; ms < TOTAL_MS - 2; ms += 6) {
            int t0 = ms * 1000;
            begin("acceptConn", t0, tid, "network");
            end("acceptConn", t0 + urand(40, 120), tid);
            begin("readFrame", t0 + urand(120, 160), tid, "network");
            end("readFrame", t0 + urand(220, 320), tid);
            begin("dispatchToWorker", t0 + urand(320, 360), tid, "network");
            end("dispatchToWorker", t0 + urand(400, 520), tid);
        }
        end("acceptLoop", (TOTAL_MS - 2) * 1000, tid);
        end("netMain", (TOTAL_MS - 2) * 1000 + 200, tid);
    }

    // ---- io thread -------------------------------------------------------
    {
        uint64_t tid = 300;
        begin("ioMain", 3000, tid, "io");
        for (int ms = 5; ms < TOTAL_MS - 2; ms += 5) {
            int t0 = ms * 1000;
            int r = urand(0, 2);
            if (r == 0) {
                begin("diskRead", t0, tid, "io");
                begin("pread", t0 + 30, tid, "io");
                end("pread", t0 + 30 + urand(60, 200), tid);
                end("diskRead", t0 + urand(240, 320), tid);
            } else if (r == 1) {
                begin("diskWrite", t0, tid, "io");
                begin("pwrite", t0 + 30, tid, "io");
                end("pwrite", t0 + 30 + urand(60, 200), tid);
                end("diskWrite", t0 + urand(240, 320), tid);
            } else {
                begin("fsync", t0, tid, "io");
                end("fsync", t0 + urand(80, 260), tid);
            }
        }
        end("ioMain", (TOTAL_MS - 2) * 1000, tid);
    }

    // ---- timer thread ----------------------------------------------------
    {
        uint64_t tid = 400;
        begin("timerMain", 4000, tid, "timer");
        begin("timerLoop", 4100, tid, "timer");
        for (int ms = 8; ms < TOTAL_MS - 2; ms += 10) {
            int t0 = ms * 1000;
            begin("fireTask", t0, tid, "timer");
            begin("tick", t0 + 20, tid, "timer");
            end("tick", t0 + 20 + urand(30, 90), tid);
            end("fireTask", t0 + urand(120, 200), tid);
        }
        end("timerLoop", (TOTAL_MS - 2) * 1000, tid);
        end("timerMain", (TOTAL_MS - 2) * 1000 + 100, tid);
    }

    bool ok = S.dump();
    std::printf("tlperf-demo: wrote %s (%s, %zu events)\n", outPath.c_str(), ok ? "ok" : "FAILED", S.eventCount());
}

// Optional: a tiny live-sampling demo (real clock + real threads).
static void genLive(const std::string& outPath) {
    tlperf::Sampler& S = tlperf::Sampler::instance();
    S.setOutput(outPath);

    S.registerThread("main");
    {
        tlperf::Scope s("boot");
        std::this_thread::sleep_for(std::chrono::microseconds(2000));
        tlperf::Scope s2("bind");
        std::this_thread::sleep_for(std::chrono::microseconds(800));
    }

    std::vector<std::thread> workers;
    for (int w = 0; w < 3; ++w) {
        workers.emplace_back([&S, w]() {
            S.registerThread("worker" + std::to_string(w));
            for (int i = 0; i < 8; ++i) {
                tlperf::Scope job("processJob");
                std::this_thread::sleep_for(std::chrono::microseconds(300));
                tlperf::Scope calc("compute");
                std::this_thread::sleep_for(std::chrono::microseconds(150));
            }
            S.unregisterThread();
        });
    }
    for (auto& t : workers)
        t.join();
    S.unregisterThread();

    bool ok = S.dump();
    std::printf("tlperf-demo (live): wrote %s (%s)\n", outPath.c_str(), ok ? "ok" : "FAILED");
}

int main(int argc, char** argv) {
    std::string out = "tlperf.data";
    bool live = false;
    int nWorkers = 4;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "-o" && i + 1 < argc)
            out = argv[++i];
        else if (std::string(argv[i]) == "-t" && i + 1 < argc)
            nWorkers = std::max(1, std::atoi(argv[++i]));
        else if (std::string(argv[i]) == "-l")
            live = true;
        else if (std::string(argv[i]) == "-h" || std::string(argv[i]) == "--help") {
            std::printf(
                "tlperf-demo [-o FILE] [-t NWORKERS] [-l]\n"
                "  -o FILE    output path (default tlperf.data)\n"
                "  -t N       number of simulated worker threads (default 4)\n"
                "  -l         use live sampling (real clock/threads) instead of simulated\n");
            return 0;
        }
    }
    if (live)
        genLive(out);
    else
        genSimulated(out, nWorkers);
    return 0;
}
