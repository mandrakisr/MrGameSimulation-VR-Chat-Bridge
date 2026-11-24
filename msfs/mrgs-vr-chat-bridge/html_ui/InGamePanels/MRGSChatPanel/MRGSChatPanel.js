/* MrGameSimulations VR Chat Bridge — MSFS 2024 toolbar panel (v1.0.0).
 *
 * A standalone Social Stream Ninja chat viewer any streamer can use:
 * save your own SSN session ID once (localStorage, this PC only) and
 * the panel joins wss://io.socialstream.ninja/join/<SESSION_ID>/4 —
 * channel 4 is the consolidated chat of every platform connected in
 * SSN (Twitch / YouTube / Kick / TikTok / Facebook / Trovo). Read-only.
 * The session ID is a secret: never logged, never rendered back; the
 * join URL is built in exactly one place (chatConnect).
 *
 * The panel framework wrapper, the SU2 hide-class fix, the message
 * normalization and the reconnect discipline mirror the proven reference
 * toolbar panel's Stream Chat implementation. The socket lives only
 * while the chat view is open; a dropped socket reconnects with
 * backoff (1s doubling to 15s). Incoming messages are ONE append-only
 * DOM insert built with textContent — message text is never parsed
 * as HTML — and the store is capped at the latest 100.
 *
 * Background opacity: a "Background" slider (0–100%, default 85%,
 * persisted as MRGS_BG_OPACITY) drives the --mrgs-bg-alpha CSS
 * variable. v1.0.1: EVERYTHING except text consumes it (surfaces,
 * buttons, pill, tags, input, slider) — at 0% only the words float
 * over the sim; text and names always stay fully opaque. */

/* ---------------- Panel framework wrapper ----------------
 * Find the <ingame-ui>, hide the stock #MainDisplay / #Footer
 * sections, subscribe to the panel lifecycle events, and enable the
 * stock header — fs-base-ui hides each header button until the panel
 * is told it is active / collapsible / closeable / externalizable,
 * and a .spb-registered panel never receives the engine's
 * SetupPanelInfos, so the JS must set those itself. Every framework
 * call is guarded: a failure here can never kill the app below. */
if (typeof TemplateElement !== "undefined" && window.customElements &&
    !window.customElements.get("ingamepanel-custom")) {
  class IngamePanelCustom extends TemplateElement {
    constructor() {
      super(...arguments);
      this.ingameUi = null;
      this.mrgsWired = false;
    }

    connectedCallback() {
      try {
        super.connectedCallback();
      } catch (e) {
        /* framework base unavailable — the app below still runs */
      }
      this.mrgsWirePanel();
    }

    disconnectedCallback() {
      try {
        super.disconnectedCallback();
      } catch (e) {
        /* guarded, see connectedCallback */
      }
    }

    /* Find the <ingame-ui>, hide the stock sections and subscribe to
     * the lifecycle events — once. The element can connect before its
     * light DOM is parsed, so retry briefly when it isn't there yet. */
    mrgsWirePanel(retries) {
      if (this.mrgsWired) return;
      const ui = this.querySelector ? this.querySelector("ingame-ui") : null;
      if (!ui) {
        const left = typeof retries === "number" ? retries : 10;
        if (left > 0) setTimeout(() => this.mrgsWirePanel(left - 1), 250);
        return;
      }
      this.ingameUi = ui;
      this.mrgsWired = true;
      mrgsHideStockSections();
      ui.addEventListener("panelActive", () => {
        mrgsUnhidePanelUi();
        this.mrgsEnableStockHeader();
      });
      ui.addEventListener("panelInactive", () => {
        /* lifecycle anchor — the app renders on demand */
      });
      ui.addEventListener("onResizeElement", () => {
        /* lifecycle anchor — layout adapts via CSS */
      });
      this.mrgsEnableStockHeader();
      setTimeout(() => this.mrgsEnableStockHeader(), 250);
    }

    /* fs-base-ui's ingameUiHeader hides each stock button until it is
     * told the panel is active, collapsible, closeable and
     * externalizable. Only collapsible has an attribute; closeable and
     * externalizable are runtime states set here — on connect, on a
     * 250 ms retry, and again on every panelActive. */
    mrgsEnableStockHeader() {
      try {
        const ui = this.ingameUi;
        if (!ui) return;
        ui.setAttribute("title", "MrGameSimulations VR Chat Bridge");
        ui.setAttribute("collapsible", "true");
        const header =
          ui.headerElement || (ui.querySelector ? ui.querySelector("ingame-ui-header") : null);
        if (!header) return;
        if (typeof header.setActive === "function") header.setActive(true);
        if (typeof header.setForcedAttach === "function") header.setForcedAttach(false);
        if (typeof header.setCollapsible === "function") header.setCollapsible(true);
        if (typeof header.setCloseable === "function") header.setCloseable(true);
        const vr = typeof g_externalVariables !== "undefined" && g_externalVariables.vrMode;
        if (typeof header.setExternalizable === "function") header.setExternalizable(!vr);
      } catch (e) {
        /* header chrome is framework-owned; never fatal */
      }
    }
  }
  window.customElements.define("ingamepanel-custom", IngamePanelCustom);
}

