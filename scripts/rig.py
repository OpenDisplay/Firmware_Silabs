#!/usr/bin/env python3
"""One handle on the whole measurement rig: chamber + lighting + camera.

Measurement scripts should talk to this instead of poking the chamber, the
lamp and the camera separately, because those three are not independent:

* The chamber drives the measurement LAMP. A chamber reset silently clears
  air_ctrl AND drops the lamp 30% -> 5%, so a crash corrupts photometry and
  temperature at the same time with no error anywhere.
* Captures and the live preview fight over the camera. go2rtc keeps
  rpicam-vid alive while a browser preview is attached and respawns it within
  well under a second, so a capture that just kills the producer and shoots
  loses the race whenever someone has the web UI open.

So: lamp asserted and verified around every capture, chamber state checked
before trusting a frame, and captures retried against the preview the way the
Pi's own still_server does it.

CLI:
    rig.py status
    rig.py temp 25 --wait        set air setpoint, optionally soak
    rig.py lamp 30
    rig.py capture TAG           locked-exposure 12-bit DNG + PNG
    rig.py selftest --loops 10   hammer both subsystems and report
"""
import argparse
import json
import os
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

# The rig moves between locations and its two boxes land on whatever the local
# DHCP hands out, so these are env-overridable: export RIG_PI_HOST /
# RIG_CHAMBER_HOST rather than editing three files. (They were hardcoded in
# rig.py AND chamber_ws.py, which is how a relocation turned into a hunt.)
PI_HOST = os.environ.get("RIG_PI_HOST", "10.0.38.217")
PI_USER = os.environ.get("RIG_PI_USER", "epapercam")
PI = f"{PI_USER}@{PI_HOST}"
CHAMBER_HOST = os.environ.get("RIG_CHAMBER_HOST", "10.0.66.142")
# NOT /tmp on the Pi: that is a 453 MB tmpfs and a 12-bit DNG is ~25 MB.
# Filling it makes rpicam-still write a TRUNCATED DNG that passes a size check
# and only fails later as "TiffFile contains no pages".
PI_RAW_DIR = "/home/epapercam/hqcap"
LAMP_PCT = 30.0                 # the level all photometric calibration assumes
SHUTTER_US = 50000              # white cell lands ~0.50 linear, nothing clips
FULL_DNG_BYTES = 20_000_000     # a full-frame IMX477 DNG is ~24.8 MB


def _ssh(cmd, timeout=180):
    return subprocess.run(["ssh", "-o", "ConnectTimeout=10", PI, cmd],
                          capture_output=True, text=True, timeout=timeout)


