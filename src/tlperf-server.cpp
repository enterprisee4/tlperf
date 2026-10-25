// tlperf-server - analyzer backend for tlperf.data.
//
// Serves the parsed profiling data over HTTP/JSON using cpp-httplib and mounts
// the web/ front-end (index.html + app.js) at "/".
//
//   ./tlperf-server -d tlperf.data -p 8080 [-w web]
//
// Endpoints:
//   GET /            -> front-end (mounted directory)
//   GET /api/info    -> {version, threadCount, eventCount, minUs, maxUs, totalUs}
//   GET /api/threads -> [{i, threadId, name, eventCount}]
//   GET /api/summary -> [{i, name, selfUs, totalUs, callCount, avgUs}] (self time per function)
//   GET /api/events?from=X&to=Y&thread=[i]  -> [{t,i,f,p,d,s,e,n}]
//     where t=thread index, i=event id, f=funcId, p=parentId, d=depth, s=startUs, e=endUs, n=name
//
// The front-end renders one column per thread; within a column, nested calls are
// stacked (a child's column is indented under its parent). Rows are time.

#include <httplib.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "tlperf.h"

// ---------------------------------------------------------------------------
// Minimal JSON writer (strings escaped; no external deps).
// ---------------------------------------------------------------------------

namespace json {

std::string esc(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"':
                o += "\\\"";
                break;
            case '\\':
                o += "\\\\";
                break;
            case '\b':
                o += "\\b";
                break;
            case '\f':
                o += "\\f";
                break;
            case '\n':
                o += "\\n";
                break;
            case '\r':
                o += "\\r";
                break;
            case '\t':
                o += "\\t";
                break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    o += buf;
                } else {
                    o += static_cast<char>(c);
                }
        }
    }
    return o;
}

inline std::string num(double v) {
    if (std::isfinite(v) && v == std::floor(v) && std::fabs(v) < 9e15) {
        std::ostringstream o;
        o << static_cast<long long>(v);
        return o.str();
    }
    std::ostringstream o;
    o << v;
    return o.str();
}

inline std::string num(int64_t v) {
    return std::to_string(v);
}
inline std::string num(uint64_t v) {
    return std::to_string(v);
}
inline std::string num(uint32_t v) {
    return std::to_string(v);
}
inline std::string num(int32_t v) {
    return std::to_string(v);
}

}  // namespace json

// ---------------------------------------------------------------------------
// Server state (loaded once at start; read-only thereafter).
// ---------------------------------------------------------------------------

struct State {
    tlperf::ParsedData data;
    bool ok = false;

    // Per-thread self (exclusive) time per function: selfUs[thread][funcId].
    std::vector<std::vector<double>> selfUs;
    // Per-function inclusive (raw on-CPU, summed over all frames with that name)
    // time and call counts across all threads.
    std::vector<double> totalUs;
    std::vector<uint64_t> callCount;
};

static State g_state;

// ---------------------------------------------------------------------------
// Analysis
// ---------------------------------------------------------------------------

