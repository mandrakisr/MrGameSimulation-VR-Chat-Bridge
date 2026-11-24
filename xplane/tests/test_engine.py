import importlib.util, json, sys, time
spec = importlib.util.spec_from_file_location(
    "mrgs", "xplane/PythonPlugins/PI_MRGSChatBridge.py")
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
fails = []
def check(name, cond):
    print(("PASS " if cond else "FAIL ") + name)
    if not cond: fails.append(name)

# failing factory: error recorded, sid sanitized, attempts climb
def bad_factory(accept, on_open, on_close):
    raise OSError("boom while joining SECRETID99")
r = m.MrgsRelay(lambda: "SECRETID99", ws_factory=bad_factory)
r.start(); time.sleep(2.6); snap = r.snapshot(); r.stop()
check("failing relay not connected", snap["connected"] is False)
check("attempts climb", snap["attempts"] >= 2)
check("error recorded", "OSError" in snap["error"] and "boom" in snap["error"])
check("sid sanitized from error", "SECRETID99" not in snap["error"])

# working fake ws
class FakeWS:
    def __init__(self, accept, on_open, on_close):
        self._a, self._o = accept, on_open; self.n = 0
    def connect(self): self._o()
    def poll(self, t):
        self.n += 1
        if self.n == 2:
            self._a(json.dumps({"chatname": "Tester", "chatmessage": "hi <b>there</b>", "type": "twitch"}).encode(), False)
        time.sleep(0.02); return True
    def close(self): pass
r2 = m.MrgsRelay(lambda: "ABC123", ws_factory=lambda a, o, c: FakeWS(a, o, c))
r2.start(); time.sleep(1.2); s2 = r2.snapshot(); r2.stop()
check("working relay connects", s2["connected"] is True)
check("message cleaned", s2["messages"] and s2["messages"][0]["text"] == "hi there")
check("no error on success", s2["error"] == "")

# bridge snapshot carries diagnostics
b = m.ChatBridge.__new__(m.ChatBridge)
b.settings = {"ssn_session_id": "ABC123"}
b.relay = r2
bs = b.snapshot()
check("state has relay_error + attempts", "relay_error" in bs and "attempts" in bs)



# --- real end-to-end: local RFC6455 server + REAL BridgeWebSocket -------
import base64, hashlib, socket, struct, threading
def ws_server_once(port, payload):
    srv = socket.socket(); srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port)); srv.listen(1); srv.settimeout(10)
    conn, _ = srv.accept(); conn.settimeout(10)
    req = b""
    while b"\r\n\r\n" not in req: req += conn.recv(4096)
    key = [l.split(":", 1)[1].strip() for l in req.decode("latin1").split("\r\n")
           if l.lower().startswith("sec-websocket-key")][0]
    accept = base64.b64encode(hashlib.sha1(
        (key + m.WS_GUID).encode("ascii")).digest()).decode("ascii")
    conn.sendall(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                  "Connection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n" % accept).encode())
    body = payload.encode()
    conn.sendall(bytes([0x81, len(body)]) + body)
    time.sleep(1.0); conn.close(); srv.close()

class LocalRelay(m.MrgsRelay):
    URL = "ws://127.0.0.1:17931/join/%s/4"
t = threading.Thread(target=ws_server_once, args=(17931,
    json.dumps({"chatname": "RealPath", "chatmessage": "over the <i>real</i> socket", "type": "youtube"})), daemon=True)
t.start()
r3 = LocalRelay(lambda: "TESTID")
r3.start()
ok = False
for _ in range(40):
    s3 = r3.snapshot()
    if s3["messages"]: ok = True; break
    time.sleep(0.25)
r3.stop()
check("REAL BridgeWebSocket handshake + frame", ok and s3["messages"][0]["name"] == "RealPath"
      and s3["messages"][0]["text"] == "over the real socket")
check("real path no error", s3["error"] == "")



# --- setup page renders the brand + working form --------------------------
import urllib.request
b2 = m.ChatBridge.__new__(m.ChatBridge)
b2.settings = {"ssn_session_id": ""}
b2.relay = m.MrgsRelay(lambda: "")
srv = m.SetupServer(b2, port=17933); srv.start()
time.sleep(0.4)
html = urllib.request.urlopen("http://127.0.0.1:17933/", timeout=5).read().decode()
check("setup page branded", "MrGameSimulation" in html and "VR CHAT BRIDGE" in html
      and "Mandraki Bey" in html)
check("setup form intact", 'name="session_id"' in html and 'action="/save"' in html)
srv.stop()
print("FAILURES:", fails if fails else "none")
import sys; sys.exit(1 if fails else 0)
