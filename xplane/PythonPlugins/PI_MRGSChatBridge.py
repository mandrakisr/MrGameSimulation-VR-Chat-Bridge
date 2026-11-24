"""MrGameSimulation VR Chat Bridge - X-Plane 12 engine (v1.1.0).

Brings your Social Stream Ninja stream chat into X-Plane, on the
desktop and in VR. Works with any streaming setup Social Stream
Ninja supports (Twitch, YouTube, Kick, TikTok, and more): enter
YOUR OWN Social Stream Ninja session ID once, and chat from every
connected source lands in one clean in-sim window.

How it fits together:
  * This file is the engine. It runs under XPPython3, joins the
    Social Stream Ninja channel-4 relay
    (wss://io.socialstream.ninja/join/<your-session-id>/4) with a
    stdlib WebSocket client, keeps the newest 40 messages (HTML
    stripped, text only), and publishes them to a small state file
    the native window renders.
  * The native MRGS-VRChatBridge window (mac.xpl / win.xpl) is the
    display. It reads the state file and nothing else.
  * Setup is a tiny local web page at http://127.0.0.1:17890/ -
    paste your session ID there once. It is saved to
    mrgs_chat_settings.json next to this file and never leaves
    your computer. Social Stream Ninja session IDs are persistent:
    the same ID keeps working across restarts.

Read-only by design: the bridge never sends chat.

Requires XPPython3 (https://xppython3.readthedocs.io/).
"""

import base64
import hashlib
import json
import os
import re
import socket
import ssl
import struct
import threading
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PRODUCT = "MrGameSimulation VR Chat Bridge"
VERSION = "1.4.1"
SETTINGS_FILE = "mrgs_chat_settings.json"
STATE_FILE = "mrgs_chat_state.json"
ACTIONS_FILE = "mrgs_chat_actions.jsonl"
SETUP_PORT = 17890
SETUP_URL = "http://127.0.0.1:%d/" % SETUP_PORT


def plugin_dir():
    """Folder this plugin file lives in (settings/state live there)."""
    try:
        return os.path.dirname(os.path.abspath(__file__))
    except Exception:
        return "."


def _ca_bundle_candidates():
    """Paths that may hold a CA bundle for ssl_context(), best first."""
    paths = []
    try:
        paths.append(os.path.join(plugin_dir(), "cacert.pem"))
    except Exception:
        pass
    try:
        paths.append(os.path.join(
            os.path.dirname(os.path.abspath(__file__)), "cacert.pem"))
    except Exception:
        pass
    return paths


_SSL_CONTEXT = None


def ssl_context():
    """The one TLS context every connection in this plugin uses.

    The Python bundled with XPPython3 on Windows has no usable CA
    store, so default verification fails against every host even
    though the network is fine. Prefer certifi when importable;
    else the Mozilla bundle shipped next to this plugin
    (cacert.pem); else the platform default. Built once, cached.
    Never raises.
    """
    global _SSL_CONTEXT
    if _SSL_CONTEXT is not None:
        return _SSL_CONTEXT
    ctx = None
    try:
        import ssl as _ssl
        try:
            import certifi
            ctx = _ssl.create_default_context(cafile=certifi.where())
        except Exception:
            ctx = None
        if ctx is None:
            for cand in _ca_bundle_candidates():
                if os.path.exists(cand):
                    ctx = _ssl.create_default_context(cafile=cand)
                    break
        if ctx is None:
            ctx = _ssl.create_default_context()
    except Exception:
        ctx = None
    if ctx is not None:
        _SSL_CONTEXT = ctx
    return ctx


class WsFrameDecoder:
    """Incremental RFC 6455 frame parser.

    feed(chunk) returns the (fin, opcode, payload) frames completed
    by that chunk; partial frames wait for more bytes. Handles
    masked and unmasked frames and 16/64-bit payload lengths."""

    def __init__(self):
        self._buf = bytearray()

    def feed(self, chunk):
        if chunk:
            self._buf += chunk
        frames = []
        while True:
            frame = self._read_one()
            if frame is None:
                return frames
            frames.append(frame)

    def _read_one(self):
        buf = self._buf
        if len(buf) < 2:
            return None
        fin = bool(buf[0] & 0x80)
        opcode = buf[0] & 0x0F
        masked = bool(buf[1] & 0x80)
        length = buf[1] & 0x7F
        off = 2
        if length == 126:
            if len(buf) < 4:
                return None
            length = struct.unpack("!H", bytes(buf[2:4]))[0]
            off = 4
        elif length == 127:
            if len(buf) < 10:
                return None
            length = struct.unpack("!Q", bytes(buf[2:10]))[0]
            off = 10
        mask = None
        if masked:
            if len(buf) < off + 4:
                return None
            mask = bytes(buf[off:off + 4])
            off += 4
        if len(buf) < off + length:
            return None
        payload = bytes(buf[off:off + length])
        if mask is not None:
            payload = bytes(b ^ mask[i % 4]
                            for i, b in enumerate(payload))
        del self._buf[:off + length]
        return (fin, opcode, payload)


WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


class BridgeWebSocket:
    """Minimal WebSocket client for the LPXP bridge endpoint.

    Mirrors bridge/ws-client.mjs: handshake with the
    "bridge.<token>" subprotocol, 101 + accept-key verification,
    masked sends, fragmentation reassembly, ping -> pong, close.
    Every blocking call runs on the relay thread only."""

    def __init__(self, url, token, on_message=None, on_open=None,
                 on_close=None):
        parsed = urllib.parse.urlparse(url)
        self._secure = parsed.scheme == "wss"
        self._host = parsed.hostname or ""
        self._port = parsed.port or (443 if self._secure else 80)
        self._path = parsed.path or "/"
        if parsed.query:
            self._path += "?" + parsed.query
        self._token = token
        self.on_message = on_message or (
            lambda payload, is_binary: None)
        self.on_open = on_open or (lambda: None)
        self.on_close = on_close or (lambda error=None: None)
        self.connected = False
        self._sock = None
        self._decoder = WsFrameDecoder()
        self._fragments = []
        self._fragment_binary = True
        self._closed = False

    def connect(self):
        """Blocking handshake. Raises ValueError/OSError on failure."""
        raw = socket.create_connection((self._host, self._port),
                                       timeout=10)
        try:
            if self._secure:
                context = ssl_context()
                raw = context.wrap_socket(
                    raw, server_hostname=self._host)
            key = base64.b64encode(os.urandom(16)).decode("ascii")
            host = self._host
            if self._port not in (80, 443):
                host = "%s:%d" % (host, self._port)
            proto = ""
            if self._token:
                proto = ("Sec-WebSocket-Protocol: bridge.%s\r\n"
                         % self._token)
            request = (
                "GET %s HTTP/1.1\r\n"
                "Host: %s\r\n"
                "Connection: Upgrade\r\n"
                "Upgrade: websocket\r\n"
                "Sec-WebSocket-Version: 13\r\n"
                "Sec-WebSocket-Key: %s\r\n"
                "%s\r\n" % (
                    self._path, host, key, proto))
            raw.sendall(request.encode("ascii"))
            head, rest = self._read_head(raw)
            status = head.split("\r\n", 1)[0] if head else ""
            if " 101" not in status:
                code = status.rsplit(" ", 1)[-1] if status else "?"
                raise ValueError("HTTP %s" % code)
            expected = base64.b64encode(hashlib.sha1(
                (key + WS_GUID).encode("ascii")).digest()
                ).decode("ascii")
            accept = ""
            for line in head.split("\r\n")[1:]:
                if line.lower().startswith("sec-websocket-accept:"):
                    accept = line.split(":", 1)[1].strip()
            if accept != expected:
                raise ValueError("bad accept key")
        except Exception:
            try:
                raw.close()
            except Exception:
                pass
            raise
        self._sock = raw
        self.connected = True
        self._closed = False
        if rest:
            self._on_data(rest)
        self.on_open()

    @staticmethod
    def _read_head(sock):
        data = b""
        while b"\r\n\r\n" not in data:
            chunk = sock.recv(4096)
            if not chunk:
                raise ValueError("closed during handshake")
            data += chunk
            if len(data) > 65536:
                raise ValueError("handshake too large")
        head, rest = data.split(b"\r\n\r\n", 1)
        return head.decode("iso-8859-1"), rest

    # -- receiving ----------------------------------------------------

    def poll(self, timeout=0.05):
        """Read whatever is available and dispatch its frames.

        Returns False once the connection is closed (by either
        side); True while it is alive."""
        if not self.connected or self._sock is None:
            return False
        try:
            self._sock.settimeout(timeout)
            chunk = self._sock.recv(65536)
        except socket.timeout:
            return True
        except Exception:
            self._finish()
            return False
        if not chunk:
            self._finish()
            return False
        self._on_data(chunk)
        return self.connected

    def _on_data(self, chunk):
        for fin, opcode, payload in self._decoder.feed(chunk):
            self._handle_frame(fin, opcode, payload)
            if not self.connected:
                return

    def _handle_frame(self, fin, opcode, payload):
        if opcode == 0x9:                      # ping -> pong
            self._send_frame(0xA, payload)
            return
        if opcode == 0x8:                      # close
            self._finish()
            return
        if opcode == 0xA:                      # pong: nothing to do
            return
        if opcode != 0x0:
            self._fragment_binary = (opcode == 0x2)
            self._fragments = [payload]
        else:
            self._fragments.append(payload)
        if fin:
            whole = b"".join(self._fragments)
            self._fragments = []
            self.on_message(whole, self._fragment_binary)

    # -- sending --------------------------------------------------------

    def _send_frame(self, opcode, payload):
        if not self.connected or self._sock is None:
            return False
        try:
            self._sock.sendall(ws_encode_frame(opcode, payload))
            return True
        except Exception:
            self._finish()
            return False

    def send_binary(self, payload):
        return self._send_frame(0x2, payload)

    def send_text(self, text):
        return self._send_frame(0x1, text.encode("utf-8"))

    def close(self):
        if self._sock is not None:
            try:
                self._sock.sendall(ws_encode_frame(0x8, b""))
            except Exception:
                pass
        self._finish()

    def _finish(self):
        if self._closed:
            return
        self._closed = True
        self.connected = False
        sock, self._sock = self._sock, None
        if sock is not None:
            try:
                sock.close()
            except Exception:
                pass
        self.on_close()



