// tlperf - lightweight per-thread timeline profiler (implementation).
//
// Design:
//   * A process-wide singleton (Sampler) owns the function-name table and a
//     registry of per-thread state. A single mutex guards the registry + name
//     table. begin()/end() on a given thread only mutate that thread's state,
//     so concurrent profiling on different threads stays cheap and race-free.
//   * Each thread's events form a nesting stack: a begin() pushes an event whose
//     parentId is the id of the current top of stack; the matching end() pops it.
//   * dump() writes a little-endian binary file (see tlperf.h) that is later
//     consumed by the analyzer (readData / tlperf-server).

#include "tlperf.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace tlperf {
namespace {

inline uint64_t nowUs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

inline uint64_t osThreadId() {
    std::hash<std::thread::id> h;
    return static_cast<uint64_t>(h(std::this_thread::get_id()));
}

// ---------------------------------------------------------------------------
// Per-thread state. One instance per profiled thread, owned by the registry.
// ---------------------------------------------------------------------------

struct ThreadState {
    uint64_t threadId = 0;
    std::string name = "thread";
    std::vector<uint32_t> openStack;  // ids of currently-open events (nesting)
    std::vector<Event> events;        // events recorded on this thread (write order)

    // Push a new event; returns its (per-thread) id.
    uint32_t push(uint32_t funcId, int32_t parentId, uint64_t startUs) {
        Event e;
        e.id = static_cast<uint32_t>(events.size());
        e.funcId = funcId;
        e.parentId = parentId;
        e.startUs = startUs;
        e.endUs = startUs;
        openStack.push_back(e.id);
        events.push_back(e);
        return e.id;
    }

    // Close the most recent open event matching funcId. Returns its id or -1.
    int64_t close(uint32_t funcId, uint64_t endUs) {
        for (size_t i = openStack.size(); i-- > 0;) {
            uint32_t id = openStack[i];
            if (id < events.size() && events[id].funcId == funcId) {
                events[id].endUs = endUs;
                openStack.erase(openStack.begin() + static_cast<long>(i));
                return static_cast<int64_t>(id);
            }
        }
        return -1;
    }

    void closeAll(uint64_t t) {
        while (!openStack.empty()) {
            uint32_t id = openStack.back();
            openStack.pop_back();
            if (id < events.size())
                events[id].endUs = t;
        }
    }
};

// ---------------------------------------------------------------------------
// Process-wide state.
// ---------------------------------------------------------------------------

struct Global {
    std::mutex mu;
    std::string output = "tlperf.data";
    std::vector<std::string> functions;                      // index == funcId
    std::unordered_map<std::string, uint32_t> funcIndex;     // name -> funcId
    std::vector<std::unique_ptr<ThreadState>> threads;       // registry, owns state, creation order
    std::unordered_map<uint64_t, ThreadState*> manualIndex;  // threadId -> state (non-owning; for the manual API)

    // Fetch-or-assign a func id. Caller holds mu.
    uint32_t intern(const std::string& name) {
        auto it = funcIndex.find(name);
        if (it != funcIndex.end())
            return it->second;
        uint32_t id = static_cast<uint32_t>(functions.size());
        functions.push_back(name);
        funcIndex.emplace(name, id);
        return id;
    }
};

Global& global() {
    static Global g;
    return g;
}

// Per-thread pointer to this thread's state (no allocation on the hot path).
thread_local ThreadState* tlsState = nullptr;

}  // namespace

// ---------------------------------------------------------------------------
// Sampler
// ---------------------------------------------------------------------------

Sampler& Sampler::instance() {
    static Sampler s;
    return s;
}

Sampler::~Sampler() = default;

void Sampler::setOutput(const std::string& path) {
    auto& g = global();
    std::lock_guard<std::mutex> lk(g.mu);
    g.output = path;
}

const std::string& Sampler::output() const {
    return global().output;
}

uint64_t Sampler::registerThread(const std::string& name) {
    auto& g = global();
    std::lock_guard<std::mutex> lk(g.mu);
    ThreadState* st = new ThreadState();
    st->threadId = osThreadId();
    st->name = name;
    tlsState = st;
    g.threads.emplace_back(st);
    g.manualIndex[st->threadId] = st;
    return st->threadId;
}

void Sampler::unregisterThread() {
    if (tlsState) {
        tlsState->closeAll(nowUs());
        tlsState = nullptr;
    }
}

int64_t Sampler::begin(const std::string& func) {
    auto& g = global();
    std::lock_guard<std::mutex> lk(g.mu);
    if (!tlsState)
        return -1;
    uint32_t funcId = g.intern(func);
    int32_t parent = tlsState->openStack.empty() ? -1 : static_cast<int32_t>(tlsState->openStack.back());
    return static_cast<int64_t>(tlsState->push(funcId, parent, nowUs()));
}

