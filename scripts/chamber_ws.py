#!/usr/bin/env python3
"""Thermal-chamber client over WebSocket, replacing the HTTP /api path.

WHY NOT HTTP
The ESP32 reset twice during an unattended run while being polled over
HTTP /api/status and commanded over /api/command. Each reset silently cleared
air_ctrl, dropped the illumination LED from 30% to 5% and parked the PSU --
breaking temperature control and all photometry at once, with no error
anywhere. The web UI, which uses the WebSocket, has been stable. So this
drives the chamber the same way the UI does.

It is also simply less work for the device: telemetry is PUSHED about every
500 ms, so a long soak costs zero requests instead of one HTTP round trip
every poll.

Protocol (from esp32/src/web_server.cpp onWsEvent):
    -> {"type":"ping"}                     <- {"type":"pong"}
    -> {"type":"get_snapshot"}             <- {"type":"telemetry","data":{...}}
    -> {"type":"command","data":{...}}     <- {"type":"command_result",...}
    on connect:                            <- {"type":"hello","data":{...}}
    unsolicited:                           <- {"type":"telemetry","data":{...}}
"""
import json
import threading
import time

import os

import websocket

# Override with RIG_CHAMBER_HOST when the rig moves; see rig.py.
URL = f"ws://{os.environ.get('RIG_CHAMBER_HOST', '10.0.66.142')}/ws"


class Chamber:
    """Persistent WebSocket connection with auto-reconnect.

    Reconnects on its own because the device DOES still reset occasionally;
    the point here is that a reset becomes a visible, recoverable event rather
    than a silent stall. `reboots` counts them so a run can report honestly
    how stable the hardware was.
    """

    def __init__(self, url=URL, stale_s=10.0):
        self.url = url
        self.stale_s = stale_s
        self._status = None
        self._status_ts = 0.0
        self._result = None
        self._result_ev = threading.Event()
        self._lock = threading.Lock()
        self._ws = None
        self._stop = False
        self._last_uptime = 0.0
        self.reboots = 0
        self.connects = 0
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    # ---------------------------------------------------------------- internals
    def _on_message(self, _ws, raw):
        try:
            m = json.loads(raw)
        except Exception:
            return
        t = m.get("type")
        if t in ("telemetry", "hello"):
            d = m.get("data")
            if isinstance(d, dict):
                with self._lock:
                    self._status = d
                    self._status_ts = time.time()
                up = d.get("uptime_ms", 0) / 1000.0
                if up < self._last_uptime - 5:
                    self.reboots += 1
                    print(f"[chamber] RESET detected "
                          f"(uptime {self._last_uptime:.0f}s -> {up:.0f}s), "
                          f"total {self.reboots}", flush=True)
                self._last_uptime = up
        elif t == "command_result":
            self._result = m.get("result")
            self._result_ev.set()

    def _run(self):
        while not self._stop:
            try:
                self._ws = websocket.WebSocketApp(
                    self.url, on_message=self._on_message)
                self.connects += 1
                self._ws.run_forever(ping_interval=20, ping_timeout=10)
            except Exception as e:
                print(f"[chamber] ws error {type(e).__name__}: {e}", flush=True)
            if not self._stop:
                time.sleep(3)

    # ------------------------------------------------------------------- public
    def wait_ready(self, timeout=30):
        t0 = time.time()
        while time.time() - t0 < timeout:
            if self.status() is not None:
                return True
            time.sleep(0.5)
        return False

    def status(self):
        """Latest pushed telemetry, or None if stale/never received."""
        with self._lock:
            if self._status is None:
                return None
            if time.time() - self._status_ts > self.stale_s:
                return None
            return self._status

    def command(self, payload, timeout=8.0):
        """Send a command, wait for its result. -> (ok, result)"""
        if self._ws is None or not self._ws.sock:
            return False, {"error": "not_connected"}
        self._result = None
        self._result_ev.clear()
        try:
            self._ws.send(json.dumps({"type": "command", "data": payload}))
        except Exception as e:
            return False, {"error": f"send_failed:{type(e).__name__}"}
        if not self._result_ev.wait(timeout):
            return False, {"error": "timeout"}
        res = self._result or {}
        return bool(res.get("ok")), res

    def air_temp(self):
        st = self.status()
        if not st:
            return None
        return (st.get("sht45") or {}).get("t_c")

    def air_enabled(self):
        st = self.status()
        if not st:
            return None
        return bool((st.get("air_control") or {}).get("enabled"))

    def led_pct(self):
        st = self.status()
        return None if not st else st.get("led_pct")

    def close(self):
        self._stop = True
        try:
            if self._ws:
                self._ws.close()
        except Exception:
            pass


if __name__ == "__main__":
    import sys
    c = Chamber()
    if not c.wait_ready():
        sys.exit("no telemetry from chamber")
    st = c.status()
    print("connected. uptime_s =", round(st.get("uptime_ms", 0) / 1000))
    print("air enabled:", c.air_enabled(), "target",
          (st.get("air_control") or {}).get("target_c"))
    print("led:", c.led_pct(), "air T:", c.air_temp())
    if "--restore" in sys.argv:
        print("set_led ->", c.command({"cmd": "set_led", "pct": 30.0}))
        print("air_ctrl ->", c.command({"cmd": "air_ctrl", "enable": True,
                                        "target_c": 25.0}))
    if "--watch" in sys.argv:
        t0 = time.time()
        while time.time() - t0 < float(sys.argv[sys.argv.index("--watch") + 1]):
            s = c.status() or {}
            ac = s.get("air_control") or {}
            print(f"  {time.strftime('%H:%M:%S')} "
                  f"up={round(s.get('uptime_ms',0)/1000):5d}s "
                  f"air_en={ac.get('enabled')} led={s.get('led_pct')} "
                  f"T={(s.get('sht45') or {}).get('t_c')} "
                  f"I={(s.get('psu') or {}).get('i_out')} "
                  f"reboots={c.reboots}", flush=True)
            time.sleep(10)
    c.close()