class SsnChatRelay:
    """Social Stream Ninja stream chat feed for the native window.

    One daemon thread joins the SSN channel-4 relay
    (wss://io.socialstream.ninja/join/<session>/4) with the same
    stdlib WebSocket client the LPXP bridge uses, keeps the newest
    40 chat messages, and reconnects with backoff. Chat text is
    rendered TEXT ONLY everywhere in AAA apps: any HTML in a
    message is stripped here, at intake, and entities are decoded.
    The session ID is read through a callable on every (re)connect
    and re-checked while connected, so saving a new ID in Settings
    retargets the feed without a restart. Read-only: the relay
    never sends chat. Status + messages ride the UI state file as
    the "chat" object (see _ui_state)."""

    MAX_MESSAGES = 40
    URL = "wss://io.socialstream.ninja/join/%s/4"
    _ENTITIES = (("&lt;", "<"), ("&gt;", ">"), ("&quot;", '"'),
                 ("&#39;", "'"), ("&apos;", "'"), ("&nbsp;", " "),
                 ("&amp;", "&"))

    def __init__(self, session_id_getter, ws_factory=None):
        self._get_id = session_id_getter
        self._ws_factory = ws_factory        # test seam
        self._stop = threading.Event()
        self._thread = None
        self._lock = threading.Lock()
        self.messages = []                   # [{name, text, source}]
        self.connected = False
        self.session_id = ""
        self.last_error = ""
        self.attempts = 0

    # -- lifecycle ----------------------------------------------------

    def start(self):
        if self._thread is not None and self._thread.is_alive():
            return
        self._stop.clear()
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def stop(self):
        self._stop.set()

    # -- message intake (unit-tested) ---------------------------------

    @classmethod
    def clean_message(cls, frame):
        """One raw SSN frame -> {name, text, source} or None."""
        try:
            data = json.loads(frame)
        except Exception:
            return None
        if not isinstance(data, dict):
            return None
        name = data.get("chatname")
        text = data.get("chatmessage")
        if not isinstance(name, str) or not isinstance(text, str):
            return None
        text = re.sub(r"<[^>]+>", "", text)
        for ent, ch in cls._ENTITIES:
            text = text.replace(ent, ch)
        # The native window's font has no emoji glyphs (they would
        # render as '?'), so drop emoji/symbols at intake.
        _EMOJI = r"[\U0001F000-\U0001FAFF\u2600-\u27BF\uFE0F\u200D\u20E3]"
        text = re.sub(_EMOJI, "", text)
        name = re.sub(_EMOJI, "", name)
        text = " ".join(text.split())[:300]
        name = " ".join(name.split())[:40]
        if not name or not text:
            return None
        source = data.get("type")
        source = source.lower()[:16] if isinstance(source, str) else ""
        return {"name": name, "text": text, "source": source}

    def _accept(self, payload, is_binary):
        if is_binary:
            return
        msg = self.clean_message(payload.decode("utf-8", "replace"))
        if msg is None:
            return
        with self._lock:
            self.messages.append(msg)
            del self.messages[:-self.MAX_MESSAGES]

    def snapshot(self):
        with self._lock:
            msgs = list(self.messages)
        return {"connected": bool(self.connected),
                "has_session": bool(self.session_id),
                "messages": msgs,
                "error": getattr(self, "last_error", ""),
                "attempts": getattr(self, "attempts", 0)}

    # -- thread ---------------------------------------------------------

    def _run(self):
        backoff = 2.0
        while not self._stop.is_set():
            sid = (self._get_id() or "").strip()
            if not sid:
                self.connected = False
                self.session_id = ""
                if self._stop.wait(2.0):
                    break
                continue
            self.attempts += 1
            ws = None
            try:
                def on_open():
                    self.connected = True

                def on_close(error=None):
                    self.connected = False

                if self._ws_factory is not None:
                    ws = self._ws_factory(self._accept, on_open,
                                          on_close)
                else:
                    ws = BridgeWebSocket(
                        self.URL % sid, None,
                        on_message=self._accept, on_open=on_open,
                        on_close=on_close)
                self.session_id = sid
                ws.connect()
                backoff = 2.0
                while not self._stop.is_set():
                    if not ws.poll(0.25):
                        break
                    if (self._get_id() or "").strip() != sid:
                        break        # saved ID changed: retarget
            except Exception as e:
                try:
                    err = "%s: %s" % (type(e).__name__, e)
                    if sid:
                        err = err.replace(sid, "***")
                    self.last_error = err[:220]
                except Exception:
                    pass
            self.connected = False
            if ws is not None:
                try:
                    ws.close()
                except Exception:
                    pass
            if self._stop.wait(backoff):
                break
            backoff = min(backoff * 2.0, 30.0)



