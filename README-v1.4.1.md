# MrGameSimulation VR Chat Bridge — v1.4.1

**Your live stream chat, inside X-Plane 12 — on the desktop and in VR.**

By **Mandraki Bey** · **MrGameSimulation**

The MrGameSimulation VR Chat Bridge connects X-Plane 12 to [Social Stream Ninja](https://socialstream.ninja/) and shows your combined chat — Twitch, YouTube, Kick, TikTok, Facebook, Trovo and more — in a clean floating window right in your cockpit. No browser windows, no second monitor, no tabbing out of the sim.

Enter your own Social Stream Ninja session ID once, and chat from every connected source lands in one window. Read-only by design: the bridge receives chat, it never sends it.

**If it's on Social Stream Ninja, it works here.** Whatever platform you stream on — Twitch, YouTube, Kick, TikTok, Facebook, Trovo, or any of the 100+ sources Social Stream Ninja supports — your chat shows up in the Bridge. If Social Stream Ninja can capture it, the Bridge can show it.

---

## Follow MrGameSimulation

Fly along live — **MrGameSimulation** streams on every platform:

- **Twitch:** https://www.twitch.tv/mrgamesimulation
- **YouTube:** https://www.youtube.com/@MrGameSimulation
- **Kick:** https://kick.com/mandraki-bey
- **TikTok:** https://www.tiktok.com/@therealmalign20
- **Trovo:** https://trovo.live/s/mrgamesimulation
- **Facebook (MrGameSimulation):** https://www.facebook.com/mrgamesimulation
- **Facebook (African American Airlines):** https://www.facebook.com/AfricanAmericanAirlinesGroup/

---

## Features

- **One chat for every platform** — every source Social Stream Ninja merges, color-tagged by platform, in a single in-sim feed
- **Made for VR** — the window moves into VR automatically when you enter VR, with big readable text; drag it where you want it and resize from the edges
- **See-through mode** — the BG slider (0–100%) fades *everything except the chat text*: panel, borders, buttons and controls. At 0%, only the words float over your cockpit
- **Set up in the sim** — paste your session ID right inside the X-Plane window, or use the local setup page at **http://127.0.0.1:17890/** — no config files to edit
- **Enter it once** — your session ID is saved and remembered across restarts
- **Read-only** — it never posts to your chat
- **Private by design** — your session ID is stored locally only, on your computer, and is never displayed back or sent anywhere except to join your own Social Stream Ninja relay room
- **Zero performance cost** — lightweight engine, newest 40 messages kept

## Requirements

- **X-Plane 12**
- **XPPython3** (free, one-time install) — https://xppython3.readthedocs.io/ — the chat engine runs on it
- **A Social Stream Ninja session** (free) — desktop app or browser extension, running with your sources connected

## Install

1. Copy the folder **`MRGS-VRChatBridge`** (contains `mac.xpl` / `win.xpl`) into:
   `X-Plane 12/Resources/plugins/`
2. Copy the **contents of `PythonPlugins`** (`PI_MRGSChatBridge.py` and `cacert.pem`) into:
   `X-Plane 12/Resources/plugins/PythonPlugins/`
3. **macOS only:** the downloaded files arrive quarantined. Run these once in Terminal (adjust the path to your X-Plane install):
   ```
   xattr -dr com.apple.quarantine "/path/to/X-Plane 12/Resources/plugins/MRGS-VRChatBridge"
   xattr -dr com.apple.quarantine "/path/to/X-Plane 12/Resources/plugins/PythonPlugins/PI_MRGSChatBridge.py"
   ```
4. Start (or restart) X-Plane 12. The window opens centered; show / hide it any time from **Plugins → MrGameSimulation VR Chat Bridge → Show / Hide**.

## Connect Your Chat

1. In the bridge window, click the **session ID field** (or the footer's **SETUP** button), type or paste your Social Stream Ninja session ID, and press **CONNECT** (or Enter). The same form also lives in your browser at **http://127.0.0.1:17890/**.
2. Chat starts flowing within a few seconds. No restart needed if you change the ID later — the bridge retargets on its own.

### Where to find your Social Stream Ninja session ID

In the Social Stream Ninja **desktop app → Stream Deck Setup** — use the **"copy this ID"** button.

> **Note:** every Social Stream Ninja install has its *own* session ID. If you use the desktop app on one machine and the browser extension on another, they will have different IDs — use the ID from the install that is actually running your chat.

Use the window's **BG slider** in the footer to fade the background, **CLEAR** to empty the feed, and **HIDE** to tuck the window away.

## Troubleshooting

- **No window / no menu entry** — XPPython3 is missing or not loading. Install XPPython3 first, restart X-Plane, and check `Log.txt` for Python plugin errors.
- **Stuck on CONNECTING** — make sure the Social Stream Ninja app (or extension) is actually **running** with your sources connected, and double-check you pasted the **right session ID** for that install (see above). Chat cannot flow if Social Stream Ninja itself isn't running.
- **Window missing in VR** — toggle it from **Plugins → MrGameSimulation VR Chat Bridge**; entering VR moves it into the headset automatically.
- **macOS: plugin won't load / "damaged"** — you skipped the quarantine step. Run the two `xattr` commands in Install step 3, then restart X-Plane.

## Privacy & Notes

- Your session ID never leaves your computer except to join your own Social Stream Ninja relay room — the same room the official dock uses.
- Chat is rendered as plain text (HTML is stripped on arrival).
- The feed keeps the newest 40 messages.

## Credits

- **Built by Mandraki Bey · MrGameSimulation**
  - Twitch: https://www.twitch.tv/mrgamesimulation
  - YouTube: https://www.youtube.com/@MrGameSimulation
  - Kick: https://kick.com/mandraki-bey
  - TikTok: https://www.tiktok.com/@therealmalign20
  - Trovo: https://trovo.live/s/mrgamesimulation
  - Facebook: https://www.facebook.com/mrgamesimulation
  - African American Airlines on Facebook: https://www.facebook.com/AfricanAmericanAirlinesGroup/
- Chat aggregation by **Social Stream Ninja** — https://socialstream.ninja/
- Not affiliated with Laminar Research or X-Plane.

*MrGameSimulation — we'll take the 40 acres; keep the mule, we want a plane.*

## Changelog

- **1.4.1** — The panel is now a landscape rectangle (1008x773, the AAA DispatchUI tablet's proportions) instead of the 560x660 portrait shape. Everything else is the v1.4.0 GS4XP-method build, unchanged.
- **1.4.0** — The clean GS4XP-method build: native window, VR handled purely through the positioning-mode API at the app's natural 560x660 size, rendering switched by XPLMWindowIsInVR. All manual container-fitting from the v1.1.x-v1.2.4 line (safe-area insets, taller panel, bezel, content drop, scroll pin) is removed, so this build answers one question: does the panel present fully in VR with sanctioned handling alone?
- **1.3.2** — Renderer aligned with the GS4XP engine recipe (native XPLM window, Dear ImGui compiled in, VR and popped-out states handled through the positioning-mode API): popped-out windows now get the same correct rendering as VR instead of being projected against the monitor.
- **1.3.1** — Direction settled (Mandraki): the in-sim panel is the native window, Homebase-style - fully custom-drawn and compiled into the .xpl, no webview, no served pages, no external UI assets. The web chat page from 1.3.0 stays as a tablet / second-screen extra only.
- **1.3.0** — The chat is also served as a full web page for a tablet or second screen on the same network (the setup page shows the address). Same app, same design. The native X-Plane window is unchanged.
- **1.2.4** — Correction to 1.2.3: only the top comes down (130 px); the bottom and footer stay exactly where they were.
- **1.2.3** — Same app surface, but in VR the content is brought 130 px further down inside the frame so the whole app clears the top and is seen.
- **1.2.2** — The app now sits inside a black rounded bezel frame, 5/16 inch (44 px) on every side - the same frame the AAA DispatchUI tablet uses. The whole app, header included, lives inside the frame.
- **1.2.1** — VR display restored (the v1.1.8 measured fit: taller panel, app below X-Plane's VR window bar) plus a fix for the missing header: the panel's inner frame is pinned to the top every frame so it can no longer drift and push the header out of view.
- **1.2.0** — The window is now built exactly like the AAA DispatchUI (Dear ImGui) window: same renderer, same VR handling, no overlays or workarounds. If DispatchUI displays correctly in your headset, this will too.
- **1.1.8** — The measured VR fit: the calibration screenshot showed X-Plane's VR window bar covers the top ~100 px of the panel (the rest maps 1:1). The VR panel is now 560x770 and the app sits below the bar with its full usable height restored, so the entire app is visible inside the container.
- **1.1.7** — Adds a VR fit calibration pattern (setup page - "Show calibration pattern"): a measurement grid drawn over the full panel so one VR screenshot shows exactly how X-Plane maps the panel into its VR container; the final fit and container size are then set from that measurement.
- **1.1.6** — Hotfix: the chrome-free overlay is held OFF (in v1.1.5 it hid the window in VR but the overlay did not draw on some systems, leaving no chat at all). The VR window is back exactly as in v1.1.4, with the responsive layout from v1.1.5. The overlay now logs its matrix sources so it can be proven and re-enabled safely.
- **1.1.5** — Responsive layout (the footer and background now anchor to the real window size, so nothing slides off in VR), chrome-free VR (entering VR hides the X-Plane square window and the chat becomes a floating overlay that follows your view in the cockpit AND external views; the square window returns when you leave VR or choose Show from the Plugins menu; in-VR it is view-only — use the desktop window for buttons and setup), and the follow overlay now keys off X-Plane's external-view flag.
- **1.1.4** — VR safe area: in VR the whole UI is inset from the panel edges (54 px top, 14 px sides/bottom), so the header and footer never sit in the strip X-Plane overlays with its own window bar or crops at the panel edge. Desktop rendering is unchanged.
- **1.1.3** — VR polish: the panel now scales its clipping to the VR texture resolution (fixes the cut-off top and off margins); in external camera views the chat follows your field of view as a floating overlay (read-only out there — the cockpit panel stays interactive); emoji in messages are dropped at intake instead of showing as ???? (the in-sim font has no emoji glyphs).
- **1.1.2** — Leaving VR no longer strands the window off-screen in 2D: its desktop position is saved on VR entry and restored on exit, and Show from the Plugins menu pulls a lost window back on-screen.
- **1.1.1** — Fixed the window rendering blank in VR (the renderer projected in monitor space; VR panel windows now render in window-local space).

- **1.1.0** — Branding / xplane.to release. Full MrGameSimulation branding: gold chat-bubble logo and wordmark in the native window, branded local setup page with channel links, plugin renamed to "MrGameSimulation VR Chat Bridge".
- **1.0.4** — Fixed WebSocket handshake (missing `WS_GUID` constant in the extracted client — the cause of "not connecting"); engine now records relay errors and connection attempts in its state file for diagnosis.
- **1.0.2** — In-window session ID entry (no config editing); background opacity now fades everything *except* the chat text.
- **1.0.0** — First release. Social Stream Ninja chat in X-Plane 12, desktop + VR.