/* Hide the stock #MainDisplay / #Footer template sections — they ship
 * in the HTML already classed hidden; this keeps them hidden if the
 * framework re-renders them. */
function mrgsHideStockSections() {
  try {
    const main = document.getElementById("MainDisplay");
    if (main && main.classList) main.classList.add("hidden");
    const footer = document.getElementById("Footer");
    if (footer && footer.classList) footer.classList.add("hidden");
  } catch (e) {
    /* never fatal */
  }
}

/* MSFS 2024 SU2 framework fix — the `hide` / `panelInvisible` classes.
 * Since SU2, Asobo adds a `hide` class to every <ingame-ui> element,
 * and the panel's own JS is responsible for removing it. If the panel
 * should also be draggable, `panelInvisible` must be removed as well —
 * but ONLY when `hide` is present. The framework can add the classes
 * after this script loads, so the check runs on DOMContentLoaded,
 * again on the element's "panelActive" event, and on two timed
 * retries (~600 ms / ~1800 ms) in case the classes land late. */
function mrgsUnhidePanelUi() {
  const ui = document.querySelector("ingame-ui");
  if (!ui || !ui.classList || !ui.classList.contains("hide")) return;
  ui.classList.remove("hide");
  ui.classList.remove("panelInvisible");
}
(function wirePanelUnhide() {
  function arm() {
    const ui = document.querySelector("ingame-ui");
    if (ui && ui.addEventListener) {
      ui.addEventListener("panelActive", mrgsUnhidePanelUi);
    }
    mrgsUnhidePanelUi();
    setTimeout(mrgsUnhidePanelUi, 600);
    setTimeout(mrgsUnhidePanelUi, 1800);
  }
  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", arm);
  } else {
    arm();
  }
})();