class MrgsRelay(SsnChatRelay):
    """SsnChatRelay plus a Clear the native window can ask for."""

    def clear(self):
        with self._lock:
            del self.messages[:]


# ----------------------------------------------------------------------
# Settings: one field, the user's own Social Stream Ninja session ID.
# ----------------------------------------------------------------------

def load_settings():
    try:
        with open(os.path.join(plugin_dir(), SETTINGS_FILE),
                  "r", encoding="utf-8") as fh:
            data = json.load(fh)
        if isinstance(data, dict):
            return {"ssn_session_id":
                    str(data.get("ssn_session_id", "") or "")}
    except Exception:
        pass
    return {"ssn_session_id": ""}


def _atomic_write(path, text):
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as fh:
        fh.write(text)
    os.replace(tmp, path)


def save_settings(settings):
    _atomic_write(os.path.join(plugin_dir(), SETTINGS_FILE),
                  json.dumps(settings, indent=1))


# ----------------------------------------------------------------------
# Web pages: setup + a chat page for a tablet / second screen,
# served on all interfaces. The IN-SIM panel is the native
# Homebase-style window (custom-drawn, compiled into the .xpl);
# these pages are an extra, never the in-sim UI. The session ID
# is never echoed.
# ----------------------------------------------------------------------

_LAN_IP_CACHE = None


def _lan_ip():
    global _LAN_IP_CACHE
    if _LAN_IP_CACHE:
        return _LAN_IP_CACHE
    ip = None
    try:
        probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            probe.connect(("8.8.8.8", 80))
            ip = probe.getsockname()[0]
        finally:
            probe.close()
    except Exception:
        ip = None
    if not ip:
        try:
            ip = socket.gethostbyname(socket.gethostname())
        except Exception:
            ip = "127.0.0.1"
    _LAN_IP_CACHE = ip
    return ip