// Compute self time: for each event, its self duration = (end-start) minus the
// sum of direct children durations. Children are found via parentId within the
// same thread.
static void analyze(State& st) {
    const auto& d = st.data;
    size_t nThreads = d.threads.size();
    size_t nFunc = d.functions.size();
    st.selfUs.assign(nThreads, std::vector<double>(nFunc, 0.0));
    st.totalUs.assign(nFunc, 0.0);
    st.callCount.assign(nFunc, 0u);

    for (size_t t = 0; t < nThreads; ++t) {
        const auto& evs = d.threads[t].events;
        // Accumulate child durations into the parent.
        std::vector<double> childSum(evs.size(), 0.0);
        for (size_t i = 0; i < evs.size(); ++i) {
            const auto& e = evs[i];
            double dur = static_cast<double>(e.endUs > e.startUs ? e.endUs - e.startUs : 0);
            if (e.parentId >= 0 && static_cast<size_t>(e.parentId) < evs.size())
                childSum[e.parentId] += dur;
            // inclusive "total": raw on-CPU duration of every frame with this name
            if (e.funcId < nFunc)
                st.totalUs[e.funcId] += dur;
        }
        for (size_t i = 0; i < evs.size(); ++i) {
            const auto& e = evs[i];
            double dur = static_cast<double>(e.endUs > e.startUs ? e.endUs - e.startUs : 0);
            double s = std::max(0.0, dur - childSum[i]);
            if (e.funcId < nFunc) {
                st.selfUs[t][e.funcId] += s;
                st.callCount[e.funcId]++;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// HTTP handlers
// ---------------------------------------------------------------------------

static void handleInfo(httplib::Server& svr) {
    svr.Get("/api/info", [](const httplib::Request&, httplib::Response& res) {
        const auto& d = g_state.data;
        std::string j = "{";
        j += "\"version\":" + json::num(d.version) + ",";
        j += "\"threadCount\":" + json::num(d.threads.size()) + ",";
        j += "\"eventCount\":" + json::num(d.eventCount()) + ",";
        j += "\"minUs\":" + json::num(d.minUs) + ",";
        j += "\"maxUs\":" + json::num(d.maxUs) + ",";
        j += "\"totalUs\":" + json::num(d.totalUs()) + ",";
        j += "\"functions\":" + json::num(d.functions.size());
        j += "}";
        res.set_content(j, "application/json");
    });
}

static void handleThreads(httplib::Server& svr) {
    svr.Get("/api/threads", [](const httplib::Request&, httplib::Response& res) {
        const auto& d = g_state.data;
        std::string j = "[";
        for (size_t i = 0; i < d.threads.size(); ++i) {
            const auto& t = d.threads[i];
            if (i)
                j += ",";
            j += "{";
            j += "\"i\":" + json::num(i) + ",";
            j += "\"threadId\":" + json::num(t.threadId) + ",";
            j += "\"name\":\"" + json::esc(t.name) + "\",";
            j += "\"eventCount\":" + json::num(t.events.size());
            j += "}";
        }
        j += "]";
        res.set_content(j, "application/json");
    });
}

static void handleSummary(httplib::Server& svr) {
    svr.Get("/api/summary", [](const httplib::Request&, httplib::Response& res) {
        const auto& d = g_state.data;
        const auto& st = g_state;
        size_t nFunc = d.functions.size();

        // Build rows: funcId, selfUs (summed), callCount.
        struct Row {
            uint32_t funcId;
            double selfUs;
            double totalUs;
            uint64_t count;
        };
        std::vector<Row> rows;
        rows.reserve(nFunc);
        for (uint32_t f = 0; f < nFunc; ++f) {
            double s = 0;
            for (const auto& perThread : st.selfUs)
                s += perThread[f];
            rows.push_back({f, s, st.totalUs[f], st.callCount[f]});
        }
        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.selfUs > b.selfUs; });

        std::string j = "[";
        for (size_t i = 0; i < rows.size(); ++i) {
            const auto& r = rows[i];
            if (i)
                j += ",";
            double avg = r.count ? r.selfUs / static_cast<double>(r.count) : 0.0;
            j += "{";
            j += "\"i\":" + json::num(r.funcId) + ",";
            j += "\"name\":\"" + json::esc(d.functions[r.funcId]) + "\",";
            j += "\"selfUs\":" + json::num(r.selfUs) + ",";
            j += "\"totalUs\":" + json::num(r.totalUs) + ",";
            j += "\"callCount\":" + json::num(r.count) + ",";
            j += "\"avgUs\":" + json::num(avg);
            j += "}";
        }
        j += "]";
        res.set_content(j, "application/json");
    });
}

// Append one event as a comma-separated object to `j` (no brackets).
static void appendEvent(std::string& j, bool& first, size_t t, const tlperf::Event& e) {
    if (!first)
        j += ",";
    first = false;
    j += "{";
    j += "\"t\":" + json::num(t) + ",";
    j += "\"i\":" + json::num(e.id) + ",";
    j += "\"f\":" + json::num(e.funcId) + ",";
    j += "\"p\":" + json::num(e.parentId) + ",";
    j += "\"d\":" + json::num(e.depth) + ",";
    j += "\"s\":" + json::num(e.startUs) + ",";
    j += "\"e\":" + json::num(e.endUs) + ",";
    j += "\"n\":\"" + json::esc(e.name ? e.name : "") + "\"";
    j += "}";
}

// Emit events of a single thread overlapping [from,to) as a JSON array string.
static std::string threadEventsJson(size_t t, uint64_t from, uint64_t to) {
    const auto& d = g_state.data;
    if (t >= d.threads.size())
        return "[]";
    const auto& evs = d.threads[t].events;
    std::string j = "[";
    bool first = true;
    for (const auto& e : evs) {
        if (e.endUs <= from || e.startUs >= to)
            continue;
        appendEvent(j, first, t, e);
    }
    j += "]";
    return j;
}

// Emit events of a single thread overlapping [from,to) as comma-separated items
// (no surrounding brackets), appending to `j`.
static void threadEventsItems(std::string& j, bool& first, size_t t, uint64_t from, uint64_t to) {
    const auto& d = g_state.data;
    if (t >= d.threads.size())
        return;
    const auto& evs = d.threads[t].events;
    for (const auto& e : evs) {
        if (e.endUs <= from || e.startUs >= to)
            continue;
        appendEvent(j, first, t, e);
    }
}

static void handleEvents(httplib::Server& svr) {
    svr.Get("/api/events", [](const httplib::Request& req, httplib::Response& res) {
        uint64_t from = 0, to = 0;
        if (req.has_param("from"))
            from = std::strtoull(req.get_param_value("from").c_str(), nullptr, 10);
        if (req.has_param("to"))
            to = std::strtoull(req.get_param_value("to").c_str(), nullptr, 10);

        const auto& d = g_state.data;
        if (to == 0)
            to = d.maxUs;
        if (from == 0)
            from = d.minUs;
        if (to < from)
            std::swap(from, to);

        std::string j;
        if (req.has_param("thread")) {
            uint64_t t = std::strtoull(req.get_param_value("thread").c_str(), nullptr, 10);
            j = threadEventsJson(t, from, to);
        } else {
            j = "[";
            bool first = true;
            for (size_t t = 0; t < d.threads.size(); ++t)
                threadEventsItems(j, first, t, from, to);
            j += "]";
        }
        res.set_content(j, "application/json");
    });
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

static void printUsage(const char* prog) {
    std::fprintf(stderr,
                 "tlperf-server - analyze tlperf.data over HTTP\n"
                 "  -d FILE   path to tlperf.data (default: tlperf.data)\n"
                 "  -p PORT   listen port (default: 8080)\n"
                 "  -w DIR    web front-end directory (default: web)\n"
                 "  -h        this help\n");
}

int main(int argc, char** argv) {
    std::string dataPath = "tlperf.data";
    std::string webDir = "web";
    int port = 8080;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-d" && i + 1 < argc)
            dataPath = argv[++i];
        else if (a == "-p" && i + 1 < argc)
            port = std::atoi(argv[++i]);
        else if (a == "-w" && i + 1 < argc)
            webDir = argv[++i];
        else if (a == "-h" || a == "--help") {
            printUsage(argv[0]);
            return 0;
        }
    }

    if (!tlperf::readData(dataPath, g_state.data)) {
        std::fprintf(stderr, "error: failed to read %s (missing or bad magic)\n", dataPath.c_str());
        return 1;
    }
    g_state.ok = true;
    analyze(g_state);

    std::printf("tlperf-server: loaded %s\n", dataPath.c_str());
    std::printf("  threads=%zu events=%llu functions=%zu range=[%llu,%llu]us\n", g_state.data.threads.size(),
                static_cast<unsigned long long>(g_state.data.eventCount()), g_state.data.functions.size(),
                static_cast<unsigned long long>(g_state.data.minUs),
                static_cast<unsigned long long>(g_state.data.maxUs));

    httplib::Server svr;
    handleInfo(svr);
    handleThreads(svr);
    handleSummary(svr);
    handleEvents(svr);

    svr.set_mount_point("/", webDir);
    svr.set_error_handler([](const httplib::Request&, httplib::Response& res) {
        res.status = 404;
        res.set_content("not found", "text/plain");
    });

    std::printf("serving http://127.0.0.1:%d  (Ctrl-C to stop)\n", port);
    // listen() blocks and serves requests until the process is stopped (Ctrl-C).
    if (!svr.listen("0.0.0.0", port)) {
        std::fprintf(stderr, "error: failed to listen on port %d\n", port);
        return 1;
    }
    return 0;
}