int64_t Sampler::end(const std::string& func) {
    auto& g = global();
    std::lock_guard<std::mutex> lk(g.mu);
    if (!tlsState)
        return -1;
    uint32_t funcId = g.intern(func);
    return tlsState->close(funcId, nowUs());
}

int64_t Sampler::begin(const std::string& func, uint64_t startUs, uint64_t threadId, const std::string& threadName) {
    auto& g = global();
    std::lock_guard<std::mutex> lk(g.mu);
    ThreadState* st = nullptr;
    auto it = g.manualIndex.find(threadId);
    if (it != g.manualIndex.end()) {
        st = it->second;
    } else {
        st = new ThreadState();
        st->threadId = threadId;
        st->name = threadName;
        g.threads.emplace_back(st);
        g.manualIndex[threadId] = st;
    }
    uint32_t funcId = g.intern(func);
    int32_t parent = st->openStack.empty() ? -1 : static_cast<int32_t>(st->openStack.back());
    return static_cast<int64_t>(st->push(funcId, parent, startUs));
}

int64_t Sampler::end(const std::string& func, uint64_t endUs, uint64_t threadId) {
    auto& g = global();
    std::lock_guard<std::mutex> lk(g.mu);
    ThreadState* st = nullptr;
    auto it = g.manualIndex.find(threadId);
    if (it != g.manualIndex.end())
        st = it->second;
    if (!st)
        return -1;
    uint32_t funcId = g.intern(func);
    return st->close(funcId, endUs);
}

size_t Sampler::eventCount() const {
    auto& g = global();
    std::lock_guard<std::mutex> lk(g.mu);
    size_t n = 0;
    for (const auto& t : g.threads)
        n += t->events.size();
    return n;
}

std::vector<ThreadData> Sampler::snapshot() const {
    auto& g = global();
    std::vector<ThreadData> out;
    std::vector<std::string> functions;
    {
        std::lock_guard<std::mutex> lk(g.mu);
        functions = g.functions;
        for (const auto& t : g.threads) {
            ThreadData td;
            td.threadId = t->threadId;
            td.name = t->name;
            td.events = t->events;
            out.push_back(std::move(td));
        }
    }
    for (auto& td : out)
        for (auto& e : td.events)
            e.name = e.funcId < functions.size() ? functions[e.funcId].c_str() : "";
    return out;
}

bool Sampler::dumpTo(const std::string& path) {
    auto& g = global();

    // Snapshot under the lock: copy function table + thread list (and close any
    // still-open events so every event has a valid end timestamp).
    std::vector<std::string> functions;
    std::vector<ThreadData> threads;
    {
        std::lock_guard<std::mutex> lk(g.mu);
        functions = g.functions;
        for (const auto& t : g.threads) {
            if (!t->openStack.empty())
                t->closeAll(nowUs());
            ThreadData td;
            td.threadId = t->threadId;
            td.name = t->name;
            td.events = t->events;
            threads.push_back(std::move(td));
        }
    }

    uint64_t totalEvents = 0;
    for (const auto& td : threads)
        totalEvents += td.events.size();

    std::ofstream f(path, std::ios::binary);
    if (!f)
        return false;

    auto w32 = [&f](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto w64 = [&f](uint64_t v) { f.write(reinterpret_cast<const char*>(&v), 8); };

    char header[8] = {'T', 'L', 'P', 'E', 'R', 'F', 0, 0};
    f.write(header, 8);
    w32(kFormatVersion);
    w32(static_cast<uint32_t>(threads.size()));
    w64(totalEvents);

    for (const auto& td : threads) {
        w64(td.threadId);
        w32(static_cast<uint32_t>(td.name.size()));
        w32(static_cast<uint32_t>(td.events.size()));
        f.write(td.name.data(), static_cast<std::streamsize>(td.name.size()));
    }
    for (size_t t = 0; t < threads.size(); ++t) {
        const auto& td = threads[t];
        for (const auto& e : td.events) {
            w64(t);  // thread index
            w32(e.funcId);
            f.write(reinterpret_cast<const char*>(&e.parentId), 4);
            w64(e.startUs);
            w64(e.endUs);
        }
    }
    w32(static_cast<uint32_t>(functions.size()));
    for (const auto& fn : functions) {
        w32(static_cast<uint32_t>(fn.size()));
        f.write(fn.data(), static_cast<std::streamsize>(fn.size()));
    }

    return f.good();
}

bool Sampler::dump() {
    auto& g = global();
    std::string path;
    {
        std::lock_guard<std::mutex> lk(g.mu);
        path = g.output;
    }
    return dumpTo(path);
}

// ---------------------------------------------------------------------------
// Scope
// ---------------------------------------------------------------------------

Scope::Scope(const std::string& func) : _func(func) {
    Sampler::instance().begin(_func);
}

Scope::~Scope() {
    Sampler::instance().end(_func);
}

// ---------------------------------------------------------------------------
// File reader
// ---------------------------------------------------------------------------

namespace {

struct Reader {
    std::ifstream f;
    explicit Reader(const std::string& path) : f(path, std::ios::binary) {}
    bool ok() const { return f.good(); }
    bool read8(char* b) { return f.read(b, 8).good(); }
    bool readN(char* b, std::streamsize n) { return f.read(b, n).good(); }
    uint32_t u32() {
        char b[4];
        if (!f.read(b, 4).good())
            return 0;
        return static_cast<uint32_t>(static_cast<unsigned char>(b[0])) |
               (static_cast<uint32_t>(static_cast<unsigned char>(b[1])) << 8) |
               (static_cast<uint32_t>(static_cast<unsigned char>(b[2])) << 16) |
               (static_cast<uint32_t>(static_cast<unsigned char>(b[3])) << 24);
    }
    uint64_t u64() {
        char b[8];
        if (!f.read(b, 8).good())
            return 0;
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i)
            v |= static_cast<uint64_t>(static_cast<unsigned char>(b[i])) << (8 * i);
        return v;
    }
};

}  // namespace

