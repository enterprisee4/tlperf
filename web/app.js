// tlperf front-end - timeline view.
//
// Layout: threads are vertical columns (left-to-right). Within a thread, each
// call is drawn in a "depth column" (nesting depth -> horizontal indentation),
// and time runs vertically (rows = time axis).
//
// Navigation:
//   * drag canvas vertically  -> move the visible TIME window (rows = time)
//   * wheel                   -> zoom time (anchored at cursor)
//   * shift+wheel / horizontal wheel -> pan THREADS horizontally
//   * bottom thread strip     -> click/drag to jump across many threads
//   * "col" slider            -> manual depth-column width
//   * ⊞ button                -> fit ALL threads into the width
//   * minimap (right)         -> global time overview / jump
//
// Scaling to many threads (e.g. 72+):
//   * the timeline canvas is content-sized (wider than the viewport) and is
//     panned via scrollX; only threads intersecting the viewport are drawn
//     (culling), so frame cost stays ~O(visible threads × visible events).
//   * column width auto-fits the available width (min 3px) and is scrollable
//     when it cannot fit; can also be set manually or "fit all".
//   * hit-testing resolves the thread/depth from coordinates in O(1)
//     (uniform column grid) instead of scanning every thread.
//   * the minimap and thread strip are pre-rendered to offscreen canvases and
//     blitted each frame.

