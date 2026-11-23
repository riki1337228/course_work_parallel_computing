// Web UI: talks to the backend through the WebSocket /ws/search (search pipeline
// stages) and polls GET /api/health and GET /api/stats for the status bar.
(() => {
  "use strict";

  const $ = (id) => document.getElementById(id);
  const form = $("searchForm");
  const input = $("queryInput");
  const button = $("searchButton");
  const expandToggle = $("expandToggle");
  const limitSelect = $("limitSelect");
  const statusLine = $("statusLine");
  const termsBox = $("termsBox");
  const termsList = $("termsList");
  const resultsHeader = $("resultsHeader");
  const resultsTitle = $("resultsTitle");
  const resultsMeta = $("resultsMeta");
  const resultsEl = $("results");
  const template = $("resultTemplate");
  const steps = { expansion: $("stepExpansion"), search: $("stepSearch"), done: $("stepDone") };
  const lines = document.querySelectorAll(".step__line");

  const STOP_WORDS = new Set(("a an and are as at be by for from has have in is it of on or that the this to was were " +
    "will with").split(" "));

  let socket = null;
  let pending = null;   // {query, startedAt, terms}
  let searching = false;

  // ------------------------------------------------------------------ status bar
  async function refreshStatus() {
    const ollamaPill = $("ollamaStatus");
    const docsPill = $("docsStatus");
    let stats = null;
    try {
      stats = await fetch("/api/stats", { cache: "no-store" }).then((r) => r.json());
    } catch {
      /* handled below */
    }
    try {
      const health = await fetch("/api/health", { cache: "no-store" }).then((r) => r.json());
      const model = stats && stats.ollama_model ? ` (${stats.ollama_model})` : "";
      ollamaPill.className = "pill " + (health.ollama ? "pill--ok" : "pill--warn");
      ollamaPill.querySelector(".pill__label").textContent =
        health.ollama ? `Ollama: online${model}` : "Ollama: unavailable";
    } catch {
      ollamaPill.className = "pill pill--warn";
      ollamaPill.querySelector(".pill__label").textContent = "Server: offline";
    }
    try {
      if (!stats) throw new Error("no stats");
      const label = docsPill.querySelector(".pill__label");
      if (stats.indexing_in_progress && !stats.index_ready) {
        label.textContent = "Index: building…";
      } else {
        label.textContent = `Documents: ${Number(stats.documents).toLocaleString("en-US")}`;
      }
      docsPill.title = `Index build: ${stats.index_build_time_ms} ms, threads: ${stats.threads_used}, ` +
        `terms: ${stats.unique_terms}, searches served: ${stats.searches_served}`;
    } catch {
      /* server offline: already shown above */
    }
  }

  // ------------------------------------------------------------------ pipeline view
  function setStep(name, state) {
    const el = steps[name];
    el.classList.remove("step--active", "step--done", "step--fallback", "step--error");
    if (state) el.classList.add("step--" + state);
  }

  function resetPipeline() {
    Object.keys(steps).forEach((k) => setStep(k, null));
    lines.forEach((l) => l.classList.remove("step__line--done"));
    termsBox.hidden = true;
    termsList.replaceChildren();
  }

  function setStatus(text, kind) {
    statusLine.textContent = text;
    statusLine.className = "pipeline__status" + (kind ? " pipeline__status--" + kind : "");
  }

  function setSearching(value) {
    searching = value;
    button.disabled = value;
    button.textContent = value ? "Searching…" : "Search";
  }

  // ------------------------------------------------------------------ rendering
  function escapeHtml(text) {
    return text.replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
  }

  function tokens(text) {
    return text.toLowerCase().split(/[^a-z0-9]+/).filter((t) => t.length >= 2 && !STOP_WORDS.has(t));
  }

  function highlight(snippet, terms) {
    let html = escapeHtml(snippet);
    if (!terms.length) return html;
    const pattern = terms.map((t) => t.replace(/[.*+?^${}()|[\]\\]/g, "\\$&")).join("|");
    return html.replace(new RegExp(`\\b(${pattern})\\b`, "gi"), "<mark>$1</mark>");
  }

  function renderTerms(expansion) {
    termsList.replaceChildren();
    if (expansion.status !== "ok" || !expansion.expanded_terms.length) {
      termsBox.hidden = true;
      return;
    }
    expansion.expanded_terms.forEach((term) => {
      const chip = document.createElement("span");
      chip.className = "chip";
      chip.textContent = term;
      termsList.appendChild(chip);
    });
    termsBox.hidden = false;
  }

  function renderResults(data) {
    resultsEl.replaceChildren();
    resultsHeader.hidden = false;
    const total = Number(data.total_matches || 0).toLocaleString("en-US");
    resultsTitle.textContent = `Results for “${pending ? pending.query : ""}”`;
    resultsMeta.textContent = `${total} matching documents · index search ${data.time_ms} ms`;

    if (!data.results.length) {
      const empty = document.createElement("div");
      empty.className = "card empty";
      empty.textContent = "No documents matched the query.";
      resultsEl.appendChild(empty);
      return;
    }
    const maxScore = data.results[0].score || 1;
    data.results.forEach((r, i) => {
      const node = template.content.firstElementChild.cloneNode(true);
      node.style.animationDelay = `${Math.min(i, 10) * 25}ms`;
      node.querySelector(".result__rank").textContent = i + 1;
      node.querySelector(".result__doc").textContent = r.doc_id;
      node.querySelector(".score__value").textContent = `score ${r.score.toFixed(6)}`;
      node.querySelector(".bar__fill").style.width = `${Math.max(4, (r.score / maxScore) * 100)}%`;
      node.querySelector(".result__snippet").innerHTML = highlight(r.snippet, pending ? pending.terms : []);
      resultsEl.appendChild(node);
    });
  }

  // ------------------------------------------------------------------ WebSocket
  function connect() {
    return new Promise((resolve, reject) => {
      if (socket && socket.readyState === WebSocket.OPEN) return resolve(socket);
      const scheme = location.protocol === "https:" ? "wss" : "ws";
      const ws = new WebSocket(`${scheme}://${location.host}/ws/search`);
      ws.onopen = () => { socket = ws; resolve(ws); };
      ws.onerror = () => reject(new Error("WebSocket connection failed"));
      ws.onclose = () => {
        if (socket === ws) socket = null;
        if (searching) {
          setStatus("Connection to the server was lost.", "error");
          setStep("search", "error");
          setSearching(false);
        }
      };
      ws.onmessage = (event) => handleMessage(event.data);
    });
  }

  function handleMessage(raw) {
    let msg;
    try { msg = JSON.parse(raw); } catch { return; }
    switch (msg.stage) {
      case "query_expansion": {
        const exp = msg.data;
        if (pending) pending.terms = pending.terms.concat(exp.expanded_terms.flatMap(tokens));
        if (exp.status === "fallback") {
          setStep("expansion", "fallback");
          const reason = exp.fallback_reason ? ` (${exp.fallback_reason})` : "";
          setStatus(`Ollama unavailable — fallback mode${reason}. Searching index…`, "warn");
        } else {
          setStep("expansion", "done");
          setStatus(exp.status === "disabled" ? "Query expansion disabled. Searching index…" : "Searching index…", "busy");
        }
        lines[0].classList.add("step__line--done");
        setStep("search", "active");
        renderTerms(exp);
        break;
      }
      case "keyword_search":
        setStep("search", "done");
        lines[1].classList.add("step__line--done");
        setStep("done", "active");
        renderResults(msg.data);
        break;
      case "done": {
        setStep("done", "done");
        const seconds = pending ? ((performance.now() - pending.startedAt) / 1000).toFixed(2) : "?";
        const fallback = steps.expansion.classList.contains("step--fallback");
        setStatus(fallback ? `Search completed in ${seconds} s (Ollama unavailable — fallback mode).`
                           : `Search completed in ${seconds} s.`, fallback ? "warn" : "ok");
        setSearching(false);
        break;
      }
      case "error": {
        const err = msg.data || {};
        const active = Object.keys(steps).find((k) => steps[k].classList.contains("step--active")) || "expansion";
        setStep(active, "error");
        setStatus(`Error: ${err.message || err.error || "unknown error"}`, "error");
        setSearching(false);
        break;
      }
      default:
        break;
    }
  }

  async function search(query) {
    query = query.trim();
    if (!query || searching) return;
    resetPipeline();
    setSearching(true);
    pending = { query, startedAt: performance.now(), terms: tokens(query) };
    setStatus("Searching…", "busy");
    try {
      const ws = await connect();
      const expand = expandToggle.checked;
      setStep("expansion", "active");
      setStatus(expand ? "Expanding query…" : "Searching index…", "busy");
      ws.send(JSON.stringify({ query, limit: Number(limitSelect.value), expand }));
    } catch (e) {
      setStatus(`Error: ${e.message}`, "error");
      setSearching(false);
    }
  }

  // ------------------------------------------------------------------ events
  form.addEventListener("submit", (e) => { e.preventDefault(); search(input.value); });
  document.querySelectorAll("[data-query]").forEach((el) =>
    el.addEventListener("click", () => { input.value = el.dataset.query; search(el.dataset.query); }));

  refreshStatus();
  setInterval(refreshStatus, 5000);
  input.focus();

  // Deep link: /?q=<query> runs the search immediately (handy for demos).
  const initialQuery = new URLSearchParams(location.search).get("q");
  if (initialQuery) {
    input.value = initialQuery;
    search(initialQuery);
  }
})();