_SETUP_PAGE = """<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>MrGameSimulation VR Chat Bridge - Setup</title>
<style>
  body { margin:0; background:#0a1120; color:#f2ecd9;
         font-family:'Segoe UI', Arial, sans-serif; }
  .wrap { max-width:520px; margin:48px auto; padding:0 20px; }
  .card { background:#101b31; border:1px solid #2a3a58;
          border-radius:14px; padding:28px; }
  .brand { text-align:center; margin-bottom:22px; }
  .brand h1 { font-size:27px; margin:10px 0 2px; color:#f2ecd9;
       letter-spacing:.01em; }
  .brand .vr { color:#e8c268; font-size:12.5px; font-weight:700;
       letter-spacing:.42em; margin-left:.42em; }
  .brand .by { color:#9fb0cc; font-size:12.5px; margin-top:8px; }
  .status { font-size:14px; margin-bottom:18px; text-align:center; }
  .dot { display:inline-block; width:10px; height:10px;
         border-radius:50%%; margin-right:8px; background:%(dot)s; }
  label { display:block; font-size:12px; letter-spacing:.12em;
          color:#9fb0cc; margin-bottom:6px; }
  input[type=text] { width:100%%; box-sizing:border-box;
    background:#0a1120; border:1px solid #2a3a58; color:#f2ecd9;
    border-radius:8px; padding:11px 12px; font-size:15px; }
  button { margin-top:14px; width:100%%; background:#e8c268;
    color:#0a1120; border:0; border-radius:8px; padding:12px;
    font-size:15px; font-weight:700; cursor:pointer; }
  .hint { color:#9fb0cc; font-size:12.5px; line-height:1.55;
          margin-top:18px; }
  .ok { color:#7dd87d; font-size:13px; margin-top:12px; }
  .links { text-align:center; margin-top:22px; font-size:12.5px;
           color:#9fb0cc; }
  .links a { color:#e8c268; text-decoration:none; }
</style></head><body><div class="wrap"><div class="card">
<div class="brand">
<svg width="54" height="46" viewBox="0 0 54 46">
<rect x="2" y="2" width="50" height="34" rx="10" fill="#e8c268"/>
<polygon points="12,34 24,34 14,44" fill="#e8c268"/>
<circle cx="15" cy="19" r="3.4" fill="#0a1120"/>
<circle cx="27" cy="19" r="3.4" fill="#0a1120"/>
<circle cx="39" cy="19" r="3.4" fill="#0a1120"/>
</svg>
<h1>MrGameSimulation</h1>
<div class="vr">VR CHAT BRIDGE</div>
<div class="by">Your stream chat, right in the cockpit - by Mandraki Bey</div>
</div>
<div class="status"><span class="dot"></span>%(status)s</div>
<form method="post" action="/save">
<label for="sid">YOUR SOCIAL STREAM NINJA SESSION ID</label>
<input type="text" id="sid" name="session_id" autocomplete="off"
       spellcheck="false" placeholder="Paste your session ID">
<button type="submit">Save and connect</button>
</form>
%(saved)s
<div style="margin-top:20px;border-top:1px solid #2a3a58;
padding-top:16px">
<div style="font-size:12px;letter-spacing:.12em;
color:#9fb4d8">CHAT ON A TABLET / SECOND SCREEN</div>
<div class="hint">The chat is also a web page for a tablet or
second screen on the same network - handy next to the sim.
(The in-sim panel is the X-Plane window itself.) Open:</div>
<div style="margin-top:10px;font-size:16px">
<a href="%(lan)s" style="color:#e8c268">%(lan)s</a></div>
</div>
<div class="hint">Find your session ID in the Social Stream Ninja
desktop app under <b>Stream Deck Setup</b> ("copy this ID"), or in the
extension under Global settings &rarr; Session Options. The ID is
persistent - you only paste it once. You can also paste it right
inside the X-Plane window (the SETUP button). It is stored on this
computer only and is never shown back here.</div>
<div class="links">Fly with MrGameSimulation:
<a href="https://www.twitch.tv/mrgamesimulation">Twitch</a> &middot;
<a href="https://www.youtube.com/@MrGameSimulation">YouTube</a> &middot;
<a href="https://kick.com/mandraki-bey">Kick</a> &middot;
<a href="https://www.tiktok.com/@therealmalign20">TikTok</a></div>
</div></div></body></html>"""