(function () {
  "use strict";

  // ---- state -------------------------------------------------------------
  let info = null;
  let threads = [];
  let summary = [];
  let allEvents = [];        // {t,i,f,p,s,e,n,d}
  let byThread = [];         // byThread[t] = array of event objects
  let levelsMax = 1;        // global max depth + 1
  let funcColor = new Map(); // funcId -> color
  let selected = null;
  let hover = null;
  let searchTerm = "";
  let highlightFunc = -1;
  let playing = false;
  let playTimer = null;

  // view: visible time window (microseconds)
  let view = { start: 0, span: 1 };

  // horizontal scroll over the (possibly wider) thread content
  let scrollX = 0;
  // depth column width: 'auto' (fit) or 'manual' (colWUser px)
  let colMode = "auto";
  let colWUser = 16;

  // layout constants
  const HEADER_H = 26;     // thread header row
  const TIME_GUTTER = 74;  // left time-axis gutter (screen-fixed)
  const THREAD_GAP = 14;
  const MIN_COL = 3;
  const MAX_COL = 74;
  const THMAP_H = 30;      // bottom thread-navigation strip height

  // ---- dom ---------------------------------------------------------------
  const cv = document.getElementById("timeline");
  const ctx = cv.getContext("2d");
  const tmap = document.getElementById("threadmap");
  const tmapCtx = tmap.getContext("2d");
  const mm = document.getElementById("minimap");
  const mmctx = mm.getContext("2d");
  const wrap = document.getElementById("timeline-wrap");
  const tooltip = document.getElementById("tooltip");
  const statusEl = document.getElementById("status");
  const inspector = document.getElementById("inspector");
  const windowLabel = document.getElementById("windowLabel");
  const summaryEl = document.getElementById("summary");
  const colwSlider = document.getElementById("colw");
  let W = 0, H = 0, dpr = 1;
  let H_plot = 1;

  // offscreen pre-render layers
  let mmLayer = null;      // minimap content
  let tmapLayer = null;    // thread strip content

  // ---- helpers -----------------------------------------------------------
  function fmtUs(us) {
    us = Math.round(us);
    if (Math.abs(us) >= 1e6) return (us / 1e6).toFixed(3) + " ms";
    if (Math.abs(us) >= 1e3) return (us / 1e3).toFixed(2) + " ms";
    return us + " us";
  }
  function fmtUsAxis(us) {
    us = Math.round(us);
    if (Math.abs(us) >= 1e6) return (us / 1e6).toFixed(2) + "s";
    if (Math.abs(us) >= 1e3) return (us / 1e3).toFixed(1) + "ms";
    return us + "us";
  }
  function clamp(v, a, b) { return Math.max(a, Math.min(b, v)); }

  // Stable, distinguishable color per function id.
  function colorFor(fid) {
    if (funcColor.has(fid)) return funcColor.get(fid);
    const hue = (fid * 2654435761) % 360;
    const c = "hsl(" + hue + ", 62%, 52%)";
    funcColor.set(fid, c);
    return c;
  }
  function colorForDim(fid) {
    return "hsl(" + ((fid * 2654435761) % 360) + ", 24%, 34%)";
  }

  // ---- data loading ------------------------------------------------------
  async function getJSON(url) {
    const r = await fetch(url);
    if (!r.ok) throw new Error(url + " -> " + r.status);
    return r.json();
  }

  async function load() {
    statusEl.textContent = "loading…";
    info = await getJSON("/api/info");
    threads = await getJSON("/api/threads");
    summary = await getJSON("/api/summary");
    allEvents = await getJSON("/api/events?from=" + info.minUs + "&to=" + info.maxUs);

    // group by thread
    byThread = new Array(threads.length);
    for (let t = 0; t < threads.length; t++) byThread[t] = [];
    for (const ev of allEvents) {
      if (ev.t < threads.length) {
        if (ev.d === undefined) ev.d = 0; // older servers without depth
        byThread[ev.t].push(ev);
      }
    }

    // depth: prefer server-provided d; fall back to parent-chain computation.
    levelsMax = 1;
    for (let t = 0; t < threads.length; t++) {
      const evs = byThread[t];
      if (evs[0] && evs[0].d === undefined) {
        const byId = new Array(evs.length);
        const depthById = new Array(evs.length).fill(0);
        for (const e of evs) byId[e.i] = e;
        for (let id = 0; id < evs.length; id++) {
          const e = byId[id];
          e.d = e.p >= 0 && e.p < evs.length ? depthById[e.p] + 1 : 0;
          depthById[id] = e.d;
        }
      }
      for (const e of evs) if (e.d + 1 > levelsMax) levelsMax = e.d + 1;
    }

    view.start = info.minUs;
    view.span = Math.max(1, info.maxUs - info.minUs);

    // Optional URL params to position the view (shareable screenshots/links):
    //   ?from=US&to=US        visible time window
    //   ?sel=THREAD:EVENTID   pre-select an event (inspector + highlight)
    const q = new URLSearchParams(location.search);
    if (q.has("from") && q.has("to")) {
      const a = +q.get("from"), b = +q.get("to");
      if (b > a && b > 0 && b <= info.maxUs && a >= info.minUs) {
        view.start = a;
        view.span = b - a;
      }
    }
    if (q.has("sel")) {
      const parts = q.get("sel").split(":");
      const t = +parts[0], id = +parts[1];
      const e = (byThread[t] || []).find((x) => x.i === id);
      if (e) {
        selected = e;
        const pad = (e.e - e.s) * 2 + 200;
        const c = (e.s + e.e) / 2;
        view.span = Math.min(pad, info.maxUs - info.minUs);
        view.start = clamp(c - view.span / 2, info.minUs, info.maxUs - view.span);
      }
    }

    // header stats
    document.getElementById("s-threads").textContent = info.threadCount;
    document.getElementById("s-events").textContent = info.eventCount;
    document.getElementById("s-span").textContent = fmtUs(info.totalUs);

    renderSummary();
    scrollX = 0;
    resize();
    if (selected) {
      showInspector(selected);
      scrollToThread(selected.t);
    }
    statusEl.textContent = "ready · " + info.eventCount + " events · " + threads.length + " threads" +
      (threads.length > 24 ? " · 横向拖动/shift+滚轮 查看其它线程" : "");
  }

  // ---- self-time table ---------------------------------------------------
  function renderSummary() {
    const max = summary.length ? summary[0].selfUs : 1;
    let html = "<table><thead><tr><th>function</th><th class=num>self</th><th class=num>total</th><th class=num>calls</th></tr></thead><tbody>";
    for (const r of summary) {
      if (r.selfUs <= 0 && r.totalUs <= 0) continue;
      const w = Math.max(1, Math.round(100 * r.selfUs / max));
      html += '<tr class="fn" data-f="' + r.i + '">' +
        '<td><span class="swatch" style="display:inline-block;width:9px;height:9px;border-radius:2px;' +
        'background:' + colorFor(r.i) + ';margin-right:6px;vertical-align:middle;"></span>' + escHtml(r.name) +
        '<div class="bar" style="width:' + w + '%;background:' + colorFor(r.i) + ';"></div></td>' +
        '<td class="num">' + fmtUs(r.selfUs) + '</td>' +
        '<td class="num">' + fmtUs(r.totalUs) + '</td>' +
        '<td class="num">' + r.callCount + '</td></tr>';
    }
    html += "</tbody></table>";
    summaryEl.innerHTML = html;
    summaryEl.querySelectorAll("tr.fn").forEach((tr) => {
      tr.addEventListener("click", () => {
        const f = +tr.dataset.f;
        highlightFunc = highlightFunc === f ? -1 : f;
        document.querySelectorAll("tr.fn").forEach((x) => x.classList.remove("sel"));
        if (highlightFunc >= 0) tr.classList.add("sel");
        draw();
      });
    });
  }

  function escHtml(s) {
    return String(s).replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]));
  }

  // ---- layout ------------------------------------------------------------
  function layout() {
    const nT = threads.length;
    const levels = Math.max(1, levelsMax);
    let colW;
    if (colMode === "manual") {
      colW = colWUser;
    } else {
      const avail = Math.max(60, W - TIME_GUTTER - THREAD_GAP * (nT - 1));
      colW = avail / (nT * levels);
      colW = Math.max(MIN_COL, Math.min(MAX_COL, colW));
    }
    const threadW = levels * colW;
    const totalW = TIME_GUTTER + nT * threadW + (nT - 1) * THREAD_GAP;
    return { nT, levels, colW, threadW, totalW };
  }
  function contentW() { const L = layout(); return Math.max(W, L.totalW); }
  function threadX(i) { const L = layout(); return TIME_GUTTER + i * (L.threadW + THREAD_GAP); }
  function yOf(us) { return HEADER_H + ((us - view.start) / view.span) * H_plot; }
  function usOf(y) { return view.start + ((y - HEADER_H) / H_plot) * view.span; }

  // range of threads intersecting the viewport (for draw culling)
  function visibleThreads(L) {
    const xL = scrollX, xR = scrollX + W;
    const stride = L.threadW + THREAD_GAP;
    let t0 = Math.max(0, Math.floor((xL - TIME_GUTTER - 1) / stride));
    let t1 = Math.min(L.nT - 1, Math.ceil((xR - TIME_GUTTER) / stride));
    return [t0, t1];
  }

  function maxScrollX() { return Math.max(0, contentW() - W); }
  function clampScroll() { scrollX = clamp(scrollX, 0, maxScrollX()); }
  function scrollToThread(t) {
    const L = layout();
    scrollX = clamp(threadX(t) - W / 2 + L.threadW / 2, 0, maxScrollX());
  }

  // ---- drawing -----------------------------------------------------------
  function resize() {
    dpr = window.devicePixelRatio || 1;
    W = wrap.clientWidth;
    H = Math.max(50, wrap.clientHeight - THMAP_H);
    const cw = contentW();
    cv.width = Math.round(cw * dpr);
    cv.height = Math.round(H * dpr);
    cv.style.width = cw + "px";
    cv.style.height = H + "px";
    tmap.width = Math.round(cw * dpr);
    tmap.height = Math.round(THMAP_H * dpr);
    tmap.style.width = cw + "px";
    tmap.style.height = THMAP_H + "px";
    H_plot = H - HEADER_H;
    clampScroll();
    buildLayers();
    draw();
    drawMinimap();
    drawThreadMap();
  }

  function niceStep(span, target) {
    const raw = span / target;
    const mag = Math.pow(10, Math.floor(Math.log10(raw)));
    const norm = raw / mag;
    let step;
    if (norm < 1.5) step = 1;
    else if (norm < 3) step = 2;
    else if (norm < 7) step = 5;
    else step = 10;
    return step * mag;
  }

  function draw() {
    if (!info || !W || !H) return;
    const L = layout();
    const cw = contentW();
    const vEnd = view.start + view.span;

    // content-space transform (horizontal scroll)
    ctx.setTransform(dpr, 0, 0, dpr, -scrollX * dpr, 0);

    // background
    ctx.clearRect(0, 0, cw, H);
    ctx.fillStyle = "#0c0f13";
    ctx.fillRect(0, 0, cw, H);

    const [t0, t1] = visibleThreads(L);

    // ---- thread headers (visible threads only) ----
    ctx.textBaseline = "middle";
    for (let t = t0; t <= t1; t++) {
      const x = threadX(t);
      ctx.fillStyle = "#141b22";
      ctx.fillRect(x, 0, L.threadW, HEADER_H);
      ctx.strokeStyle = "#242e39";
      ctx.strokeRect(x + 0.5, 0.5, L.threadW - 1, HEADER_H - 1);
      if (L.threadW > 24) {
        ctx.fillStyle = "#9fb2c4";
        ctx.font = "600 11px ui-monospace, monospace";
        ctx.textAlign = "center";
        const name = threads[t].name + "  ·  " + threads[t].threadId;
        ctx.fillText(trunc(name, L.threadW - 8), x + L.threadW / 2, HEADER_H / 2);
      }
    }

    // ---- depth sub-column guides (visible threads only) ----
    for (let t = t0; t <= t1; t++) {
      const x = threadX(t);
      for (let d = 0; d < L.levels; d++) {
        ctx.fillStyle = d % 2 ? "#101720" : "#0e141b";
        ctx.fillRect(x + d * L.colW, HEADER_H, L.colW, H_plot);
      }
    }

    // ---- time grid lines (span full content width) ----
    const step = niceStep(view.span, Math.max(3, Math.floor(H_plot / 48)));
    const first = Math.ceil(view.start / step) * step;
    for (let us = first; us <= vEnd; us += step) {
      const y = yOf(us);
      if (y < HEADER_H - 1 || y > H + 1) continue;
      ctx.strokeStyle = "#1a232d";
      ctx.beginPath();
      ctx.moveTo(TIME_GUTTER, y + 0.5);
      ctx.lineTo(cw, y + 0.5);
      ctx.stroke();
    }

    // ---- events (visible threads only) ----
    const showNames = L.colW > 26;
    for (let t = t0; t <= t1; t++) {
      const xBase = threadX(t);
      for (const e of byThread[t]) {
        if (e.e <= view.start || e.s >= vEnd) continue;
        const x = xBase + e.d * L.colW;
        const y1 = yOf(Math.max(e.s, view.start));
        const y2 = yOf(Math.min(e.e, vEnd));
        const h = y2 - y1;
        if (h <= 0) continue;
        const w = L.colW - 2;
        if (w < 1) continue;

        const dim = (searchTerm && e.n.toLowerCase().indexOf(searchTerm) < 0) ||
          (highlightFunc >= 0 && e.f !== highlightFunc);
        ctx.globalAlpha = dim ? 0.5 : 1;
        ctx.fillStyle = dim ? colorForDim(e.f) : colorFor(e.f);
        ctx.fillRect(x + 1, y1, w, h);

        if (w >= 3) {
          ctx.strokeStyle = "rgba(0,0,0,.55)";
          ctx.lineWidth = 1;
          ctx.strokeRect(x + 1.5, y1 + 0.5, w - 1, Math.max(0, h - 1));
        }

        if (!dim && showNames && h > 12) {
          ctx.save();
          ctx.beginPath();
          ctx.rect(x + 1, y1, w, h);
          ctx.clip();
          ctx.fillStyle = "#0b0e12";
          ctx.font = "10px ui-monospace, monospace";
          ctx.textAlign = "left";
          ctx.fillText(trunc(e.n, w - 6), x + 4, y1 + 9);
          ctx.restore();
        }
        ctx.globalAlpha = 1;

        if (selected === e) {
          ctx.strokeStyle = "#ffffff";
          ctx.lineWidth = 2;
          ctx.strokeRect(x + 2, y1 + 1, w - 2, Math.max(1, h - 2));
        }
        if (hover === e && selected !== e) {
          ctx.strokeStyle = "#ffd76a";
          ctx.lineWidth = 1.5;
          ctx.strokeRect(x + 1.5, y1 + 0.5, w - 1, Math.max(0, h - 1));
        }
      }
    }

    // ---- screen-space pass: fixed time gutter ----
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.fillStyle = "#0c0f13";
    ctx.fillRect(0, 0, TIME_GUTTER, H);
    ctx.font = "10px ui-monospace, monospace";
    for (let us = first; us <= vEnd; us += step) {
      const y = yOf(us);
      if (y < HEADER_H - 1 || y > H + 1) continue;
      ctx.fillStyle = "#6b7b8c";
      ctx.textAlign = "right";
      ctx.fillText(fmtUsAxis(us), TIME_GUTTER - 6, y);
    }
    ctx.strokeStyle = "#2a333d";
    ctx.beginPath();
    ctx.moveTo(TIME_GUTTER + 0.5, 0);
    ctx.lineTo(TIME_GUTTER + 0.5, H);
    ctx.stroke();

    updateWindowLabel();
  }

  function trunc(s, px) {
    const max = Math.floor(px / 6);
    return s.length > max ? s.slice(0, max - 1) + "…" : s;
  }

  function updateWindowLabel() {
    windowLabel.textContent = fmtUs(view.start) + "  →  " + fmtUs(view.start + view.span) +
      "  (" + fmtUs(view.span) + ")";
  }

  // ---- pre-render layers (minimap + thread strip) ------------------------
  function buildLayers() {
    if (!info) return;
    const total = Math.max(1, info.maxUs - info.minUs);

    // minimap content
    mmLayer = document.createElement("canvas");
    const mw = mm.clientWidth, mh = mm.clientHeight;
    mmLayer.width = Math.max(1, Math.round(mw * dpr));
    mmLayer.height = Math.max(1, Math.round(mh * dpr));
    const g = mmLayer.getContext("2d");
    g.setTransform(dpr, 0, 0, dpr, 0, 0);
    g.fillStyle = "#0c0f13";
    g.fillRect(0, 0, mw, mh);
    const nT = threads.length;
    const colW = mw / Math.max(1, nT);
    g.globalAlpha = 0.85;
    for (let t = 0; t < nT; t++) {
      const x = t * colW;
      for (const e of byThread[t]) {
        const y1 = ((e.s - info.minUs) / total) * mh;
        const y2 = ((e.e - info.minUs) / total) * mh;
        g.fillStyle = colorFor(e.f);
        g.fillRect(x + 1, y1, Math.max(1, colW - 2), Math.max(0.5, y2 - y1));
      }
    }
    g.globalAlpha = 1;

    // thread strip content
    const cw = contentW();
    tmapLayer = document.createElement("canvas");
    tmapLayer.width = Math.max(1, Math.round(cw * dpr));
    tmapLayer.height = Math.max(1, Math.round(THMAP_H * dpr));
    const g2 = tmapLayer.getContext("2d");
    g2.setTransform(dpr, 0, 0, dpr, 0, 0);
    g2.fillStyle = "#0a0d11";
    g2.fillRect(0, 0, cw, THMAP_H);
    const L = layout();
    for (let t = 0; t < nT; t++) {
      const x = threadX(t);
      g2.fillStyle = "#111820";
      g2.fillRect(x, 2, L.threadW, THMAP_H - 4);
      if (L.threadW >= 6) {
        g2.globalAlpha = 0.9;
        for (const e of byThread[t]) {
          const y = 3 + ((e.s - info.minUs) / total) * (THMAP_H - 8);
          g2.fillStyle = colorFor(e.f);
          g2.fillRect(x + 1, y, L.threadW - 2, 1);
        }
        g2.globalAlpha = 1;
        // thread number
        g2.fillStyle = "#7d8a97";
        g2.font = "9px ui-monospace, monospace";
        g2.textAlign = "center";
        g2.fillText(String(t + 1), x + L.threadW / 2, THMAP_H - 2);
      }
    }
  }

  function drawThreadMap() {
    if (!tmapLayer) return;
    tmapCtx.setTransform(1, 0, 0, 1, -scrollX, 0);
    tmapCtx.clearRect(0, 0, tmap.width, tmap.height);
    tmapCtx.drawImage(tmapLayer, 0, 0);
    // visible viewport marker
    tmapCtx.setTransform(1, 0, 0, 1, 0, 0);
    tmapCtx.strokeStyle = "#4ea1ff";
    tmapCtx.lineWidth = 1;
    tmapCtx.strokeRect(scrollX + 0.5, 0.5, W - 1, THMAP_H - 1);
  }

  function drawMinimap() {
    if (!info) return;
    const w = mm.clientWidth, h = mm.clientHeight;
    mm.width = Math.round(w * dpr);
    mm.height = Math.round(h * dpr);
    mmctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    mmctx.clearRect(0, 0, w, h);
    if (mmLayer) mmctx.drawImage(mmLayer, 0, 0, w, h);
    const total = Math.max(1, info.maxUs - info.minUs);
    const vy1 = ((view.start - info.minUs) / total) * h;
    const vy2 = ((view.start + view.span - info.minUs) / total) * h;
    mmctx.fillStyle = "rgba(255,215,106,.10)";
    mmctx.fillRect(0, vy1, w, Math.max(2, vy2 - vy1));
    mmctx.strokeStyle = "#ffd76a";
    mmctx.lineWidth = 1.5;
    mmctx.strokeRect(0.5, vy1, w - 1, Math.max(2, vy2 - vy1));
    document.getElementById("mm-range").textContent = fmtUsAxis(view.start) + " – " + fmtUsAxis(view.start + view.span);
  }

  function setViewByMinimapY(yPix) {
    const h = mm.clientHeight;
    const total = Math.max(1, info.maxUs - info.minUs);
    const centerUs = info.minUs + (yPix / h) * total;
    view.start = clamp(centerUs - view.span / 2, info.minUs, info.maxUs - view.span);
    draw();
    drawMinimap();
  }

  // ---- hit testing (O(1) thread/depth from uniform grid) -----------------
  function eventAt(clientX, clientY) {
    const r = cv.getBoundingClientRect();
    const my = clientY - r.top;
    if (my < HEADER_H) return null;
    const cx = (clientX - r.left) + scrollX;   // content x
    const L = layout();
    if (cx < TIME_GUTTER) return null;
    const t = Math.floor((cx - TIME_GUTTER) / (L.threadW + THREAD_GAP));
    if (t < 0 || t >= L.nT) return null;
    const tx = threadX(t);
    if (cx > tx + L.threadW) return null;       // in the gap
    const d = Math.floor((cx - tx) / L.colW);
    if (d >= L.levels) return null;
    const us = usOf(my);
    for (const e of byThread[t]) {
      if (e.d !== d) continue;
      if (e.s <= us && us < e.e) return e;
    }
    return null;
  }

  // ---- inspector ---------------------------------------------------------
  function showInspector(e) {
    if (!e) {
      inspector.innerHTML = '<div class="empty">click a call to inspect it</div>';
      return;
    }
    const chain = [];
    let cur = e;
    const byId = {};
    for (const x of byThread[e.t]) byId[x.i] = x;
    while (cur) {
      chain.unshift(cur);
      if (cur.p < 0) break;
      cur = byId[cur.p];
    }
    const dur = e.e - e.s;
    let html = '<div class="fname" style="color:' + colorFor(e.f) + '">' + escHtml(e.n) + "</div>";
    html += kv("thread", threads[e.t].name + " (#" + e.t + ")");
    html += kv("id", String(e.i));
    html += kv("depth", String(e.d));
    html += kv("start", fmtUs(e.s));
    html += kv("end", fmtUs(e.e));
    html += kv("duration", fmtUs(dur));
    html += '<div id="stack"><div class="depth" style="margin-top:8px;color:var(--muted)">call stack</div>';
    for (const c of chain) {
      html += '<div class="row"><span class="swatch" style="background:' + colorFor(c.f) + '"></span>' +
        '<span class="depth">└ ' + c.d + '</span> ' + escHtml(c.n) + "</div>";
    }
    html += "</div>";
    inspector.innerHTML = html;
  }
  function kv(k, v) {
    return '<div class="kv"><span>' + k + '</span><span>' + v + "</span></div>";
  }

  // ---- interaction: time pan / zoom / thread pan -------------------------
  let pan = null;
  let tmapNav = null;

  cv.addEventListener("mousedown", (ev) => {
    const r = cv.getBoundingClientRect();
    pan = {
      x: ev.clientX - r.left,
      y: ev.clientY - r.top,
      start: view.start,
      scrollX0: scrollX,
      h: ev.shiftKey,
      moved: false,
    };
    cv.classList.add("dragging");
  });

  window.addEventListener("mousemove", (ev) => {
    const r = cv.getBoundingClientRect();
    const mx = ev.clientX - r.left;
    const my = ev.clientY - r.top;

    if (pan) {
      if (pan.h) {
        // horizontal thread panning
        const dx = mx - pan.x;
        if (Math.abs(dx) > 3) pan.moved = true;
        scrollX = clamp(pan.scrollX0 - dx, 0, maxScrollX());
        draw();
        drawThreadMap();
        return;
      }
      const dy = my - pan.y;
      if (Math.abs(dy) > 3) pan.moved = true;
      // dragging down (dy>0) -> move window toward later time
      const dUs = (dy / H_plot) * view.span;
      view.start = clamp(pan.start + dUs, info.minUs, info.maxUs - view.span);
      draw();
      drawMinimap();
      return;
    }

    // hover tooltip
    if (ev.target === cv) {
      const e = eventAt(ev.clientX, ev.clientY);
      if (e !== hover) {
        hover = e;
        draw();
      }
      if (e) {
        tooltip.style.display = "block";
        tooltip.style.left = ev.clientX + 14 + "px";
        tooltip.style.top = ev.clientY + 14 + "px";
        tooltip.innerHTML = '<div class="t-fn">' + escHtml(e.n) + "</div>" +
          '<div class="t-row">thread <b>' + escHtml(threads[e.t].name) + " (#" + e.t + ")</b></div>" +
          '<div class="t-row">depth <b>' + e.d + '</b> · id <b>' + e.i + "</b></div>" +
          '<div class="t-row">' + fmtUs(e.s) + " → " + fmtUs(e.e) + "  (<b>" + fmtUs(e.e - e.s) + "</b>)</div>";
      } else {
        tooltip.style.display = "none";
      }
    } else {
      if (hover) { hover = null; draw(); }
      tooltip.style.display = "none";
    }
  });

  window.addEventListener("mouseup", (ev) => {
    if (pan) {
      const r = cv.getBoundingClientRect();
      cv.classList.remove("dragging");
      if (!pan.moved && !pan.h) {
        const e = eventAt(ev.clientX, ev.clientY);
        selected = e;
        draw();
        showInspector(e);
      }
      pan = null;
      return;
    }
    if (tmapNav) {
      tmapNav = null;
      cv.classList.remove("dragging");
    }
  });

  cv.addEventListener("wheel", (ev) => {
    ev.preventDefault();
    if (ev.shiftKey || Math.abs(ev.deltaX) > Math.abs(ev.deltaY)) {
      // horizontal thread panning
      const dx = Math.abs(ev.deltaX) > Math.abs(ev.deltaY) ? ev.deltaX : ev.deltaY;
      scrollX = clamp(scrollX + dx, 0, maxScrollX());
      draw();
      drawThreadMap();
      return;
    }
    const r = cv.getBoundingClientRect();
    const my = ev.clientY - r.top;
    const anchorUs = usOf(my);
    const factor = ev.deltaY > 0 ? 1.2 : 1 / 1.2;
    const span = clamp(view.span * factor, 1, info.maxUs - info.minUs);
    const start = clamp(anchorUs - ((my - HEADER_H) / H_plot) * span, info.minUs, info.maxUs - span);
    view.span = span;
    view.start = start;
    draw();
    drawMinimap();
  }, { passive: false });

  // ---- thread strip navigation -------------------------------------------
  tmap.addEventListener("mousedown", (ev) => {
    ev.preventDefault();
    tmapNav = { active: true };
    onTmapNav(ev);
  });
  window.addEventListener("mousemove", (ev) => { if (tmapNav && tmapNav.active) onTmapNav(ev); });
  function onTmapNav(ev) {
    const r = tmap.getBoundingClientRect();
    const cx = (ev.clientX - r.left) + scrollX;   // content x under cursor
    scrollX = clamp(cx - W / 2, 0, maxScrollX());
    draw();
    drawThreadMap();
  }

  // ---- minimap interaction ----------------------------------------------
  let mmDrag = false;
  mm.addEventListener("mousedown", (ev) => { mmDrag = true; onMmMove(ev); });
  window.addEventListener("mousemove", (ev) => { if (mmDrag) onMmMove(ev); });
  window.addEventListener("mouseup", () => { mmDrag = false; });
  function onMmMove(ev) {
    const r = mm.getBoundingClientRect();
    setViewByMinimapY(ev.clientY - r.top);
  }

  // ---- top controls ------------------------------------------------------
  document.getElementById("btn-zoomin").onclick = () => zoom(1 / 1.5);
  document.getElementById("btn-zoomout").onclick = () => zoom(1.5);
  document.getElementById("btn-fit").onclick = () => {
    view.start = info.minUs;
    view.span = Math.max(1, info.maxUs - info.minUs);
    scrollX = 0;
    draw(); drawMinimap(); drawThreadMap();
  };
  document.getElementById("btn-cols").onclick = () => {
    // fit ALL threads into the current width (may be very narrow)
    const L = layout();
    const avail = Math.max(60, W - TIME_GUTTER - THREAD_GAP * (L.nT - 1));
    colWUser = Math.max(2, Math.floor(avail / (L.nT * L.levels)));
    colMode = "manual";
    colwSlider.value = colWUser;
    scrollX = 0;
    buildLayers();
    resize();
  };
  colwSlider.addEventListener("input", (ev) => {
    colMode = "manual";
    colWUser = +ev.target.value;
    buildLayers();
    resize();
  });
  function zoom(f) {
    const center = view.start + view.span / 2;
    const span = clamp(view.span * f, 1, info.maxUs - info.minUs);
    view.span = span;
    view.start = clamp(center - span / 2, info.minUs, info.maxUs - span);
    draw();
    drawMinimap();
  }

  // play: sweep the time window across the trace
  document.getElementById("btn-play").onclick = (ev) => {
    playing = !playing;
    ev.target.textContent = playing ? "❚❚" : "▶";
    if (playing) {
      const frame = Math.max(16, 2000 / 60);
      playTimer = setInterval(() => {
        const stepUs = (info.maxUs - info.minUs) / 600;
        view.start += stepUs;
        if (view.start + view.span > info.maxUs) view.start = info.minUs;
        view.start = clamp(view.start, info.minUs, info.maxUs - view.span);
        draw(); drawMinimap();
      }, frame);
    } else if (playTimer) {
      clearInterval(playTimer);
      playTimer = null;
    }
  };

  document.getElementById("search").addEventListener("input", (ev) => {
    searchTerm = ev.target.value.trim().toLowerCase();
    draw();
  });

  // keyboard: arrows pan threads, up/down move time
  window.addEventListener("keydown", (ev) => {
    if (!info) return;
    const tag = (document.activeElement && document.activeElement.tagName) || "";
    if (tag === "INPUT" || tag === "TEXTAREA") return;
    const L = layout();
    if (ev.key === "ArrowRight") {
      scrollX = clamp(scrollX + (L.threadW + THREAD_GAP) * 2, 0, maxScrollX());
      draw(); drawThreadMap();
    } else if (ev.key === "ArrowLeft") {
      scrollX = clamp(scrollX - (L.threadW + THREAD_GAP) * 2, 0, maxScrollX());
      draw(); drawThreadMap();
    } else if (ev.key === "ArrowDown") {
      view.start = clamp(view.start + view.span / 10, info.minUs, info.maxUs - view.span);
      draw(); drawMinimap();
    } else if (ev.key === "ArrowUp") {
      view.start = clamp(view.start - view.span / 10, info.minUs, info.maxUs - view.span);
      draw(); drawMinimap();
    }
  });

  window.addEventListener("resize", resize);

  // ---- boot --------------------------------------------------------------
  load().catch((err) => { statusEl.textContent = "error: " + err.message; console.error(err); });
})();