class Rig:
    def __init__(self, connect_chamber=True):
        self.ch = None
        if connect_chamber:
            from chamber_ws import Chamber                         # noqa: PLC0415
            self.ch = Chamber()
            self.ch.wait_ready(25)

    # ------------------------------------------------------------- chamber
    def status(self):
        st = self.ch.status() if self.ch else None
        if not st:
            return {"chamber": None}
        ac = st.get("air_control") or {}
        return {
            "uptime_s": round(st.get("uptime_ms", 0) / 1000),
            "air_c": (st.get("sht45") or {}).get("t_c"),
            "air_enabled": bool(ac.get("enabled")),
            "target_c": ac.get("target_c"),
            "lamp_pct": st.get("led_pct"),
            "psu_i": (st.get("psu") or {}).get("i_out"),
            "reboots_seen": self.ch.reboots,
        }

    def set_temp(self, target_c, ramp_step=4.0, settle_s=45):
        """Walk the setpoint there. A big jump commands maximum TEC current
        immediately, which is when this controller has historically reset."""
        cur = self.ch.air_temp()
        if cur is None:
            return self._air(target_c)
        while abs(cur - target_c) > ramp_step:
            cur += ramp_step if target_c > cur else -ramp_step
            self._air(cur)
            time.sleep(settle_s)
            now = self.ch.air_temp()
            if now is not None:
                cur = max(min(cur, now + ramp_step), now - ramp_step)
        return self._air(target_c)

    def _air(self, target_c):
        ok, _ = self.ch.command({"cmd": "air_ctrl", "enable": True,
                                 "target_c": float(target_c)})
        return ok

    def ensure_regulating(self, target_c):
        """Re-assert control and lamp. Returns notes describing any repair.

        Checked on every poll rather than once per setpoint: a reset leaves
        target_c reading correctly while nothing is actually regulating, so
        'I set the setpoint' is not evidence that anything is happening.
        """
        notes = []
        st = self.ch.status() or {}
        if not (st.get("air_control") or {}).get("enabled"):
            notes.append("air_ctrl was off")
            self._air(target_c)
        led = st.get("led_pct")
        if led is None or abs(led - LAMP_PCT) > 0.5:
            notes.append(f"lamp was {led}")
            self.set_lamp()
        return notes

    def soak(self, target_c, tol=2.0, hold_s=300, timeout_s=3600, poll=20):
        t0 = time.time()
        in_band = None
        while time.time() - t0 < timeout_s:
            self.ensure_regulating(target_c)
            t = self.ch.air_temp()
            if t is not None:
                if abs(t - target_c) <= tol:
                    in_band = in_band or time.time()
                    if time.time() - in_band >= hold_s:
                        return True, t
                else:
                    in_band = None
            time.sleep(poll)
        return False, self.ch.air_temp()

    # ------------------------------------------------------------- lighting
    def set_lamp(self, pct=LAMP_PCT, verify=True):
        ok, _ = self.ch.command({"cmd": "set_led", "pct": float(pct)})
        if not verify:
            return ok
        time.sleep(1.5)
        got = self.ch.led_pct()
        return got is not None and abs(got - pct) < 0.5

    # --------------------------------------------------------------- camera
    def capture_http(self, tag, outdir, shutter_us=SHUTTER_US, gain=1.0,
                     awb_r=1.6, awb_b=1.6, timeout=150):
        """Capture via still_server's /api/cameras/hq/raw.

        Preferred over the SSH path because the server holds its OWN camera
        lock, so a measurement capture and someone clicking snapshot in the web
        UI are serialised rather than racing. The SSH path can only retry
        politely around the collision; this prevents it.
        """
        os.makedirs(outdir, exist_ok=True)
        dst = os.path.join(outdir, f"{tag}.dng")
        url = (f"http://{PI_HOST}:8080/api/cameras/hq/raw?tag={tag}"
               f"&shutter_us={int(shutter_us)}&gain={gain}"
               f"&awb_r={awb_r}&awb_b={awb_b}")
        r = subprocess.run(["curl", "-s", "--max-time", str(timeout),
                            "-o", dst, "-w", "%{http_code}", "-X", "POST", url],
                           capture_output=True, text=True)
        code = (r.stdout or "").strip()
        out = {"tag": tag, "ok": False, "files": [], "warnings": [],
               "via": "http"}
        if code != "200":
            detail = ""
            try:
                detail = Path(dst).read_text(errors="replace")[:160]
            except Exception:
                pass
            out["warnings"].append(f"http {code}: {detail}")
            Path(dst).unlink(missing_ok=True)
            return out
        size = os.path.getsize(dst) if os.path.exists(dst) else 0
        if size < FULL_DNG_BYTES:
            out["warnings"].append(f"short dng {size}B")
            return out
        out["ok"] = True
        out["files"].append(dst)
        return out

    def capture(self, tag, outdir, shutter_us=SHUTTER_US, gain=1.0,
                awb_r=1.6, awb_b=1.6, dng=True, attempts=8):
        """Locked-exposure capture, retried against the live preview.

        go2rtc respawns the preview producer in well under a second, so a
        single kill-then-shoot loses the race whenever the web UI is open --
        which is exactly when a capture is most likely to be wanted. The Pi's
        own still_server solves this by killing, attempting IMMEDIATELY, and
        retrying on a camera-busy error; this mirrors that, so captures and
        the browser preview can coexist instead of knocking each other over.
        The preview comes back on its own afterwards (go2rtc relaunches it).
        """
        os.makedirs(outdir, exist_ok=True)
        args = [
            "rpicam-still", "-n", "--immediate", "-t", "500",
            "--shutter", str(int(shutter_us)), "--gain", str(gain),
            "--awbgains", f"{awb_r},{awb_b}", "--denoise", "off",
            "-e", "png", "-o", f"{PI_RAW_DIR}/{tag}.png",
        ]
        if dng:
            args.append("--raw")
        # 'rpicam[-]vid' not 'rpicam-vid': over SSH the whole command string is
        # in bash's argv, so a literal pattern makes pkill match its OWN
        # session and kill it (ssh exits 255, no capture).
        shoot = (f"mkdir -p {PI_RAW_DIR} && "
                 f"for i in $(seq 1 {attempts}); do "
                 f"  pkill -f 'rpicam[-]vid' 2>/dev/null; sleep 0.15; "
                 f"  if {' '.join(args)} 2>/tmp/rpicap.err; then echo CAPTURE_OK; break; fi; "
                 f"  grep -qiE 'in use by another process|failed to acquire camera' "
                 f"     /tmp/rpicap.err || break; "
                 f"done; tail -2 /tmp/rpicap.err")
        r = _ssh(shoot)
        ok = "CAPTURE_OK" in (r.stdout or "")
        out = {"tag": tag, "ok": ok, "files": [], "warnings": []}
        if not ok:
            out["warnings"].append((r.stdout or r.stderr or "")[-200:])
            return out
        for ext in (("png", "dng") if dng else ("png",)):
            src = f"{PI_RAW_DIR}/{tag}.{ext}"
            dst = os.path.join(outdir, f"{tag}.{ext}")
            rs = _ssh(f"stat -c %s {src} 2>/dev/null || echo 0", timeout=30)
            rsize = int(rs.stdout.strip() or 0)
            if rsize == 0:
                out["warnings"].append(f"{ext}: missing on pi")
                continue
            c = subprocess.run(["scp", "-q", f"{PI}:{src}", dst],
                               capture_output=True, text=True)
            if c.returncode != 0 or not os.path.exists(dst):
                out["warnings"].append(f"{ext}: scp failed")
                continue
            if os.path.getsize(dst) != rsize:
                out["warnings"].append(f"{ext}: truncated transfer")
                continue
            if ext == "dng" and rsize < FULL_DNG_BYTES:
                out["warnings"].append(
                    f"dng short write {rsize}B — check free space on the Pi")
                continue
            out["files"].append(dst)
        return out

    def capture_verified(self, tag, outdir, target_c=None, **kw):
        """Capture with the lamp asserted first and the frame checked after.

        A chamber reset between the lamp check and the shutter would dim the
        frame ~6x while every other field still looked sane, so the captured
        backdrop is compared against a reference rather than trusting the
        lamp setting alone.
        """
        notes = []
        if self.ch is not None:
            if target_c is not None:
                notes += self.ensure_regulating(target_c)
            elif not self.set_lamp():
                notes.append("lamp could not be set")
        # Prefer the server-side endpoint (it holds the camera lock); fall
        # back to the direct path if it is not deployed or errors.
        res = self.capture_http(tag, outdir,
                                **{k: v for k, v in kw.items()
                                   if k in ("shutter_us", "gain", "awb_r", "awb_b")})
        if not res["ok"]:
            notes.append(f"http capture failed ({res['warnings'][:1]}), "
                         f"falling back to ssh")
            res = self.capture(tag, outdir, **kw)
            res["via"] = "ssh"
        res["notes"] = notes
        if self.ch is not None:
            led = self.ch.led_pct()
            if led is None or abs(led - LAMP_PCT) > 0.5:
                res["warnings"].append(f"lamp was {led} AFTER capture — "
                                       f"frame may be mis-lit")
        return res

    def preview_alive(self, timeout=20):
        """Is the live preview producer running again?"""
        r = _ssh("pgrep -f 'rpicam[-]vid' >/dev/null && echo YES || echo NO",
                 timeout=timeout)
        return "YES" in (r.stdout or "")

    def web_ok(self):
        """Both web UIs reachable — the user keeps these open while we work."""
        out = {}
        for name, url in (("camera", f"http://{PI_HOST}:8080/api/cameras"),
                          ("chamber", f"http://{CHAMBER_HOST}/api/health")):
            r = subprocess.run(["curl", "-s", "-o", "/dev/null", "-w", "%{http_code}",
                                "--max-time", "8", url],
                               capture_output=True, text=True)
            out[name] = r.stdout.strip() == "200"
        return out

    def close(self):
        if self.ch:
            self.ch.close()


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("status")
    p = sub.add_parser("temp"); p.add_argument("target", type=float)
    p.add_argument("--wait", action="store_true")
    p.add_argument("--tol", type=float, default=2.0)
    p = sub.add_parser("lamp"); p.add_argument("pct", type=float)
    p = sub.add_parser("capture"); p.add_argument("tag")
    p.add_argument("--outdir", default="/tmp/rigcap")
    p.add_argument("--shutter", type=int, default=SHUTTER_US)
    p = sub.add_parser("selftest"); p.add_argument("--loops", type=int, default=10)
    # Quick look that does NOT touch the camera: pulled from the live go2rtc
    # stream, so the preview keeps running and a capture in flight is safe.
    # Use `capture` when the numbers matter -- this is auto-exposed H264.
    p = sub.add_parser("snap"); p.add_argument("out")
    p.add_argument("--cam", default="hq", choices=("hq", "usb"))
    p.add_argument("--max-width", type=int, default=0)
    # High-frame-rate burst for watching things move (particles, not settling).
    p = sub.add_parser("video")
    p.add_argument("--preset", choices=("fast", "mid", "full"), default="fast")
    p.add_argument("--seconds", type=float, default=3.0)
    p.add_argument("--codec", default="h264", choices=("h264", "mjpeg"))
    p.add_argument("--shutter-us", type=int, default=0)
    p.add_argument("--gain", type=float, default=0)
    p.add_argument("--tag", default="burst")
    p.add_argument("--pull", default="", help="download to this path when done")
    sub.add_parser("health")
    p = sub.add_parser("recover"); p.add_argument("--services", action="store_true")
    p = sub.add_parser("pull"); p.add_argument("name"); p.add_argument("out")
    sub.add_parser("files")
    a = ap.parse_args()

    rig = Rig()
    try:
        if a.cmd == "status":
            print(json.dumps({**rig.status(), "web": rig.web_ok(),
                              "preview_alive": rig.preview_alive()}, indent=1))
        elif a.cmd == "temp":
            rig.set_temp(a.target)
            print(f"setpoint {a.target}")
            if a.wait:
                ok, t = rig.soak(a.target, tol=a.tol)
                print(f"soak {'ok' if ok else 'TIMEOUT'} at {t}")
        elif a.cmd == "lamp":
            print("lamp ok" if rig.set_lamp(a.pct) else "lamp FAILED")
        elif a.cmd == "capture":
            print(json.dumps(rig.capture_verified(
                a.tag, a.outdir, shutter_us=a.shutter), indent=1))
        elif a.cmd == "snap":
            import urllib.request                                  # noqa: PLC0415
            url = (f"http://{PI_HOST}:8080/api/snap?cam={a.cam}"
                   f"&max_width={a.max_width}")
            with urllib.request.urlopen(url, timeout=30) as r:
                data = r.read()
            Path(a.out).write_bytes(data)
            print(json.dumps({"ok": True, "file": a.out, "bytes": len(data),
                              "cam": a.cam}, indent=1))
        elif a.cmd == "video":
            import urllib.request                                  # noqa: PLC0415
            body = json.dumps({"preset": a.preset, "seconds": a.seconds,
                               "codec": a.codec, "shutter_us": a.shutter_us,
                               "gain": a.gain, "tag": a.tag}).encode()
            req = urllib.request.Request(
                f"http://{PI_HOST}:8080/api/video", data=body,
                headers={"Content-Type": "application/json"}, method="POST")
            with urllib.request.urlopen(req, timeout=a.seconds + 120) as r:
                out = json.load(r)
            print(json.dumps(out, indent=1))
            if a.pull and out.get("file"):
                u = f"http://{PI_HOST}:8080/api/files/{out['file']}"
                with urllib.request.urlopen(u, timeout=300) as r:
                    Path(a.pull).write_bytes(r.read())
                print(f"pulled -> {a.pull}")
        elif a.cmd == "health":
            import urllib.request                                  # noqa: PLC0415
            with urllib.request.urlopen(
                    f"http://{PI_HOST}:8080/api/health", timeout=20) as r:
                print(json.dumps(json.load(r), indent=1))
        elif a.cmd == "recover":
            import urllib.request                                  # noqa: PLC0415
            u = f"http://{PI_HOST}:8080/api/recover"
            if a.services:
                u += "?services=1"
            req = urllib.request.Request(u, data=b"", method="POST")
            with urllib.request.urlopen(req, timeout=120) as r:
                print(json.dumps(json.load(r), indent=1))
        elif a.cmd == "files":
            import urllib.request                                  # noqa: PLC0415
            with urllib.request.urlopen(
                    f"http://{PI_HOST}:8080/api/files", timeout=20) as r:
                print(json.dumps(json.load(r), indent=1))
        elif a.cmd == "pull":
            import urllib.request                                  # noqa: PLC0415
            with urllib.request.urlopen(
                    f"http://{PI_HOST}:8080/api/files/{a.name}", timeout=600) as r:
                Path(a.out).write_bytes(r.read())
            print(f"pulled {a.name} -> {a.out}")
        elif a.cmd == "selftest":
            from rig_selftest import selftest                      # noqa: PLC0415
            selftest(rig, a.loops)
    finally:
        rig.close()


if __name__ == "__main__":
    main()