_CHAT_PAGE = """<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>MrGameSimulation VR Chat Bridge</title>
<style>
  :root { --bgop: 1; }
  * { box-sizing: border-box; }
  html, body { height: 100%; }
  body { margin:0; background:#000; color:#f2ecd9;
         font-family:'Segoe UI', Arial, sans-serif; }
  .frame { min-height:100%; background:#05070c; padding:14px; }
  .panel { max-width:760px; margin:0 auto;
           min-height:calc(100vh - 28px); display:flex;
           flex-direction:column; border:1px solid #2a3a58;
           border-radius:22px; padding:26px 28px;
           background:linear-gradient(180deg,
             rgba(19,28,50,var(--bgop)),
             rgba(8,13,26,var(--bgop))); }
  header { display:flex; align-items:center; gap:14px; }
  header h1 { font-size:25px; margin:0; letter-spacing:.01em; }
  .vr { color:#e8c268; font-size:11.5px; font-weight:700;
        letter-spacing:.42em; margin-top:2px; }
  .status { margin-left:auto; font-size:13.5px; color:#9fb0cc;
            white-space:nowrap; }
  .dot { display:inline-block; width:10px; height:10px;
         border-radius:50%; margin-right:7px; background:#d86a6a; }
  .by { color:#9fb0cc; font-size:12.5px; margin-top:10px; }
  hr { border:0; border-top:1px solid #2a3a58; margin:16px 0; }
  #feed { flex:1; overflow-y:auto; padding-right:6px;
          font-size:16.5px; line-height:1.45; }
  .msg { margin-bottom:13px; }
  .msg .name { color:#e8c268; font-weight:700; }
  .msg .src { color:#9fb0cc; font-size:12.5px; margin-left:7px; }
  .msg .text { display:block; margin-top:1px; overflow-wrap:anywhere; }
  .empty { color:#9fb0cc; }
  footer { display:flex; align-items:center; gap:12px;
           border-top:1px solid #2a3a58; padding-top:16px;
           margin-top:14px; flex-wrap:wrap; }
  button { background:#101b31; color:#f2ecd9; border:1px solid #2a3a58;
           border-radius:9px; padding:10px 18px; font-size:14px;
           font-weight:700; letter-spacing:.06em; cursor:pointer; }
  button:hover { border-color:#e8c268; }
  .bglabel { color:#9fb0cc; font-size:13px; letter-spacing:.08em; }
  input[type=range] { width:140px; accent-color:#e8c268; }
  .ver { margin-left:auto; color:#5f7194; font-size:12px; }
  .vrtip { color:#9fb0cc; font-size:12.5px; margin-top:12px; }
  .vrtip a { color:#e8c268; }
  #setupBox label { display:block; font-size:12px;
    letter-spacing:.12em; color:#9fb0cc; margin-bottom:6px; }
  #setupBox input[type=text] { width:100%; background:#0a1120;
    border:1px solid #2a3a58; color:#f2ecd9; border-radius:8px;
    padding:11px 12px; font-size:15px; }
  #setupBox button { margin-top:12px; width:100%; background:#e8c268;
    color:#0a1120; border:0; }
</style></head><body><div class="frame"><div class="panel">
<header>
<svg width="46" height="39" viewBox="0 0 54 46">
<rect x="2" y="2" width="50" height="34" rx="10" fill="#e8c268"/>
<polygon points="12,34 24,34 14,44" fill="#e8c268"/>
<circle cx="15" cy="19" r="3.4" fill="#0a1120"/>
<circle cx="27" cy="19" r="3.4" fill="#0a1120"/>
<circle cx="39" cy="19" r="3.4" fill="#0a1120"/>
</svg>
<div><h1>MrGameSimulation</h1><div class="vr">VR CHAT BRIDGE</div></div>
<div class="status"><span class="dot" id="dot"></span><span
id="statusText">Starting...</span></div>
</header>
<div class="by">by Mandraki Bey - twitch.tv/mrgamesimulation</div>
<hr>
<div id="setupBox" hidden>
<form method="post" action="/save">
<label for="sid">YOUR SOCIAL STREAM NINJA SESSION ID</label>
<input type="text" id="sid" name="session_id" autocomplete="off"
       spellcheck="false" placeholder="Paste your session ID">
<button type="submit">Save and connect</button>
</form>
<hr>
</div>
<div id="feed"><div class="empty">Connecting to the bridge...</div></div>
<footer>
<button id="clearBtn" type="button">CLEAR</button>
<span class="bglabel">BG</span>
<input type="range" id="bgSlider" min="20" max="100" value="100">
<button id="setupBtn" type="button">SETUP</button>
<span class="ver" id="ver"></span>
</footer>
<div class="vrtip" id="vrTip"></div>
</div></div>
<script>
var feed = document.getElementById('feed');
var dot = document.getElementById('dot');
var statusText = document.getElementById('statusText');
var setupBox = document.getElementById('setupBox');
var setupForced = false;
var sig = '';
function setStatus(connected, hasSession) {
  dot.style.background = connected ? '#7dd87d'
    : (hasSession ? '#e8c268' : '#d86a6a');
  statusText.textContent = connected ? 'LIVE'
    : (hasSession ? 'Connecting...' : 'No session ID');
}
function render(s) {
  setStatus(!!s.connected, !!s.has_session);
  setupBox.hidden = !!s.has_session && !setupForced;
  document.getElementById('ver').textContent =
    s.version ? ('v' + s.version) : '';
  var tip = document.getElementById('vrTip');
  if ((location.hostname === '127.0.0.1' ||
       location.hostname === 'localhost') && s.chat_url) {
    tip.innerHTML = 'On a tablet or second screen, open this page at ';
    var a = document.createElement('a');
    a.href = s.chat_url; a.textContent = s.chat_url;
    tip.appendChild(a);
  } else { tip.textContent = ''; }
  var msgs = s.messages || [];
  var last = msgs.length ? msgs[msgs.length - 1] : null;
  var newSig = msgs.length + '|' + (last ? last.name + last.text : '')
    + '|' + s.connected + '|' + s.has_session;
  if (newSig === sig) return;
  sig = newSig;
  var nearBottom = feed.scrollHeight - feed.scrollTop
    - feed.clientHeight < 90;
  feed.replaceChildren();
  if (!msgs.length) {
    var d = document.createElement('div');
    d.className = 'empty';
    d.textContent = s.connected
      ? 'No messages yet - chat lands here as it arrives.'
      : 'Connecting to Social Stream Ninja...';
    feed.appendChild(d);
  }
  msgs.forEach(function(m) {
    var row = document.createElement('div');
    row.className = 'msg';
    var n = document.createElement('span');
    n.className = 'name'; n.textContent = m.name || '';
    row.appendChild(n);
    if (m.source) {
      var so = document.createElement('span');
      so.className = 'src'; so.textContent = m.source;
      row.appendChild(so);
    }
    var t = document.createElement('span');
    t.className = 'text'; t.textContent = m.text || '';
    row.appendChild(t);
    feed.appendChild(row);
  });
  if (nearBottom) feed.scrollTop = feed.scrollHeight;
}
function tick() {
  fetch('/state', {cache: 'no-store'})
    .then(function(r) { return r.json(); })
    .then(render)
    .catch(function() {
      dot.style.background = '#d86a6a';
      statusText.textContent = 'Engine unreachable';
    });
}
document.getElementById('clearBtn').addEventListener('click',
  function() {
    fetch('/action', {method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({action: 'clear'})}).then(tick);
  });
document.getElementById('setupBtn').addEventListener('click',
  function() { setupForced = !setupForced; sig = ''; tick(); });
document.getElementById('bgSlider').addEventListener('input',
  function(e) {
    document.documentElement.style.setProperty('--bgop',
      e.target.value / 100);
  });
setInterval(tick, 1000);
tick();
</script>
</body></html>
"""