bool readData(const std::string& path, ParsedData& out) {
    Reader r(path);
    if (!r.f)
        return false;

    char magic[8] = {0};
    if (!r.read8(magic) || std::memcmp(magic, kFormatMagic, 6) != 0)
        return false;

    out.version = r.u32();
    uint32_t threadCount = r.u32();
    uint64_t eventCount = r.u64();

    out.threads.resize(threadCount);
    for (uint32_t t = 0; t < threadCount; ++t) {
        out.threads[t].threadId = r.u64();
        uint32_t nameLen = r.u32();
        r.u32();  // per-thread event count (events carry the thread index)
        out.threads[t].name.resize(nameLen);
        if (nameLen)
            r.readN(&out.threads[t].name[0], nameLen);
    }

    for (uint64_t i = 0; i < eventCount; ++i) {
        uint32_t threadIdx = static_cast<uint32_t>(r.u64());
        uint32_t funcId = r.u32();
        char pb[4];
        r.readN(pb, 4);
        int32_t parentId = *reinterpret_cast<const int32_t*>(pb);
        uint64_t startUs = r.u64();
        uint64_t endUs = r.u64();

        if (threadIdx >= threadCount)
            continue;
        Event e;
        e.id = static_cast<uint64_t>(out.threads[threadIdx].events.size());
        e.funcId = funcId;
        e.parentId = parentId;
        e.startUs = startUs;
        e.endUs = endUs;
        out.threads[threadIdx].events.push_back(e);
    }

    uint32_t funcCount = r.u32();
    out.functions.resize(funcCount);
    for (uint32_t i = 0; i < funcCount; ++i) {
        uint32_t nameLen = r.u32();
        out.functions[i].resize(nameLen);
        if (nameLen)
            r.readN(&out.functions[i][0], nameLen);
    }

    // Names + global bounds + per-thread sort.
    //
    // The file stores parentId as a PER-THREAD index into the thread's events in
    // WRITE order. Events are appended to the vector in file order, so index i of
    // the vector IS the per-thread id used in parentId. After sorting by start
    // time we must remap id/parentId to the new positions, otherwise parent
    // lookups (self-time, depth) break.
    uint64_t minUs = ~0ull, maxUs = 0;
    bool any = false;
    for (auto& td : out.threads) {
        std::sort(td.events.begin(), td.events.end(), [](const Event& a, const Event& b) {
            if (a.startUs != b.startUs)
                return a.startUs < b.startUs;
            return a.id < b.id;  // stable within same start: keep write order
        });
        // id (write-order index) -> new (sorted) index
        std::vector<uint64_t> oldToNew(td.events.size());
        for (size_t i = 0; i < td.events.size(); ++i)
            oldToNew[td.events[i].id] = i;
        for (size_t i = 0; i < td.events.size(); ++i) {
            Event& e = td.events[i];
            int32_t p = e.parentId;
            if (p >= 0 && static_cast<size_t>(p) < oldToNew.size())
                e.parentId = static_cast<int32_t>(oldToNew[p]);
            else
                e.parentId = -1;
            e.id = i;
            // depth: parent started earlier -> already processed (smaller index)
            e.depth = e.parentId >= 0 ? td.events[e.parentId].depth + 1 : 0;
            e.name = e.funcId < out.functions.size() ? out.functions[e.funcId].c_str() : "";
            any = true;
            minUs = std::min(minUs, e.startUs);
            maxUs = std::max(maxUs, e.endUs);
        }
    }
    if (!any) {
        minUs = 0;
        maxUs = 0;
    }
    out.minUs = minUs;
    out.maxUs = maxUs;
    return true;
}

}  // namespace tlperf
