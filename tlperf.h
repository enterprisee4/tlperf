#pragma once
//
// tlperf - lightweight per-thread timeline performance profiler (demo).
//
// Usage from an application:
//
//     #include "tlperf.h"
//
//     int main() {
//         tlperf::Sampler::instance().setOutput("tlperf.data");
//
//         // on every thread that should be profiled, register once:
//         std::thread worker([] {
//             tlperf::Sampler::instance().registerThread("worker");
//             tlperf::Sampler::instance().begin("handleRequest");   // us, auto via clock
//             tlperf::Sampler::instance().begin("parse");
//             tlperf::Sampler::instance().end("parse");
//             tlperf::Sampler::instance().end("handleRequest");
//             tlperf::Sampler::instance().unregisterThread();
//         });
//
//         worker.join();
//         tlperf::Sampler::instance().dump();   // writes "tlperf.data"
//     }
//
// Manual timestamps (microseconds, e.g. replaying an external sampler) are also
// supported via begin(name, startUs, threadId) / end(name, endUs, threadId).
//
// The resulting tlperf.data can be analyzed with the tlperf-server binary:
//
//     ./tlperf-server -d tlperf.data -p 8080
//
// then open http://127.0.0.1:8080 in a browser.
//
// ---------------------------------------------------------------------------
// Binary format (little-endian):
//   Header      : char magic[8] "TLPERF\0\0", uint32 version,
//                 uint32 threadCount, uint64 eventCount
//   Threads     : [threadCount x] uint64 threadId, uint32 nameLen,
//                 uint32 eventCount, char name[nameLen]
//   Events      : [eventCount  x] uint64 threadIdx, uint32 funcId,
//                 int32  parentId, uint64 startUs, uint64 endUs
//                 (parentId indexes events within the SAME thread; -1 = root)
//   Functions   : uint32 funcCount, then [funcCount x] uint32 nameLen,
//                 char name[nameLen]
// ---------------------------------------------------------------------------

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tlperf {

constexpr const char* kFormatMagic = "TLPERF";
constexpr uint32_t kFormatVersion = 1;

// ---------------------------------------------------------------------------
// Data model
// ---------------------------------------------------------------------------

struct Event {
    uint64_t id = 0;             // unique id within a thread (0-based, in write order)
    uint32_t funcId = 0;         // index into the global function name table
    int32_t parentId = -1;       // id of the enclosing event in the same thread, -1 = root
    uint32_t depth = 0;          // nesting depth (root = 0); filled in by readData
    uint64_t startUs = 0;        // start timestamp, microseconds
    uint64_t endUs = 0;          // end timestamp, microseconds (>= startUs)
    const char* name = nullptr;  // convenience: function name (owned by the function table)
};

struct ThreadData {
    uint64_t threadId = 0;      // raw OS thread id (std::thread::id hashed) or user-supplied
    std::string name;           // logical thread name
    std::vector<Event> events;  // events recorded on this thread
};

// ---------------------------------------------------------------------------
// Sampler
// ---------------------------------------------------------------------------

class Sampler {
   public:
    static Sampler& instance();

    Sampler(const Sampler&) = delete;
    Sampler& operator=(const Sampler&) = delete;

    /// Path written by dump() / dumpTo(). Default: "tlperf.data".
    void setOutput(const std::string& path);
    const std::string& output() const;

    /// Register the calling thread so that begin()/end() on it are recorded.
    /// Returns the tlperf thread id. Must be called before begin() on that thread.
    uint64_t registerThread(const std::string& name = "thread");

    /// Close all still-open events (with the current clock) and mark the calling
    /// thread as finished. Safe to call multiple times.
    void unregisterThread();

    /// Begin an interval on the calling thread (timestamp = steady clock now).
    int64_t begin(const std::string& func);

    /// End the most recent unmatched begin of `func` on the calling thread.
    /// Returns the id of the closed event, or -1 if there was no open event.
    int64_t end(const std::string& func);

    /// Begin an interval with explicit timestamps (microseconds) on the thread
    /// identified by `threadId` (auto-registered with `threadName` on first use).
    /// The identity is keyed by `threadId`, so a single caller can deterministically
    /// generate / replay many simulated threads without real OS threads.
    int64_t begin(const std::string& func, uint64_t startUs, uint64_t threadId,
                  const std::string& threadName = "thread");

    /// End the most recent unmatched begin of `func` on `threadId` with explicit
    /// end timestamp (microseconds). Returns the id of the closed event, or -1.
    int64_t end(const std::string& func, uint64_t endUs, uint64_t threadId);

    /// Write everything recorded so far to the output path. Thread-safe; can be
    /// called from any thread (e.g. at program exit). Returns true on success.
    bool dump();

    /// Write everything recorded so far to an explicit path.
    bool dumpTo(const std::string& path);

    /// Number of events recorded (across all threads).
    size_t eventCount() const;

    /// Current snapshot of the recorded data (for in-process inspection).
    std::vector<ThreadData> snapshot() const;

   private:
    Sampler() = default;
    ~Sampler();
};

// ---------------------------------------------------------------------------
// File reader (used by the analyzer / any tool that inspects tlperf.data)
// ---------------------------------------------------------------------------

struct ParsedData {
    uint32_t version = 0;
    std::vector<ThreadData> threads;
    std::vector<std::string> functions;  // index == funcId
    uint64_t minUs = 0;                  // earliest start across all events
    uint64_t maxUs = 0;                  // latest end across all events
    uint64_t totalUs() const { return maxUs > minUs ? maxUs - minUs : 0; }
    size_t eventCount() const {
        size_t n = 0;
        for (const auto& t : threads)
            n += t.events.size();
        return n;
    }
};

/// Read a tlperf.data file into `out`. Returns false on error / bad magic.
bool readData(const std::string& path, ParsedData& out);

// ---------------------------------------------------------------------------
// RAII scope helper:  tlperf::Scope s("frameName");  records begin()/end()
// ---------------------------------------------------------------------------

class Scope {
   public:
    explicit Scope(const std::string& func);
    ~Scope();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

   private:
    std::string _func;
};

}  // namespace tlperf