class _SetupHandler(BaseHTTPRequestHandler):
    bridge = None                      # set by SetupServer

    def log_message(self, fmt, *args):
        pass

    def _page(self, saved=False):
        snap = self.bridge.snapshot()
        if snap["connected"]:
            status, dot = "Connected - chat is flowing to X-Plane.", "#7dd87d"
        elif snap["has_session"]:
            status, dot = ("Session ID saved - connecting...",
                           "#e8c268")
        else:
            status, dot = "No session ID yet - paste yours below.", "#d86a6a"
        body = _SETUP_PAGE % {
            "status": status, "dot": dot,
            "lan": snap.get("chat_url", "/chat"),
            "saved": ('<div class="ok">Saved. The bridge retargets '
                      'within a few seconds - no restart needed.</div>'
                      if saved else "")}
        raw = body.encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)

    def _chat(self):
        raw = _CHAT_PAGE.encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)

    def do_GET(self):
        if self.path.startswith("/chat"):
            self._chat()
        elif self.path.startswith("/state"):
            raw = json.dumps(self.bridge.snapshot()).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(raw)))
            self.end_headers()
            self.wfile.write(raw)
        else:
            self._page(saved=self.path.startswith("/?saved"))

    def do_POST(self):
        if self.path.startswith("/action"):
            act = ""
            try:
                length = int(self.headers.get("Content-Length", 0))
                body = self.rfile.read(length).decode("utf-8", "replace")
                try:
                    act = str(json.loads(body).get("action", ""))
                except Exception:
                    act = (urllib.parse.parse_qs(body)
                           .get("action", [""])[0] or "").strip()
            except Exception:
                act = ""
            if act == "clear":
                self.bridge.relay.clear()
            raw = b'{"ok": true}'
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(raw)))
            self.end_headers()
            self.wfile.write(raw)
        elif self.path.startswith("/calibrate"):
            try:
                length = int(self.headers.get("Content-Length", 0))
                form = urllib.parse.parse_qs(
                    self.rfile.read(length).decode("utf-8", "replace"))
                cal = (form.get("cal", ["0"])[0] or "0").strip()
            except Exception:
                cal = "0"
            self.bridge.calibrate = (cal == "1")
            self.send_response(303)
            self.send_header("Location", "/?saved=1")
            self.end_headers()
        elif self.path.startswith("/save"):
            try:
                length = int(self.headers.get("Content-Length", 0))
                form = urllib.parse.parse_qs(
                    self.rfile.read(length).decode("utf-8", "replace"))
                sid = (form.get("session_id", [""])[0] or "").strip()
            except Exception:
                sid = ""
            self.bridge.set_session_id(sid)
            self.send_response(303)
            self.send_header("Location", "/?saved=1")
            self.end_headers()
        else:
            self.send_response(404)
            self.end_headers()


class SetupServer:
    def __init__(self, bridge, port=SETUP_PORT):
        self._server = None
        self._thread = None
        self._bridge = bridge
        self._port = port
        self.bound_port = None

    def start(self):
        handler = type("BoundSetupHandler", (_SetupHandler,),
                       {"bridge": self._bridge})
        last_err = None
        for cand in range(self._port, self._port + 10):
            try:
                self._server = ThreadingHTTPServer(("0.0.0.0", cand),
                                                   handler)
                self.bound_port = cand
                break
            except OSError as exc:
                last_err = exc
                self._server = None
        if self._server is None:
            raise last_err
        self._server.daemon_threads = True
        self._thread = threading.Thread(target=self._server.serve_forever,
                                        daemon=True)
        self._thread.start()

    def stop(self):
        if self._server is not None:
            try:
                self._server.shutdown()
                self._server.server_close()
            except Exception:
                pass
            self._server = None