/* ---------------- VR Chat app ---------------- */
(function () {
  "use strict";

  const SSN_WS_BASE = "wss://io.socialstream.ninja/join/";
  const STORAGE_KEY = "MRGS_SSN_SESSION";
  const CHAT_CAP = 100; // keep the latest ~100 messages
  const PLATFORMS = [
    ["twitch", "Twitch"],
    ["youtube", "YouTube"],
    ["kick", "Kick"],
    ["tiktok", "TikTok"],
    ["facebook", "Facebook"],
    ["trovo", "Trovo"],
  ];
  const PLATFORM_LABELS = {};
  for (const [pk, pl] of PLATFORMS) PLATFORM_LABELS[pk] = pl;
  const CHAT_ICON =
    '<svg class="mrgs-chat-icon" viewBox="0 0 24 24" width="24" height="24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M21 11.5a8.38 8.38 0 0 1-.9 3.8 8.5 8.5 0 0 1-7.6 4.7 8.38 8.38 0 0 1-3.8-.9L3 21l1.9-5.7a8.38 8.38 0 0 1-.9-3.8 8.5 8.5 0 0 1 4.7-7.6 8.38 8.38 0 0 1 3.8-.9h.5a8.48 8.48 0 0 1 8 8v.5z"/></svg>';
  const CHEVRON_ICON =
    '<svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><polyline points="6 9 12 15 18 9"/></svg>';

  const chat = {
    view: "setup", // setup | chat
    phase: "idle", // idle | connecting | live | offline
    active: false, // true only while the chat view is open
    socket: null,
    messages: [], // normalized { name, text, platform, platformLabel }
    failCount: 0,
    reconnectTimer: null,
    stickBottom: true, // auto-scroll follows the newest message
  };

  /* ---------- storage ---------- */
  function getSessionId() {
    try {
      return (window.localStorage.getItem(STORAGE_KEY) || "").trim();
    } catch (e) {
      return "";
    }
  }
  function saveSessionId(id) {
    try {
      if (id) window.localStorage.setItem(STORAGE_KEY, id);
      else window.localStorage.removeItem(STORAGE_KEY);
    } catch (e) {
      /* storage unavailable — the ID simply won't persist */
    }
  }

  /* ---------- background opacity ----------
   * 0–100%, default 85. Drives the --mrgs-bg-alpha CSS variable;
   * everything except text consumes it (v1.0.1). Persisted in
   * localStorage alongside the session ID and re-applied at boot. */
  const BG_KEY = "MRGS_BG_OPACITY";
  const BG_DEFAULT = 85;
  function getBgOpacity() {
    let v = BG_DEFAULT;
    try {
      const raw = window.localStorage.getItem(BG_KEY);
      if (raw !== null && raw !== undefined && raw !== "") v = parseInt(raw, 10);
    } catch (e) {
      /* storage unavailable — default stands */
    }
    if (!Number.isFinite(v)) v = BG_DEFAULT;
    return Math.max(0, Math.min(100, v));
  }
  function applyBgOpacity(v) {
    const alpha = (Math.max(0, Math.min(100, v)) / 100).toFixed(2);
    try {
      document.documentElement.style.setProperty("--mrgs-bg-alpha", alpha);
    } catch (e) {
      /* no documentElement — nothing to fade */
    }
    const root = document.getElementById("mrgs-root");
    if (root && root.style) root.style.setProperty("--mrgs-bg-alpha", alpha);
    try {
      window.localStorage.setItem(BG_KEY, String(Math.round(v)));
    } catch (e) {
      /* storage unavailable — the setting lasts for this session only */
    }
    const val = document.getElementById("mrgs-bg-val");
    if (val) val.textContent = Math.round(v) + "%";
  }
  function opacityRowHtml() {
    const v = getBgOpacity();
    return (
      '<div class="mrgs-opacity-row">' +
      '<label class="mrgs-opacity-label" for="mrgs-bg-slider">Background</label>' +
      '<input class="mrgs-slider" id="mrgs-bg-slider" type="range" min="0" max="100" step="1" value="' + v + '" />' +
      '<span class="mrgs-opacity-val" id="mrgs-bg-val">' + v + "%</span></div>"
    );
  }
  function wireOpacitySlider() {
    const slider = document.getElementById("mrgs-bg-slider");
    if (!slider) return;
    slider.addEventListener("input", () => {
      applyBgOpacity(parseInt(slider.value, 10) || 0);
    });
  }

  /* ---------- message normalization (mirrors the proven pattern) --- */
  function codePoint(n) {
    return Number.isFinite(n) && n > 0 && n <= 0x10ffff ? String.fromCodePoint(n) : "";
  }
  function chatText(html) {
    // SSN chatmessage may carry HTML (emote <img> tags). Render TEXT
    // ONLY: an <img> becomes its alt text, every other tag is
    // stripped, basic entities are decoded (numeric first, &amp;
    // last so nothing double-decodes).
    let s = String(html == null ? "" : html);
    s = s.replace(/<img\b[^>]*>/gi, (tag) => {
      const m = /\balt\s*=\s*"([^"]*)"/i.exec(tag) || /\balt\s*=\s*'([^']*)'/i.exec(tag);
      return m ? " " + m[1] + " " : " ";
    });
    s = s.replace(/<[^>]*>/g, " ");
    s = s
      .replace(/&#x([0-9a-fA-F]+);/g, (m, h) => codePoint(parseInt(h, 16)))
      .replace(/&#(\d+);/g, (m, d) => codePoint(parseInt(d, 10)))
      .replace(/&nbsp;/gi, " ")
      .replace(/&lt;/gi, "<")
      .replace(/&gt;/gi, ">")
      .replace(/&quot;/gi, '"')
      .replace(/&apos;/gi, "'")
      .replace(/&amp;/gi, "&");
    return s.replace(/\s+/g, " ").trim();
  }
  function platformKey(type) {
    return String(type == null ? "" : type).trim().toLowerCase() || "chat";
  }
  function platformLabel(key) {
    if (PLATFORM_LABELS[key]) return PLATFORM_LABELS[key];
    return key ? key.charAt(0).toUpperCase() + key.slice(1) : "Chat";
  }
  function normalizeChat(raw) {
    if (!raw || typeof raw !== "object") return null;
    if (typeof raw.chatmessage !== "string" || !raw.chatmessage.trim()) return null;
    const text = chatText(raw.chatmessage);
    if (!text) return null;
    const key = platformKey(raw.type);
    return {
      name: String(raw.chatname == null ? "" : raw.chatname).trim() || "Viewer",
      text,
      platform: key,
      platformLabel: platformLabel(key),
    };
  }
  function nearBottom(el) {
    if (!el) return true;
    return el.scrollHeight - el.scrollTop - el.clientHeight <= 80;
  }
  function esc(s) {
    return String(s == null ? "" : s)
      .replace(/&/g, "&amp;")
      .replace(/</g, "&lt;")
      .replace(/>/g, "&gt;")
      .replace(/"/g, "&quot;");
  }

  /* ---------- status ---------- */
  function pillFor(phase) {
    switch (phase) {
      case "live": return { cls: "on", text: "Live" };
      case "connecting": return { cls: "wait", text: "Connecting" };
      case "offline": return { cls: "off", text: "Disconnected" };
      default: return { cls: "", text: "Offline" };
    }
  }
  function statusLine() {
    switch (chat.phase) {
      case "live":
        return "Status: <b>Connected</b> — receiving chat from Social Stream Ninja · " +
          chat.messages.length + (chat.messages.length === 1 ? " message" : " messages");
      case "connecting":
        return "Status: <b>Connecting</b> to Social Stream Ninja…";
      case "offline":
        return "Status: <b>Disconnected</b> — reconnecting automatically. If it stays disconnected, check the Social Stream Ninja toggles: <b>Enable remote API control</b> and <b>Send chat messages to API server</b>.";
      default:
        return "Status: <b>Offline</b>";
    }
  }
  function refreshHead() {
    const pillEl = document.getElementById("mrgs-pill");
    if (pillEl) {
      const pill = pillFor(chat.phase);
      pillEl.className = "mrgs-livepill " + pill.cls;
      pillEl.innerHTML = '<span class="mrgs-pulse"></span>' + pill.text;
    }
    const lineEl = document.getElementById("mrgs-statusline");
    if (lineEl) lineEl.innerHTML = statusLine();
  }

  /* ---------- views ---------- */
  function setupHtml() {
    const id = getSessionId();
    return (
      '<div class="mrgs-main">' +
      '<div class="mrgs-head"><span class="mrgs-title">' + CHAT_ICON + "<span>VR Chat Bridge</span></span></div>" +
      '<div class="mrgs-card">' +
      '<div class="mrgs-setup-title">Connect your stream chat</div>' +
      '<div class="mrgs-setup-sub">Your consolidated Social Stream Ninja chat — Twitch, YouTube, Kick, TikTok, Facebook, Trovo — readable in the cockpit, sized for VR.</div>' +
      '<label class="mrgs-field-label" for="mrgs-session-input">Social Stream Ninja session ID</label>' +
      '<input class="mrgs-input" id="mrgs-session-input" type="text" autocomplete="off" spellcheck="false" placeholder="Paste your session ID" value="' + esc(id) + '" />' +
      '<div class="mrgs-setup-actions">' +
      '<button type="button" class="mrgs-btn" id="mrgs-save-btn">Save &amp; Connect</button>' +
      (id ? '<button type="button" class="mrgs-btn mrgs-btn-secondary" id="mrgs-back-btn">Back to chat</button>' : "") +
      "</div>" +
      opacityRowHtml() +
      '<div class="mrgs-step"><span class="mrgs-stepnum">1</span><span>In Social Stream Ninja, open <b>Global settings</b>, then <b>Mechanics</b> — turn ON <b>Enable remote API control</b> and <b>Send chat messages to API server</b>.</span></div>' +
      '<div class="mrgs-step"><span class="mrgs-stepnum">2</span><span>Copy your session ID — it is in the Social Stream Ninja popup, or in your dock link after <b>?session=</b>.</span></div>' +
      '<div class="mrgs-step"><span class="mrgs-stepnum">3</span><span>Paste it above and tap <b>Save &amp; Connect</b>. It is stored only on this PC and never sent anywhere except Social Stream Ninja.</span></div>' +
      "</div>" +
      '<div class="mrgs-ver">MrGameSimulations VR Chat Bridge · v1.0.0</div>' +
      "</div>"
    );
  }
  function rowHtml(msg) {
    return (
      '<div class="mrgs-chat-row"><div class="mrgs-chat-who">' +
      '<span class="mrgs-chat-tag mrgs-chat-tag-' + esc(msg.platform) + '">' + esc(msg.platformLabel) + "</span>" +
      '<span class="mrgs-chat-name">' + esc(msg.name) + "</span></div>" +
      '<div class="mrgs-chat-text">' + esc(msg.text) + "</div></div>"
    );
  }
  function chatHtml() {
    const pill = pillFor(chat.phase);
    const rows = chat.messages.length
      ? chat.messages.map(rowHtml).join("")
      : '<div class="mrgs-note" id="mrgs-chat-empty">No messages yet — chat from your streams will appear here as it comes in.</div>';
    return (
      '<div class="mrgs-main">' +
      '<div class="mrgs-head"><span class="mrgs-title">' + CHAT_ICON + "<span>Stream Chat</span></span>" +
      '<span class="mrgs-head-spacer"></span>' +
      '<span class="mrgs-livepill ' + pill.cls + '" id="mrgs-pill"><span class="mrgs-pulse"></span>' + pill.text + "</span></div>" +
      '<div class="mrgs-statusline" id="mrgs-statusline">' + statusLine() + "</div>" +
      opacityRowHtml() +
      '<div class="mrgs-chat-scroll" id="mrgs-chat-scroll"><div id="mrgs-chat-rows">' + rows + "</div></div>" +
      '<button type="button" class="mrgs-btn mrgs-btn-secondary mrgs-chat-jump mrgs-chat-jump-hidden" id="mrgs-chat-jump">' + CHEVRON_ICON + "<span>Jump to latest</span></button>" +
      '<div class="mrgs-actions">' +
      '<button type="button" class="mrgs-btn mrgs-btn-secondary mrgs-btn-small" id="mrgs-settings-btn">Settings</button>' +
      '<button type="button" class="mrgs-btn mrgs-btn-secondary mrgs-btn-small" id="mrgs-clear-btn">Clear</button>' +
      '<button type="button" class="mrgs-btn mrgs-btn-secondary mrgs-btn-small" id="mrgs-reconnect-btn">Reconnect</button>' +
      "</div></div>"
    );
  }

  /* ---------- rendering ---------- */
  function render() {
    const view = document.getElementById("mrgs-view");
    if (!view) return;
    view.innerHTML = chat.view === "chat" ? chatHtml() : setupHtml();
    if (chat.view === "chat") {
      wireChatView();
    } else {
      wireSetupView();
    }
  }
  function wireSetupView() {
    wireOpacitySlider();
    const save = document.getElementById("mrgs-save-btn");
    if (save) {
      save.addEventListener("click", () => {
        const input = document.getElementById("mrgs-session-input");
        const id = input ? input.value.trim() : "";
        if (!id) {
          if (input) input.focus();
          return;
        }
        saveSessionId(id);
        chat.view = "chat";
        chat.active = true;
        render();
        chatConnect(true);
      });
    }
    const back = document.getElementById("mrgs-back-btn");
    if (back) {
      back.addEventListener("click", () => {
        chat.view = "chat";
        chat.active = true;
        render();
        chatConnect(false);
      });
    }
    const input = document.getElementById("mrgs-session-input");
    if (input) {
      input.addEventListener("keydown", (e) => {
        if (e.key === "Enter" && save) save.click();
      });
    }
  }
  function wireChatView() {
    wireOpacitySlider();
    const scroll = document.getElementById("mrgs-chat-scroll");
    if (scroll) {
      scroll.addEventListener("scroll", () => {
        const near = nearBottom(scroll);
        chat.stickBottom = near;
        jumpShow(!near);
      });
      if (chat.stickBottom) scroll.scrollTop = scroll.scrollHeight;
      jumpShow(!chat.stickBottom);
    }
    const jump = document.getElementById("mrgs-chat-jump");
    if (jump) {
      jump.addEventListener("click", () => {
        chat.stickBottom = true;
        if (scroll) scroll.scrollTop = scroll.scrollHeight;
        jumpShow(false);
      });
    }
    const settings = document.getElementById("mrgs-settings-btn");
    if (settings) {
      settings.addEventListener("click", () => {
        chatDisconnect();
        chat.view = "setup";
        render();
      });
    }
    const clear = document.getElementById("mrgs-clear-btn");
    if (clear) {
      clear.addEventListener("click", () => {
        chat.messages = [];
        const rows = document.getElementById("mrgs-chat-rows");
        if (rows) {
          rows.innerHTML =
            '<div class="mrgs-note" id="mrgs-chat-empty">No messages yet — chat from your streams will appear here as it comes in.</div>';
        }
        refreshHead();
      });
    }
    const reconnect = document.getElementById("mrgs-reconnect-btn");
    if (reconnect) {
      reconnect.addEventListener("click", () => {
        chatConnect(true);
      });
    }
  }
  function jumpShow(show) {
    const b = document.getElementById("mrgs-chat-jump");
    if (b) b.classList.toggle("mrgs-chat-jump-hidden", !show);
  }
  function scrollFollow() {
    const el = document.getElementById("mrgs-chat-scroll");
    if (!el) return;
    if (chat.stickBottom) el.scrollTop = el.scrollHeight;
    else jumpShow(true);
  }

  /* ---------- message path: ONE append-only DOM insert ---------- */
  function rowNode(msg) {
    const row = document.createElement("div");
    row.className = "mrgs-chat-row mrgs-chat-row-new";
    const who = document.createElement("div");
    who.className = "mrgs-chat-who";
    const tag = document.createElement("span");
    tag.className = "mrgs-chat-tag mrgs-chat-tag-" + msg.platform;
    tag.textContent = msg.platformLabel;
    const name = document.createElement("span");
    name.className = "mrgs-chat-name";
    name.textContent = msg.name;
    who.appendChild(tag);
    who.appendChild(name);
    const body = document.createElement("div");
    body.className = "mrgs-chat-text";
    body.textContent = msg.text;
    row.appendChild(who);
    row.appendChild(body);
    return row;
  }
  function receive(msg) {
    chat.messages = chat.messages.concat(msg);
    if (chat.messages.length > CHAT_CAP) {
      chat.messages = chat.messages.slice(chat.messages.length - CHAT_CAP);
    }
    if (chat.view !== "chat") return;
    refreshHead();
    const rows = document.getElementById("mrgs-chat-rows");
    if (!rows) return;
    const empty = document.getElementById("mrgs-chat-empty");
    if (empty && empty.parentNode) empty.parentNode.removeChild(empty);
    rows.appendChild(rowNode(msg));
    while (rows.children.length > CHAT_CAP && rows.firstElementChild) {
      rows.removeChild(rows.firstElementChild);
    }
    scrollFollow();
  }

  /* ---------- socket ---------- */
  function chatConnect(force) {
    const c = chat;
    if (!c.active) return;
    const id = getSessionId();
    if (!id) {
      c.view = "setup";
      render();
      return;
    }
    if (c.socket && (c.socket.readyState === 0 || c.socket.readyState === 1)) {
      if (!force) return;
      try { c.socket.close(); } catch (e) { /* replaced below */ }
      c.socket = null;
    }
    if (typeof WebSocket === "undefined") {
      c.phase = "offline";
      refreshHead();
      return;
    }
    let ws;
    try {
      // The session ID is used only here — it is never logged.
      ws = new WebSocket(SSN_WS_BASE + encodeURIComponent(id) + "/4");
    } catch (e) {
      c.failCount += 1;
      c.phase = "offline";
      scheduleReconnect();
      refreshHead();
      return;
    }
    c.socket = ws;
    c.phase = "connecting";
    refreshHead();
    ws.onopen = () => {
      if (c.socket !== ws) return;
      c.failCount = 0;
      c.phase = "live";
      refreshHead();
    };
    ws.onmessage = (ev) => {
      if (c.socket !== ws) return;
      let raw = null;
      try {
        raw = JSON.parse(ev.data);
      } catch (e) {
        raw = null;
      }
      const msg = normalizeChat(raw);
      if (msg) receive(msg);
    };
    ws.onclose = () => {
      if (c.socket !== ws) return; // a replaced socket's close is not news
      c.socket = null;
      if (c.active) {
        c.failCount += 1;
        c.phase = "offline";
        scheduleReconnect();
      }
      refreshHead();
    };
    ws.onerror = () => {
      /* the close handler does the bookkeeping; an error just precedes it */
    };
  }
  function scheduleReconnect() {
    const c = chat;
    if (!c.active || c.reconnectTimer) return;
    // Backoff: 1s doubling to a 15s cap.
    const delay = Math.min(15000, 1000 * Math.pow(2, Math.min(c.failCount, 4)));
    c.reconnectTimer = setTimeout(() => {
      c.reconnectTimer = null;
      if (!c.active) return;
      c.phase = "connecting";
      refreshHead();
      chatConnect(false);
    }, delay);
  }
  function chatDisconnect() {
    const c = chat;
    c.active = false;
    if (c.reconnectTimer) {
      clearTimeout(c.reconnectTimer);
      c.reconnectTimer = null;
    }
    if (c.socket) {
      try { c.socket.close(); } catch (e) { /* already gone */ }
      c.socket = null;
    }
    c.phase = "idle";
  }

  /* ---------- boot ---------- */
  function boot() {
    applyBgOpacity(getBgOpacity());
    if (getSessionId()) {
      chat.view = "chat";
      chat.active = true;
      render();
      chatConnect(false);
    } else {
      chat.view = "setup";
      render();
    }
  }
  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", boot);
  } else {
    boot();
  }
})();

/* Panel framework hook — must stay at the end of the file. Guarded so
 * the file also loads cleanly outside the sim (tests, a plain browser). */
try {
  if (typeof checkAutoload === "function") checkAutoload();
} catch (e) {
  /* outside the sim there is no autoload — the app above still runs */
}