# ----------------------------------------------------------------------
# Core: settings + relay + setup page + state file for the window.
# ----------------------------------------------------------------------

class ChatBridge:
    def __init__(self):
        self._lock = threading.Lock()
        self.settings = load_settings()
        self.relay = MrgsRelay(self._session_id)
        self.server = SetupServer(self)
        self._actions_pos = 0
        self._last_state = ""

    def _session_id(self):
        with self._lock:
            return self.settings.get("ssn_session_id", "")

    def set_session_id(self, sid):
        with self._lock:
            self.settings = {"ssn_session_id": sid}
            save_settings(self.settings)

    def start(self):
        self.relay.start()
        try:
            self.server.start()
        except Exception:
            pass                     # chat still works without the page

    def stop(self):
        self.relay.stop()
        self.server.stop()

    def snapshot(self):
        snap = self.relay.snapshot()
        return {"product": PRODUCT, "version": VERSION,
                "connected": snap["connected"],
                "has_session": snap["has_session"],
                "messages": snap["messages"],
                "relay_error": snap.get("error", ""),
                "calibrate": bool(getattr(self, "calibrate", False)),
                "attempts": snap.get("attempts", 0),
                "setup_url": SETUP_URL,
                "chat_url": "http://%s:%d/chat" % (
                    _lan_ip(),
                    getattr(getattr(self, "server", None),
                            "bound_port", None) or SETUP_PORT)}

    # -- native-window bridge: actions in, state out -------------------

    def _actions_path(self):
        return os.path.join(plugin_dir(), ACTIONS_FILE)

    def _poll_actions(self):
        path = self._actions_path()
        try:
            size = os.path.getsize(path)
        except Exception:
            return
        if size < self._actions_pos:
            self._actions_pos = 0
        if size == self._actions_pos:
            return
        try:
            with open(path, "r", encoding="utf-8") as fh:
                fh.seek(self._actions_pos)
                chunk = fh.read()
                self._actions_pos = fh.tell()
        except Exception:
            return
        for line in chunk.splitlines():
            try:
                act = json.loads(line)
            except Exception:
                continue
            if not isinstance(act, dict):
                continue
            if act.get("action") == "clear":
                self.relay.clear()
            elif act.get("action") == "set_session":
                self.set_session_id(str(act.get("id", "") or ""))

    def tick(self):
        """Called ~2x/second by the flight loop (and by tests)."""
        self._poll_actions()
        text = json.dumps(self.snapshot())
        if text != self._last_state:
            self._last_state = text
            try:
                _atomic_write(os.path.join(plugin_dir(), STATE_FILE),
                              text)
            except Exception:
                pass


# ----------------------------------------------------------------------
# XPPython3 interface.
# ----------------------------------------------------------------------

try:
    from XPLMDefs import *
    from XPLMDisplay import *
    from XPLMGraphics import *
    from XPLMMenus import *
    from XPLMProcessing import *
    from XPLMDataAccess import *
    from XPLMUtilities import *
except Exception:
    pass


def _fn(name):
    """Resolve an X-Plane function by name (None when unavailable)."""
    return globals().get(name)


def _debug(msg):
    fn = _fn("XPLMDebugString")
    if fn is not None:
        try:
            fn("MRGS VR Chat Bridge: %s\n" % msg)
        except Exception:
            pass


class PythonInterface:
    def __init__(self):
        self.bridge = None

    def XPluginStart(self):
        self.Name = "MRGS VR Chat Bridge"
        self.Sig = "mrgs.vrchatbridge"
        self.Desc = ("MrGameSimulation VR Chat Bridge - Social "
                     "Stream Ninja chat in X-Plane and VR")
        self.bridge = ChatBridge()
        self.bridge.start()
        register = _fn("XPLMRegisterFlightLoopCallback")
        if register is not None:
            try:
                register(self._loop, 0.5, None)
            except Exception as e:
                _debug("flight loop failed: %s" % e)
        _debug("engine loaded (v%s), setup at %s" % (VERSION, SETUP_URL))
        return self.Name, self.Sig, self.Desc

    def _loop(self, elapsed1, elapsed2, counter, refcon):
        try:
            if self.bridge is not None:
                self.bridge.tick()
        except Exception:
            pass
        return 0.5

    def XPluginStop(self):
        unregister = _fn("XPLMUnregisterFlightLoopCallback")
        if unregister is not None:
            try:
                unregister(self._loop, None)
            except Exception:
                pass
        if self.bridge is not None:
            self.bridge.stop()
            self.bridge = None

    def XPluginEnable(self):
        return 1

    def XPluginDisable(self):
        pass

    def XPluginReceiveMessage(self, inFromWho, inMessage, inParam):
        pass
