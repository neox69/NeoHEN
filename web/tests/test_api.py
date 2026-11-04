#!/usr/bin/env python3
"""End-to-end tests for the full NeoWEB API using the host emulator.

    make -C web host FULL=1 && python3 web/tests/test_api.py

Starts build/neoweb-host on a free port with a temporary console root,
seeds a fake PS5 and a fake PS4 title and exercises every endpoint.
"""
import ctypes
import json
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
import unittest
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
BIN = os.path.join(HERE, "..", "build", "neoweb-host")


def ptrace_restricted():
    """True when Linux only lets a process read the memory of its children."""
    try:
        with open("/proc/sys/kernel/yama/ptrace_scope") as f:
            return f.read().strip() != "0"
    except OSError:
        return False


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def make_sfo(entries):
    """Build a param.sfo: entries = [(key, value)] (str or int)."""
    keys = b""
    data = b""
    index = b""
    for key, val in entries:
        key_off = len(keys)
        keys += key.encode() + b"\0"
        if isinstance(val, int):
            raw, fmt, length, max_len = struct.pack("<I", val), 0x0404, 4, 4
        else:
            enc = val.encode() + b"\0"
            max_len = 128
            raw, fmt, length = enc.ljust(max_len, b"\0"), 0x0204, len(enc)
        index += struct.pack("<HHIII", key_off, fmt, length, max_len, len(data))
        data += raw
    while len(keys) % 4:
        keys += b"\0"
    key_tab = 20 + len(index)
    data_tab = key_tab + len(keys)
    hdr = struct.pack("<IIIII", 0x46535000, 0x101, key_tab, data_tab, len(entries))
    return hdr + index + keys + data


def make_ucp(files):
    """A TROPHY.UCP: header, one 64 byte entry per file, the files."""
    table = b""
    data = b""
    at = 0x60 + 0x40 * len(files)
    for name, body in files:
        table += name.encode().ljust(32, b"\0") + struct.pack(">QQ", at + len(data), len(body)) + b"\0" * 16
        data += body
    head = struct.pack(">IIQII", 0xB228C60A, 1, at + len(data), len(files), 0x40)
    return head.ljust(0x60, b"\0") + table + data


def make_trptitle(trophies):
    """A TRPTITLE.DAT with the one section NeoWEB reads: (number, unix time
    earned or 0) per trophy, in blocks of 0x400 that each end in a hash."""
    recs = b""
    for num, when in trophies:
        ticks = (when + 62135596800) * 1000000 if when else 0
        recs += b"\0" * 16 + struct.pack(">IIIIQ", num, 1 if when else 0, 0x2000 if when else 0, 0, ticks).ljust(0x50, b"\0")
    off = 0x380 + 13  # not on a block boundary, like the real ones
    body = b"T2TD" + struct.pack(">II", 0x10000, 2) + b"\0" * 0x24
    body += struct.pack(">IIIIQQ", 0x100, 0x40, 1, 1, 0x200, 0)
    body += struct.pack(">IIIIQQ", 0x800, 0x50, 1, len(trophies), off, 0)
    body = body.ljust(off, b"\0") + recs
    out = b"T2PD" + struct.pack(">III", 0x10000, 0, 0x420) + b"\0" * 0x30
    for i in range(0, len(body), 0x400):
        out += body[i:i + 0x400].ljust(0x400, b"\0") + os.urandom(0x20)
    return out


def fake_cheat_service(path, listing, seen):
    """NeoHEN's cheat service, as far as NeoWEB talks to it."""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(path)
    srv.listen(4)

    def run():
        while True:
            try:
                c, _ = srv.accept()
            except OSError:
                return
            buf = b""
            while len(buf) < 12 + 0x1000:
                part = c.recv(12 + 0x1000 - len(buf))
                if not part:
                    break
                buf += part
            if len(buf) == 12 + 0x1000:
                magic, cmd, _err = struct.unpack("<IIi", buf[:12])
                ask = json.loads(buf[12:].split(b"\0")[0])
                seen.append((cmd, ask))
                if cmd == 0x800000A:
                    with open(listing["local"], "w") as f:
                        json.dump(listing["json"], f)
                    var, err = listing["console"], 0
                elif ask.get("session_id") == "cheat-1-x":
                    var, err = "God mode -> " + ("enabled" if ask["enabled"] else "disabled"), 0
                else:
                    var, err = "stale", -1
                msg = json.dumps({"var": var}).encode().ljust(0x1000, b"\0")
                c.sendall(struct.pack("<IIi", magic, 0x8000002, err) + msg)
            c.close()

    import threading
    threading.Thread(target=run, daemon=True).start()
    return srv


def fake_util_service(path, answer, seen):
    """NeoHEN's util service: answer(cmd, ask) -> (var, err)."""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    if os.path.exists(path):
        os.unlink(path)
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(path)
    srv.listen(4)

    def run():
        while True:
            try:
                c, _ = srv.accept()
            except OSError:
                return
            buf = b""
            while len(buf) < 12 + 0x1000:
                part = c.recv(12 + 0x1000 - len(buf))
                if not part:
                    break
                buf += part
            if len(buf) == 12 + 0x1000:
                magic, cmd, _err = struct.unpack("<IIi", buf[:12])
                ask = json.loads(buf[12:].split(b"\0")[0])
                seen.append((cmd, ask))
                var, err = answer(cmd, ask)
                msg = json.dumps({"var": var}).encode().ljust(0x1000, b"\0")
                c.sendall(struct.pack("<IIi", magic, 0x8000002, err) + msg)
            c.close()

    import threading
    threading.Thread(target=run, daemon=True).start()
    return srv


class Api:
    def __init__(self, base):
        self.base = base

    def req(self, method, path, body=None, raw=None, headers=None):
        data = None
        hdrs = dict(headers or {})
        if body is not None:
            data = json.dumps(body).encode()
            hdrs["Content-Type"] = "application/json"
        elif raw is not None:
            data = raw
        r = urllib.request.Request(self.base + path, data=data, method=method, headers=hdrs)
        try:
            with urllib.request.urlopen(r, timeout=10) as resp:
                return resp.status, resp.read(), resp.headers
        except urllib.error.HTTPError as e:
            return e.code, e.read(), e.headers

    def get(self, path):
        st, body, _ = self.req("GET", path)
        return st, json.loads(body)

    def post(self, path, body=None):
        st, data, _ = self.req("POST", path, body=body if body is not None else {})
        return st, json.loads(data)

    def wait_job(self, job_id, timeout=20):
        end = time.time() + timeout
        while time.time() < end:
            _, j = self.get(f"/api/jobs?id={job_id}")
            if j["job"]["state"] != "running":
                return j["job"]
            time.sleep(0.05)
        raise AssertionError("job timeout")


class PayloadTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.root = tempfile.mkdtemp(prefix="fakeps5-")
        r = cls.root
        # a PS5 title
        os.makedirs(f"{r}/user/app/PPSA01234/sce_sys")
        os.makedirs(f"{r}/user/appmeta/PPSA01234")
        param = {
            "titleId": "PPSA01234",
            "contentId": "UP0000-PPSA01234_00-GAMETEST00000000",
            "contentVersion": "01.000.000",
            "applicationCategoryType": 0,
            "localizedParameters": {
                "defaultLanguage": "en-US",
                "en-US": {"titleName": "Test Game"},
                "es-ES": {"titleName": "Juego de Prueba"},
            },
        }
        for p in (f"{r}/user/appmeta/PPSA01234/param.json",
                  f"{r}/user/app/PPSA01234/sce_sys/param.json"):
            with open(p, "w") as f:
                json.dump(param, f)
        with open(f"{r}/user/appmeta/PPSA01234/icon0.png", "wb") as f:
            f.write(b"\x89PNG\r\n\x1a\nFAKEICON")
        # a PS4 title
        os.makedirs(f"{r}/user/appmeta/CUSA00001")
        with open(f"{r}/user/appmeta/CUSA00001/param.sfo", "wb") as f:
            f.write(make_sfo([("APP_VER", "01.00"), ("ATTRIBUTE", 0),
                              ("CATEGORY", "gd"),
                              ("CONTENT_ID", "EP0000-CUSA00001_00-PS4GAME000000000"),
                              ("TITLE", "Old PS4 Game"), ("TITLE_ID", "CUSA00001")]))
        # a mounted game for the dumper
        os.makedirs(f"{r}/mnt/sandbox/pfsmnt/PPSA01234-app0/data")
        with open(f"{r}/mnt/sandbox/pfsmnt/PPSA01234-app0/eboot.bin", "wb") as f:
            f.write(os.urandom(4096))
        with open(f"{r}/mnt/sandbox/pfsmnt/PPSA01234-app0/data/big.bin", "wb") as f:
            f.write(os.urandom(3 * 1024 * 1024 + 17))

        # two users with saves, and a "USB drive"
        for user, name in (("1d31cd9c", b"NeoX ESP"), ("1d31cda3", b"TaToR")):
            os.makedirs(f"{r}/user/home/{user}")
            with open(f"{r}/user/home/{user}/username.dat", "wb") as f:
                f.write(name + b"\0" * (17 - len(name)))
        cls.save = f"{r}/user/home/1d31cd9c/savedata_prospero/PPSA01234"
        os.makedirs(cls.save)
        os.makedirs(f"{r}/user/home/1d31cd9c/savedata_prospero_meta/user/PPSA01234")
        os.makedirs(f"{r}/user/home/1d31cd9c/savedata_prospero/sce_backup")
        os.makedirs(f"{r}/user/home/1d31cda3/savedata/CUSA00001")
        with open(f"{cls.save}/sdimg_SAVEDATA0", "wb") as f:
            f.write(os.urandom(200 * 1024 + 3))
        with open(f"{r}/user/home/1d31cd9c/savedata_prospero_meta/user/PPSA01234/s_icon0.png", "wb") as f:
            f.write(b"PNG!" * 100)
        with open(f"{r}/user/home/1d31cda3/savedata/CUSA00001/sdimg_SAVEDATA", "wb") as f:
            f.write(os.urandom(5000))
        os.makedirs(f"{r}/mnt/usb0")
        # a screenshot and a clip, with the .dat the console writes next to them
        dat = b"\x01\x00\x59\x12" + bytes.fromhex("9ccd311d") + b"\0" * 0x3c + b"My Game: II_20250615150127\0" + b"\0" * 900
        for kind, name, body in (("photo", "20250615_150127_00997366.jxr", b"II\xbc\x01" + b"x" * 3000),
                                 ("video", "20261005_180320_00660961.webm", b"\x1a\x45\xdf\xa3" + b"v" * 9000)):
            d = f"{r}/user/av_contents/{kind}/NPXS40087/PPSA01234/2fc"
            os.makedirs(d, exist_ok=True)
            with open(f"{d}/{name}", "wb") as f:
                f.write(body)
            with open(f"{d}/{name.rsplit('.', 1)[0]}.dat", "wb") as f:
                f.write(dat)
            with open(f"{d}/{name.rsplit('.', 1)[0]}.meta", "wb") as f:
                f.write(b"{}")
        # the console keeps a thumbnail of the screenshot, not of the clip here
        t = f"{r}/user/av_contents/thumbnails/photo/NPXS40087/PPSA01234/2fc"
        os.makedirs(t)
        with open(f"{t}/20250615_150127_00997366.jxr.jxr", "wb") as f:
            f.write(b"II\xbc\x01" + b"t" * 500)

        # trophies kept by the console itself: one set, two of its three earned
        conf = {"defaultLanguage": "en-US", "trophies": [
            {"id": "0000", "grade": "P", "hidden": False},
            {"id": "0001", "grade": "G", "hidden": False},
            {"id": "0002", "grade": "B", "hidden": True}]}
        names = {"metadata": {"titleMetadata": {"name": "Juego de Prueba"}, "trophyMetadata": [
            {"id": "0000", "name": "Todo", "detail": "Consigue todos"},
            {"id": "0001", "name": "Oro", "detail": "Haz algo"},
            {"id": "0002", "name": "Secreto", "detail": "No se dice"}]}}
        os.makedirs(f"{r}/user/trophy2/nobackup/conf/NPWR11111_00")
        with open(f"{r}/user/trophy2/nobackup/conf/NPWR11111_00/TROPHY.UCP", "wb") as f:
            f.write(make_ucp([("icon0_en-US.png", b"\x89PNG\r\n\x1a\nSET"),
                              ("trop0001.png", b"\x89PNG\r\n\x1a\nGOLD"),
                              ("tropconf.json", json.dumps(conf).encode()),
                              ("tropmeta_es-ES.json", json.dumps(names).encode())]))
        os.makedirs(f"{r}/user/home/1d31cd9c/trophy2/nobackup/data/NPWR11111_00")
        with open(f"{r}/user/home/1d31cd9c/trophy2/nobackup/data/NPWR11111_00/TRPTITLE.DAT", "wb") as f:
            f.write(make_trptitle([(0, 0), (1, 1791460000), (2, 1791463600)]))
        os.makedirs(f"{r}/user/app/PPSA01234/sce_sys/trophy2")
        with open(f"{r}/user/app/PPSA01234/sce_sys/trophy2/npbind.dat", "wb") as f:
            f.write(b"\xd2\x94\xa0\x18" + b"\0" * 80 + b"NPWR11111_00\0" + b"\0" * 60)

        # two games served by ShadowMount+ from images; the image of the first
        # is mounted and brings a library of its own
        for tid, chunks in (("PPSA01234", 7), ("PPSA05555", 60)):
            os.makedirs(f"{r}/user/app/{tid}/sce_sys", exist_ok=True)
            os.makedirs(f"{r}/data/homebrew", exist_ok=True)
            with open(f"{r}/data/homebrew/{tid} Game.ffpfsc", "wb") as f:
                f.write(os.urandom(300 * 1024 + 5))
            with open(f"{r}/user/app/{tid}/mount_img.lnk", "w") as f:
                f.write(f"/data/homebrew/{tid} Game.ffpfsc\n/mnt/shadowmnt/pfsc/x/{tid}.exfat")
            with open(f"{r}/user/app/{tid}/mount.lnk", "w") as f:
                f.write(f"/mnt/shadowmnt/{tid}_abcd")
            with open(f"{r}/user/app/{tid}/sce_sys/playgo-chunk.dat", "wb") as f:
                f.write(b"plgx" + b"\0" * 6 + struct.pack("<HHH", chunks, 0, 2) + b"\0" * 64)
        os.makedirs(f"{r}/mnt/shadowmnt/PPSA01234_abcd/sce_sys")
        os.makedirs(f"{r}/mnt/shadowmnt/PPSA01234_abcd/fakelib")
        with open(f"{r}/mnt/shadowmnt/PPSA01234_abcd/fakelib/libSceAmpr.sprx", "wb") as f:
            f.write(b"\x4f\x15\x3d\x1d" + b"A" * 999)
        # the stub, as a backport made by hand for some other game left it
        os.makedirs(f"{r}/data/homebrew/backports/PPSA07777/fakelib")
        with open(f"{r}/data/homebrew/backports/PPSA07777/fakelib/libScePlayGo.sprx", "wb") as f:
            f.write(b"\x4f\x15\x3d\x1d" + b"S" * 500)

        # a stand-in for ShadowMount+'s API: mounting a game makes its
        # mount point real, unmounting takes it away
        import http.server, threading
        root = r
        class Smp(http.server.BaseHTTPRequestHandler):
            calls = []
            def log_message(self, *a):
                pass
            def do_POST(self):
                body = json.loads(self.rfile.read(int(self.headers.get("Content-Length", 0))) or b"{}")
                Smp.calls.append((self.path, body))
                tid = body.get("title_id", "")
                mnt = f"{root}/mnt/shadowmnt/{tid}_abcd"
                if self.path == "/api/v1/games/mount":
                    os.makedirs(f"{mnt}/sce_sys", exist_ok=True)
                elif self.path == "/api/v1/games/unmount":
                    shutil.rmtree(mnt, ignore_errors=True)
                out = json.dumps({"status": 0}).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(out)))
                self.end_headers()
                self.wfile.write(out)
        cls.smp_calls = Smp.calls
        cls.smp = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Smp)
        threading.Thread(target=cls.smp.serve_forever, daemon=True).start()

        cls.port = free_port()
        cls.ctl_port = free_port()
        env = dict(os.environ, PS5AIO_ROOT=cls.root, NEOWEB_CTL_PORT=str(cls.ctl_port),
                   NEOWEB_TEST_USB="1", NEOWEB_PLAY_TICK_MS="100",
                   NEOWEB_SMP_PORT=str(cls.smp.server_address[1]))
        cls.proc = subprocess.Popen([BIN, str(cls.port)], env=env,
                                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        cls.api = Api(f"http://127.0.0.1:{cls.port}")
        for _ in range(100):
            try:
                cls.api.get("/api/ping")
                break
            except Exception:
                time.sleep(0.05)

    @classmethod
    def tearDownClass(cls):
        cls.proc.kill()
        cls.proc.wait()
        shutil.rmtree(cls.root, ignore_errors=True)

    # ------------------------------------------------------------ system
    def test_ping(self):
        st, j = self.api.get("/api/ping")
        self.assertEqual(st, 200)
        self.assertTrue(j["ok"])
        self.assertEqual(j["name"], "NeoWEB")
        self.assertEqual(j["platform"], "host")

    def test_sysinfo_and_mounts(self):
        _, j = self.api.get("/api/sys/info")
        self.assertIn("firmware", j)
        self.assertIn("ips", j)
        _, m = self.api.get("/api/sys/mounts")
        self.assertTrue(any(x["path"] == "/" for x in m["mounts"]))

    def test_procs(self):
        _, j = self.api.get("/api/sys/procs")
        self.assertTrue(any(p["pid"] == self.proc.pid for p in j["procs"]))

    def test_notify_and_log(self):
        st, _ = self.api.post("/api/sys/notify", {"text": "hola"})
        self.assertEqual(st, 200)
        _, j = self.api.get("/api/sys/log")
        self.assertTrue(any("hola" in e["msg"] for e in j["entries"]))

    def test_errors(self):
        st, j = self.api.get("/api/nope")
        self.assertEqual(st, 404)
        self.assertFalse(j["ok"])
        st, _, _ = self.api.req("DELETE", "/api/ping")
        self.assertEqual(st, 405)
        st, _, _ = self.api.req("POST", "/api/fs/mkdir", raw=b"{not json")
        self.assertEqual(st, 400)
        st, j = self.api.get("/api/fs/list?path=/user/../../etc")
        self.assertEqual(st, 400)

    def test_webui(self):
        st, body, hdr = self.api.req("GET", "/")
        self.assertEqual(st, 200)
        self.assertIn(b"NeoWEB", body)
        self.assertIn("text/html", hdr["Content-Type"])

    def test_webui_gzip_etag(self):
        import gzip
        for path, kind in (("/", "text/html"), ("/jxr.js", "javascript")):
            st, gz, hdr = self.api.req("GET", path, headers={"Accept-Encoding": "gzip, deflate"})
            self.assertEqual(st, 200)
            self.assertEqual(hdr["Content-Encoding"], "gzip")
            self.assertIn(kind, hdr["Content-Type"])
            self.assertEqual(hdr["Cache-Control"], "no-cache")
            self.assertEqual(hdr["Vary"], "Accept-Encoding")
            self.assertEqual(int(hdr["Content-Length"]), len(gz))
            etag = hdr["ETag"]
            self.assertRegex(etag, r'^"[0-9a-f]{24}-gz"$')
            # without gzip: the same bytes, inflated by the server
            st, plain, phdr = self.api.req("GET", path)
            self.assertEqual(st, 200)
            self.assertIsNone(phdr["Content-Encoding"])
            self.assertEqual(gzip.decompress(gz), plain)
            self.assertLess(len(gz) * 2, len(plain))
            self.assertEqual(phdr["ETag"], etag[:-4] + '"')
            # "gzip;q=0" is a no
            _, _, h = self.api.req("HEAD", path, headers={"Accept-Encoding": "gzip;q=0, br"})
            self.assertIsNone(h["Content-Encoding"])
            self.assertEqual(int(h["Content-Length"]), len(plain))
            # revalidation
            for inm in (etag, "W/" + etag, '"nope", ' + etag, "*"):
                st, body, h = self.api.req("GET", path, headers={"Accept-Encoding": "gzip",
                                                                 "If-None-Match": inm})
                self.assertEqual(st, 304, inm)
                self.assertEqual(body, b"")
                self.assertEqual(h["ETag"], etag)
            st, _, _ = self.api.req("GET", path, headers={"Accept-Encoding": "gzip",
                                                          "If-None-Match": '"nope"'})
            self.assertEqual(st, 200)
            # the plain copy has its own tag
            st, _, _ = self.api.req("GET", path, headers={"If-None-Match": etag})
            self.assertEqual(st, 200)
            st, _, _ = self.api.req("GET", path, headers={"If-None-Match": phdr["ETag"]})
            self.assertEqual(st, 304)

    def test_304_keeps_the_connection(self):
        import http.client
        c = http.client.HTTPConnection("127.0.0.1", self.port, timeout=5)
        self.addCleanup(c.close)
        c.request("GET", "/", headers={"Accept-Encoding": "gzip"})
        r = c.getresponse()
        r.read()
        etag = r.getheader("ETag")
        for _ in range(3):
            c.request("GET", "/", headers={"Accept-Encoding": "gzip", "If-None-Match": etag})
            r = c.getresponse()
            self.assertEqual(r.status, 304)
            self.assertEqual(r.read(), b"")
            self.assertEqual(r.getheader("Connection"), "keep-alive")

    def test_no_cors_headers(self):
        for method, path in (("GET", "/api/ping"), ("GET", "/"), ("OPTIONS", "/api/fs/delete")):
            st, _, hdr = self.api.req(method, path, headers={"Origin": "http://evil.example",
                                                             "Access-Control-Request-Method": "POST"})
            for h in hdr.keys():
                self.assertFalse(h.lower().startswith("access-control-"), (method, path, h))
            if method == "OPTIONS":
                self.assertEqual(st, 204)
                self.assertIn("POST", hdr["Allow"])

    def test_icon_etag(self):
        st, body, hdr = self.api.req("GET", "/apps/icon?id=PPSA01234")
        self.assertEqual(st, 200)
        self.assertTrue(body.startswith(b"\x89PNG"))
        self.assertEqual(hdr["Cache-Control"], "no-cache")
        etag = hdr["ETag"]
        self.assertEqual(etag, '"%x-%x"' % (len(body), int(os.stat(
            f"{self.root}/user/appmeta/PPSA01234/icon0.png").st_mtime)))
        st, body, _ = self.api.req("GET", "/apps/icon?id=PPSA01234", headers={"If-None-Match": etag})
        self.assertEqual(st, 304)
        self.assertEqual(body, b"")

    def _ranges(self, api, data, path):
        size = len(data)
        st, body, hdr = api.req("GET", "/fs/get?path=" + path)
        self.assertEqual(st, 200)
        self.assertEqual(body, data)
        self.assertEqual(hdr["Accept-Ranges"], "bytes")
        for rng, lo, hi in (("bytes=0-0", 0, 0), ("bytes=-17", size - 17, size - 1),
                            ("bytes=1048570-2097160", 1048570, 2097160),
                            ("bytes=%d-" % (size - 1000), size - 1000, size - 1),
                            ("bytes=5-%d" % (size * 2), 5, size - 1)):
            st, body, hdr = api.req("GET", "/fs/get?path=" + path, headers={"Range": rng})
            self.assertEqual(st, 206, rng)
            self.assertEqual(body, data[lo:hi + 1], rng)
            self.assertEqual(hdr["Content-Range"], "bytes %d-%d/%d" % (lo, hi, size))
        st, _, hdr = api.req("GET", "/fs/get?path=" + path, headers={"Range": "bytes=%d-" % size})
        self.assertEqual(st, 416)
        self.assertEqual(hdr["Content-Range"], "bytes */%d" % size)
        st, body, hdr = api.req("HEAD", "/fs/get?path=" + path, headers={"Range": "bytes=10-19"})
        self.assertEqual(st, 206)
        self.assertEqual(body, b"")
        self.assertEqual(hdr["Content-Length"], "10")

    def test_range_downloads(self):
        data = os.urandom(3 * 1024 * 1024 + 333)
        os.makedirs(f"{self.root}/data/rng", exist_ok=True)
        with open(f"{self.root}/data/rng/f.bin", "wb") as f:
            f.write(data)
        self._ranges(self.api, data, "/data/rng/f.bin")          # sendfile()
        # and the read()/send() copy, for a platform without sendfile()
        port, ctl = free_port(), free_port()
        env = dict(os.environ, PS5AIO_ROOT=self.root, NEOWEB_CTL_PORT=str(ctl),
                   NEOWEB_NO_SENDFILE="1")
        proc = subprocess.Popen([BIN, str(port)], env=env,
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.addCleanup(proc.wait)
        self.addCleanup(proc.kill)
        api = Api(f"http://127.0.0.1:{port}")
        for _ in range(100):
            try:
                api.get("/api/ping")
                break
            except Exception:
                time.sleep(0.05)
        self._ranges(api, data, "/data/rng/f.bin")

    def test_connection_cap(self):
        socks = []
        try:
            for _ in range(60):
                socks.append(socket.create_connection(("127.0.0.1", self.port), timeout=10))
            time.sleep(0.3)           # all accepted: 48 threads, the rest turned away
            for s in socks:
                s.sendall(b"GET /api/ping HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n")
            answers = []
            for s in socks:
                data = b""
                while True:
                    chunk = s.recv(65536)
                    if not chunk:
                        break
                    data += chunk
                answers.append(data)
        finally:
            for s in socks:
                s.close()
        codes = [int(a.split(b" ", 2)[1]) for a in answers]
        self.assertEqual(set(codes), {200, 503}, codes)
        self.assertGreaterEqual(codes.count(503), 10, codes)
        self.assertGreaterEqual(codes.count(200), 40, codes)
        busy = next(a for a in answers if a.split(b" ", 2)[1] == b"503")
        self.assertIn(b"\r\nRetry-After: 2\r\n", busy)
        self.assertIn(b"too many connections", busy)
        # the threads go when the connections do, and the server takes new ones
        for _ in range(100):
            try:
                if self.api.get("/api/ping")[0] == 200:
                    break
            except Exception:
                pass
            time.sleep(0.05)
        self.assertEqual(self.api.get("/api/ping")[0], 200)

    def test_connection_cap_idle(self):
        # idle sockets past the cap are answered too, after a second
        socks = []
        try:
            for _ in range(56):
                socks.append(socket.create_connection(("127.0.0.1", self.port), timeout=10))
            time.sleep(1.5)
            busy = 0
            for s in socks[48:]:
                s.settimeout(0.5)
                try:
                    if s.recv(100).startswith(b"HTTP/1.1 503 "):
                        busy += 1
                except socket.timeout:
                    pass
            self.assertGreaterEqual(busy, 6)
        finally:
            for s in socks:
                s.close()
        for _ in range(100):
            try:
                if self.api.get("/api/ping")[0] == 200:
                    break
            except Exception:
                pass
            time.sleep(0.05)

    def test_discovery(self):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.addCleanup(s.close)
        s.settimeout(2)
        s.sendto(b"PS5AIO_DISCOVER", ("127.0.0.1", 8687))
        try:
            data, _ = s.recvfrom(1024)
        except socket.timeout:
            self.skipTest("discovery port busy (another instance?)")
        j = json.loads(data)
        self.assertEqual(j["name"], "NeoWEB")
        self.assertEqual(j["port"], 8686)

    # ------------------------------------------------------------ files
    def test_upload_download_resume(self):
        payload = os.urandom(2 * 1024 * 1024 + 5)
        st, body, _ = self.api.req("PUT", "/fs/put?path=/data/t/up.bin", raw=payload[:1000000])
        self.assertEqual(st, 200, body)
        st, body, _ = self.api.req("PUT", "/fs/put?path=/data/t/up.bin&offset=1000000",
                                   raw=payload[1000000:])
        self.assertEqual(json.loads(body)["written"], len(payload) - 1000000)
        st, body, _ = self.api.req("GET", "/fs/get?path=/data/t/up.bin")
        self.assertEqual(body, payload)
        st, body, hdr = self.api.req("GET", "/fs/get?path=/data/t/up.bin",
                                     headers={"Range": "bytes=100-199"})
        self.assertEqual(st, 206)
        self.assertEqual(body, payload[100:200])
        self.assertTrue(hdr["Content-Range"].startswith("bytes 100-199/"))

    def test_list_stat_mkdir_rename_delete(self):
        self.assertEqual(self.api.post("/api/fs/mkdir", {"path": "/data/a/b/c"})[0], 200)
        self.api.req("PUT", "/fs/put?path=/data/a/file.txt", raw=b"hello")
        _, j = self.api.get("/api/fs/list?path=/data/a")
        names = [(e["name"], e["type"]) for e in j["entries"]]
        self.assertEqual(names, [("b", "dir"), ("file.txt", "file")])
        _, j = self.api.get("/api/fs/stat?path=/data/a/file.txt")
        self.assertEqual(j["size"], 5)
        self.assertEqual(self.api.post("/api/fs/rename", {"from": "/data/a/file.txt",
                                                          "to": "/data/a/b/moved.txt"})[0], 200)
        _, j = self.api.get("/api/fs/walk?path=/data/a")
        paths = sorted(e["path"] for e in j["entries"])
        self.assertEqual(paths, ["b", "b/c", "b/moved.txt"])
        _, j = self.api.get("/api/fs/size?path=/data/a")
        self.assertEqual((j["bytes"], j["files"]), (5, 1))
        self.assertEqual(self.api.post("/api/fs/delete", {"path": "/data/a"})[0], 200)
        _, j = self.api.get("/api/fs/stat?path=/data/a")
        self.assertFalse(j["exists"])
        self.assertEqual(self.api.post("/api/fs/delete", {"path": "/user"})[0], 403)

    def test_text(self):
        st, _ = self.api.post("/api/fs/text", {"path": "/data/cfg.json", "content": "{\"á\":1}"})
        self.assertEqual(st, 200)
        _, j = self.api.get("/api/fs/text?path=/data/cfg.json")
        self.assertEqual(j["content"], "{\"á\":1}")

    def test_hash_and_space_check(self):
        import hashlib
        payload = os.urandom(300 * 1024 + 7)
        st, body, _ = self.api.req("PUT", "/fs/put?path=/data/t/h.bin&size=%d" % len(payload),
                                   raw=payload)
        self.assertEqual(st, 200, body)
        _, j = self.api.post("/api/fs/hash", {"path": "/data/t/h.bin"})
        job = self.api.wait_job(j["job"])
        self.assertEqual(job["state"], "done", job)
        self.assertEqual(job["result"], hashlib.sha256(payload).hexdigest())
        # an upload that cannot fit is refused before a byte is written
        st, body, _ = self.api.req("PUT", "/fs/put?path=/data/t/big.bin&size=%d" % (1 << 60),
                                   raw=b"x")
        self.assertEqual(st, 507, body)
        self.assertFalse(os.path.exists(f"{self.root}/data/t/big.bin"))
        # resuming past what is really on the console is refused
        st, body, _ = self.api.req("PUT", "/fs/put?path=/data/t/h.bin&offset=%d" % (len(payload) + 10),
                                   raw=b"x")
        self.assertEqual(st, 409, body)

    def test_playtime_and_status(self):
        _, j = self.api.get("/api/library/status")
        self.assertEqual(j["status"], {})
        st, body, _ = self.api.req("POST", "/api/library/status", body={"titleId": "PPSA01234", "status": "winning"})
        self.assertEqual(st, 400, body)
        st, body, _ = self.api.req("POST", "/api/library/status", body={"titleId": "../x", "status": "done"})
        self.assertEqual(st, 400, body)
        self.api.post("/api/library/status", {"titleId": "PPSA01234", "status": "playing"})
        self.api.post("/api/library/status", {"titleId": "CUSA00001", "status": "done"})
        self.api.post("/api/library/status", {"titleId": "CUSA00001", "status": ""})
        _, j = self.api.get("/api/library/status")
        self.assertEqual(j["status"], {"PPSA01234": "playing"})
        with open(f"{self.root}/data/neohen/web/library.json") as f:
            self.assertEqual(json.load(f)["status"], {"PPSA01234": "playing"})

        # a game open for a moment is counted, to the day
        self.api.post("/api/apps/kill", {})
        _, before = self.api.get("/api/stats/playtime")
        had = before["games"].get("PPSA01234", {}).get("sessions", 0)
        self.api.post("/api/apps/launch", {"titleId": "PPSA01234"})
        time.sleep(2.4)
        self.api.post("/api/apps/kill", {})
        time.sleep(0.5)
        _, j = self.api.get("/api/stats/playtime")
        g = j["games"]["PPSA01234"]
        self.assertEqual(g["sessions"], had + 1)
        self.assertGreaterEqual(g["total"], 1)
        self.assertLessEqual(g["total"], 60)
        self.assertEqual(sum(g["days"].values()), g["total"])
        self.assertIn(j["today"], g["days"])
        self.assertGreater(g["last"], 1700000000)
        with open(f"{self.root}/data/neohen/web/playtime.json") as f:
            self.assertIn("PPSA01234", json.load(f)["games"])

    def test_zz_autofix(self):
        # the Toolbox's one-step PlayGo fix: an image never seen is mounted
        # through ShadowMount+ for a moment, looked at, unmounted, then fixed
        r = self.root
        shutil.rmtree(f"{r}/data/neohen/web/fakelib/PPSA05555", ignore_errors=True)
        self.api.post("/api/games/unfix", {"id": "PPSA05555"})
        del self.smp_calls[:]
        st, j = self.api.post("/api/games/autofix", {"id": "PPSA05555"})
        self.assertEqual(st, 200, j)
        self.assertEqual([c[0] for c in self.smp_calls], ["/api/v1/games/mount", "/api/v1/games/unmount"])
        self.assertTrue(j["game"]["image"]["known"])
        self.assertTrue(j["game"]["backport"]["playgo"])
        self.assertFalse(os.path.exists(f"{r}/mnt/shadowmnt/PPSA05555_abcd"))
        # seen now: a second time does not mount again
        del self.smp_calls[:]
        st, j = self.api.post("/api/games/autofix", {"id": "PPSA05555"})
        self.assertEqual((st, self.smp_calls), (200, []))
        self.api.post("/api/games/unfix", {"id": "PPSA05555"})

    def test_usb_eject(self):
        r = self.root
        st, body, _ = self.api.req("POST", "/api/usb/eject", body={"drive": "sda1"})
        self.assertEqual(st, 400, body)
        st, body, _ = self.api.req("POST", "/api/usb/eject", body={"drive": "usb3"})
        self.assertEqual(st, 404, body)
        # an app is open: it is only closed when asked to
        self.api.post("/api/apps/launch", {"titleId": "PPSA01234"})
        st, body, _ = self.api.req("POST", "/api/usb/eject", body={"drive": "usb0"})
        self.assertEqual(st, 409, body)
        # something still holds the drive: nothing is forced
        open(f"{r}/mnt/usb0/.busy", "w").close()
        st, body, _ = self.api.req("POST", "/api/usb/eject", body={"drive": "usb0", "closeApp": True})
        self.assertEqual(st, 409, body)
        os.unlink(f"{r}/mnt/usb0/.busy")
        _, j = self.api.get("/api/apps/running")
        self.assertFalse(j["running"])
        st, j = self.api.post("/api/usb/eject", {"drive": "usb0"})
        self.assertEqual((st, j["drive"]), (200, "/mnt/usb0"))

    def test_remoteplay(self):
        r = f"{self.root}/data/neohen/web"
        for f in ("remoteplay.json", "rp_request.json"):
            if os.path.exists(f"{r}/{f}"):
                os.unlink(f"{r}/{f}")
        _, j = self.api.get("/api/remoteplay")
        self.assertFalse(j["known"])
        # the request is left for the Toolbox
        self.api.post("/api/remoteplay", {"op": "activate", "slot": 2})
        with open(f"{r}/rp_request.json") as f:
            self.assertEqual(json.load(f), {"op": "activate", "slot": 2})
        st, body, _ = self.api.req("POST", "/api/remoteplay", body={"op": "rm -rf"})
        self.assertEqual(st, 400, body)
        st, body, _ = self.api.req("POST", "/api/remoteplay", body={"op": "activate", "slot": 99})
        self.assertEqual(st, 400, body)
        # and what the Toolbox writes comes back as it is
        status = {"users": [{"slot": 1, "name": "NeoX", "activated": True, "accountId": "AAECAwQFBgc="}],
                  "pin": "12345678", "state": "waiting", "time": 1792100000}
        with open(f"{r}/remoteplay.json", "w") as f:
            json.dump(status, f)
        _, j = self.api.get("/api/remoteplay")
        self.assertEqual((j["known"], j["status"]), (True, status))

    def test_update_notice(self):
        off = f"{self.root}/data/neohen/update_check_off"
        _, j = self.api.get("/api/update/notice")
        self.assertTrue(j["enabled"])
        _, j = self.api.post("/api/update/notice", {"enabled": False})
        self.assertFalse(j["enabled"])
        self.assertTrue(os.path.exists(off))
        _, j = self.api.post("/api/update/notice", {"enabled": True})
        self.assertTrue(j["enabled"])
        self.assertFalse(os.path.exists(off))

    def test_launcher(self):
        # the NeoWEB icon in Media: a web shortcut (65536 + deeplinkUri) to this
        # page; NeoWEB puts it there when it starts
        _, j = self.api.get("/api/launcher")
        self.assertEqual((j["titleId"], j["installed"], j["uri"]),
                         ("NEOW00001", True, "http://127.0.0.1:8686/"))
        st, body, _ = self.api.req("POST", "/api/launcher/install", body={"uri": "ftp://x"})
        self.assertEqual(st, 400, body)
        self.api.post("/api/launcher/install", {"uri": "http://127.0.0.1:8686/x"})
        with open(f"{self.root}/user/app/NEOW00001/sce_sys/param.json") as f:
            p = json.load(f)
        self.assertEqual((p["applicationCategoryType"], p["titleId"], p["deeplinkUri"]),
                         (65536, "NEOW00001", "http://127.0.0.1:8686/x"))
        self.assertFalse(os.path.exists(f"{self.root}/system_ex/app/NEOW00001"))
        with open(f"{self.root}/user/app/NEOW00001/sce_sys/icon0.png", "rb") as f:
            self.assertEqual(f.read(8), b"\x89PNG\r\n\x1a\n")
        _, j = self.api.get("/api/launcher")
        self.assertEqual((j["installed"], j["uri"]), (True, "http://127.0.0.1:8686/x"))
        self.api.post("/api/launcher/remove", {})
        self.assertFalse(os.path.exists(f"{self.root}/user/app/NEOW00001"))
        self.assertTrue(os.path.exists(f"{self.root}/data/neohen/web/launcher_off"))
        self.api.post("/api/launcher/install", {"uri": "http://127.0.0.1:8686/"})
        self.assertFalse(os.path.exists(f"{self.root}/data/neohen/web/launcher_off"))

    def test_neohen_version(self):
        # NeoHEN writes its version at start; the page compares it with GitHub
        os.makedirs(f"{self.root}/data/neohen", exist_ok=True)
        with open(f"{self.root}/data/neohen/version.txt", "w") as f:
            f.write("0.1.0-rc6+2382\n")
        _, j = self.api.get("/api/sys/info")
        self.assertEqual(j["neohen"], "0.1.0-rc6+2382")

    def test_neohen_build_version(self):
        # the Makefile passes the repo's VERSION file in: the NeoHEN release
        # this NeoWEB was built with, next to NeoWEB's own version
        with open(os.path.join(os.path.dirname(__file__), "..", "..", "VERSION")) as f:
            want = f.read().strip()
        _, j = self.api.get("/api/sys/info")
        self.assertEqual(j["neohenBuild"], want)
        _, p = self.api.get("/api/ping")
        self.assertEqual(p["neohenBuild"], want)
        self.assertEqual(p["version"], j["version"])

    def test_zz_no_autosave(self):
        # the automatic save copy was taken out in 0.1.0-rc6: closing a game copies nothing
        base = f"{self.root}/mnt/usb0/NeoHEN/saves/users/NeoX_ESP (1d31cd9c)/PPSA01234"
        had = sorted(os.listdir(base)) if os.path.isdir(base) else []
        self.api.post("/api/apps/launch", {"titleId": "PPSA01234"})
        time.sleep(0.5)
        self.api.post("/api/apps/kill", {})
        time.sleep(1.3)
        self.assertEqual(sorted(os.listdir(base)) if os.path.isdir(base) else [], had)
        st, _, _ = self.api.req("GET", "/api/saves/auto")
        self.assertEqual(st, 404)

    def test_trophies(self):
        import sqlite3
        d = f"{self.root}/user/home/1d31cd9c/trophy_ui"
        os.makedirs(d, exist_ok=True)
        if os.path.exists(f"{d}/trophy_tracker.db"):
            os.unlink(f"{d}/trophy_tracker.db")
        db = sqlite3.connect(f"{d}/trophy_tracker.db")
        db.executescript("""
            create table meta(key text, val_text text, val_int integer);
            create table titles(titleid text primary key, platform_set integer, platinum_num integer,
              gold_num integer, silver_num integer, bronze_num integer, has_group integer, name text, icon_uri text);
            create table title_records(titleid text primary key, unlocked_platinum_num integer,
              unlocked_gold_num integer, unlocked_silver_num integer, unlocked_bronze_num integer,
              progress integer, time_last_update text);
            create table trophy_trackings(id integer primary key autoincrement, titleid text, trophyid integer, unlocked integer);
            create table id_bindings(nptitleid text, titleid text);
        """)
        long_icon = "https://example.invalid/" + "x" * 6000 + ".png"   # spills over to other pages
        db.execute("insert into titles values(?,?,?,?,?,?,?,?,?)",
                   ("20:NPWR00001_00", 8, 1, 4, 13, 59, 1, "Juego Uno ñ", long_icon))
        db.execute("insert into title_records values(?,?,?,?,?,?,?)",
                   ("20:NPWR00001_00", 1, 3, 10, 53, 83, "2022-01-29T10:31:40Z"))
        db.execute("insert into id_bindings values(?,?)", ("PPSA01234_00", "20:NPWR00001_00"))
        db.execute("insert into id_bindings values(?,?)", ("PPSA09999_00", "20:NPWR00001_00"))
        for i in range(2, 400):   # enough rows for the table to need inner pages
            db.execute("insert into titles values(?,?,?,?,?,?,?,?,?)",
                       ("10:NPWR%05d_00" % i, 4, 1, 2, 3, i, 0, "Set %d" % i, "https://example.invalid/%d.png" % i))
            if i % 2:
                db.execute("insert into title_records values(?,?,?,?,?,?,?)",
                           ("10:NPWR%05d_00" % i, 0, 1, 2, i - 1, i % 101, "2020-01-01T00:00:00Z"))
        db.commit()
        db.close()
        _, j = self.api.get("/api/trophies")
        me = [u for u in j["users"] if u["id"] == "1d31cd9c"][0]
        self.assertEqual(me["name"], "NeoX ESP")
        sets = {s["id"]: s for s in me["sets"] if not s["local"]}   # the menu's own count
        self.assertEqual(len(sets), 399)
        one = sets["NPWR00001_00"]
        self.assertEqual((one["platform"], one["name"], one["progress"], one["updated"]),
                         ("ps5", "Juego Uno ñ", 83, "2022-01-29T10:31:40Z"))
        self.assertEqual(one["icon"], long_icon[:319])
        self.assertEqual(one["total"], {"platinum": 1, "gold": 4, "silver": 13, "bronze": 59})
        self.assertEqual(one["got"], {"platinum": 1, "gold": 3, "silver": 10, "bronze": 53})
        self.assertEqual(one["games"], "PPSA01234 PPSA09999")
        s = sets["NPWR00399_00"]
        self.assertEqual((s["platform"], s["total"]["bronze"], s["got"]["bronze"], s["progress"]), ("ps4", 399, 398, 399 % 101))
        self.assertEqual(sets["NPWR00398_00"]["got"], {"platinum": 0, "gold": 0, "silver": 0, "bronze": 0})

    def test_report(self):
        import io
        import zipfile
        os.makedirs(f"{self.root}/data/neohen/themes", exist_ok=True)
        with open(f"{self.root}/data/neohen/NeoHEN.log", "w") as f:
            f.write("[1.0] [NeoHEN] info: hello\n")
        with open(f"{self.root}/data/neohen/themes/neotheme.log", "w") as f:
            f.write("1 menu seen\n")
        st, body, hdrs = self.api.req("GET", "/api/sys/report")
        self.assertEqual(st, 200)
        self.assertIn("NeoHEN-report-", hdrs["Content-Disposition"])
        z = zipfile.ZipFile(io.BytesIO(body))
        self.assertIsNone(z.testzip())
        names = z.namelist()
        for want in ("NeoHEN.log", "themes/neotheme.log", "console.json", "neoweb-log.json"):
            self.assertIn(want, names)
        self.assertEqual(z.read("NeoHEN.log"), b"[1.0] [NeoHEN] info: hello\n")
        info = json.loads(z.read("console.json"))
        self.assertIn("mounts", info)
        self.assertIn("procs", info)
        self.assertIsInstance(json.loads(z.read("neoweb-log.json")), list)

    def test_diagnostic_bundle(self):
        import hashlib
        import io
        import zipfile
        d = f"{self.root}/data/neohen"
        os.makedirs(f"{d}/payloads", exist_ok=True)
        elf = b"\x7fELF" + os.urandom(3000)
        files = {
            f"{d}/NeoHEN_crash.log": b"signal 11 at 0x1234\n",
            f"{d}/version.txt": b"0.1.0-rc15\n",
            f"{d}/config.ini": b"[meta]\nschema_version=1\n[x]\napi_token=abc123\nPassword = hunter2\n"
                               b"# token=in a comment stays\nwatchdog=relaunch\n",
            f"{d}/web/config.json": json.dumps({"token": "s3cr3t", "port": 8686, "nested": {"apiKey": "k"}}).encode(),
            f"{d}/payloads/diag-test.elf": elf,
            f"{d}/payloads/diag-test.elf.bundled": b"v1.2.3\n",
            f"{d}/payloads/diag-test.elf.auto_start": b"",
        }
        for path, body in files.items():
            with open(path, "wb") as f:
                f.write(body)
        try:
            st, body, hdrs = self.api.req("GET", "/api/sys/report")
            self.assertEqual(st, 200)
            z = zipfile.ZipFile(io.BytesIO(body))
            self.assertIsNone(z.testzip())
            names = z.namelist()
            for want in ("NeoHEN_crash.log", "version.txt", "config.ini", "neoweb-config.json",
                         "payloads.json", "console.json"):
                self.assertIn(want, names)
            # no secrets in it
            self.assertNotIn(b"abc123", body)
            self.assertNotIn(b"hunter2", body)
            self.assertNotIn(b"s3cr3t", body)
            ini = z.read("config.ini").decode()
            self.assertIn("api_token=(removed)\n", ini)
            self.assertIn("Password =(removed)\n", ini)
            self.assertIn("watchdog=relaunch\n", ini)
            cfg = json.loads(z.read("neoweb-config.json"))
            self.assertEqual((cfg["token"], cfg["port"], cfg["nested"]["apiKey"]), ("(removed)", 8686, "(removed)"))
            # every payload with its SHA-256 and version
            pay = {p["name"]: p for p in json.loads(z.read("payloads.json"))}
            one = pay["diag-test.elf"]
            self.assertEqual(one["sha256"], hashlib.sha256(elf).hexdigest())
            self.assertEqual((one["size"], one["bundled"], one["autoStart"]), (len(elf), "v1.2.3", True))
            info = json.loads(z.read("console.json"))
            self.assertIn("firmware", info)
            self.assertIn("active", info["safeMode"])
            self.assertIsInstance(info["conflicts"], list)
        finally:
            for path in files:
                if os.path.exists(path):
                    os.unlink(path)

    def test_pkg_and_eject(self):
        os.makedirs(f"{self.root}/data/pkg", exist_ok=True)
        with open(f"{self.root}/data/pkg/My Game.pkg", "wb") as f:
            f.write(b"\x7fCNT" + b"\0" * 64)
        st, j = self.api.post("/api/apps/install-pkg", {"url": "/data/pkg/My Game.pkg", "name": "My Game"})
        self.assertEqual(st, 200, j)
        self.assertTrue(j["contentId"])
        for bad, code in (("/data/pkg/nope.pkg", 404), ("/data/pkg/../pkg/My Game.pkg", 404),
                          ("/data/cfg.json", 404), ("ftp://x/y.pkg", 400), ("", 400)):
            st, body, _ = self.api.req("POST", "/api/apps/install-pkg", body={"url": bad})
            self.assertEqual(st, code, (bad, body))
        st, body, _ = self.api.req("POST", "/api/sys/eject", body={})
        self.assertEqual(st, 200, body)

    def test_search(self):
        os.makedirs(f"{self.root}/data/find/deep/Inner Folder", exist_ok=True)
        os.makedirs(f"{self.root}/mnt/sandbox/copy", exist_ok=True)
        for p in ("data/find/Save_GAME.bin", "data/find/deep/Inner Folder/gameplay.txt",
                  "data/find/deep/other.txt", "mnt/sandbox/copy/game.bin"):
            with open(f"{self.root}/{p}", "w") as f:
                f.write("x")
        _, j = self.api.get("/api/fs/search?path=/data/find&q=GaMe")
        self.assertEqual(sorted(e["path"] for e in j["entries"]),
                         ["/data/find/Save_GAME.bin", "/data/find/deep/Inner Folder/gameplay.txt"])
        self.assertFalse(j["truncated"])
        _, j = self.api.get("/api/fs/search?path=/data/find&q=inner")
        self.assertEqual([(e["path"], e["type"]) for e in j["entries"]],
                         [("/data/find/deep/Inner Folder", "dir")])
        _, j = self.api.get("/api/fs/search?path=/&q=game.bin")
        self.assertNotIn("/mnt/sandbox/copy/game.bin", [e["path"] for e in j["entries"]])
        _, j = self.api.get("/api/fs/search?path=/data/find&q=t&max=1")
        st, body, _ = self.api.req("GET", "/api/fs/search?path=/data/find&q=x")
        self.assertEqual(st, 400, body)
        _, j = self.api.get("/api/fs/search?path=/data/find&q=txt&max=1")
        self.assertEqual((len(j["entries"]), j["truncated"]), (1, True))

    def test_fan_and_power(self):
        cfg = f"{self.root}/data/neohen/config.ini"
        text = ("# NeoHEN\r\n[general]\r\nlanguage=system\r\n\r\n[cooling]\r\n# a comment\r\n"
                "fan_control=automatic\r\ntemperature_threshold_celsius=77\r\n\r\n[overlay]\r\nfan_control=keep\r\n")
        os.makedirs(os.path.dirname(cfg), exist_ok=True)
        with open(cfg, "w", newline="") as f:
            f.write(text)
        _, j = self.api.get("/api/sys/fan")
        self.assertEqual((j["manual"], j["threshold"], j["min"], j["max"]), (False, 77, 50, 80))
        st, body, _ = self.api.req("POST", "/api/sys/fan", body={"manual": True, "threshold": 95})
        self.assertEqual(st, 400, body)
        st, body, _ = self.api.req("POST", "/api/sys/fan", body={"manual": True, "threshold": 68})
        self.assertEqual(st, 200, body)
        with open(cfg, newline="") as f:
            self.assertEqual(f.read(), text.replace("fan_control=automatic", "fan_control=temperature_threshold")
                             .replace("celsius=77", "celsius=68"))
        _, j = self.api.get("/api/sys/fan")
        self.assertEqual((j["manual"], j["threshold"]), (True, 68))
        self.api.post("/api/sys/fan", {"manual": False})
        with open(cfg, newline="") as f:
            self.assertEqual(f.read(), text.replace("celsius=77", "celsius=68"))
        _, j = self.api.get("/api/sys/sensors")
        self.assertEqual(j["fanDuty"], 37)
        st, body, _ = self.api.req("POST", "/api/sys/power", body={"action": "explode"})
        self.assertEqual(st, 400, body)
        st, body, _ = self.api.req("POST", "/api/sys/power", body={"action": "rest"})
        self.assertEqual(st, 200, body)

    def test_captures(self):
        import io
        import zipfile
        _, j = self.api.get("/api/captures/list")
        items = sorted(j["items"], key=lambda x: x["id"])
        self.assertEqual([(x["id"], x["kind"], x["ext"], x["titleId"], x["title"], x["user"]) for x in items],
                         [("20250615_150127_00997366", "photo", "jxr", "PPSA01234", "My Game: II", "1d31cd9c"),
                          ("20261005_180320_00660961", "video", "webm", "PPSA01234", "My Game: II", "1d31cd9c")])
        self.assertEqual(j["users"], {"1d31cd9c": "NeoX ESP"})
        self.assertEqual(items[0]["size"], 3004)
        self.assertEqual(items[0]["thumb"], "/user/av_contents/thumbnails/photo/NPXS40087/PPSA01234/2fc/"
                                            "20250615_150127_00997366.jxr.jxr")
        self.assertEqual(items[1]["thumb"], "")
        st, body, hdrs = self.api.req("GET", "/jxr.js")
        self.assertEqual(st, 200)
        self.assertIn("javascript", hdrs["Content-Type"])
        self.assertIn(b"JxrModule", body[:200])
        st, body, _ = self.api.req("GET", "/api/captures/zip")
        self.assertEqual(st, 200)
        z = zipfile.ZipFile(io.BytesIO(body))
        self.assertIsNone(z.testzip())
        self.assertEqual(sorted(z.namelist()),
                         ["NeoX ESP/My Game_ II (PPSA01234)/20250615_150127_00997366.jxr",
                          "NeoX ESP/My Game_ II (PPSA01234)/20261005_180320_00660961.webm"])
        st, body, _ = self.api.req("GET", "/api/captures/zip?kind=video")
        self.assertEqual(len(zipfile.ZipFile(io.BytesIO(body)).namelist()), 1)
        st, body, _ = self.api.req("GET", "/api/captures/zip?ids=20250615_150127_00997366,nope")
        self.assertEqual(len(zipfile.ZipFile(io.BytesIO(body)).namelist()), 1)
        st, body, _ = self.api.req("GET", "/api/captures/zip?user=1d31cda3")
        self.assertEqual(st, 404, body)

    def test_saves(self):
        import io
        import zipfile
        _, j = self.api.get("/api/saves/list")
        users = {u["id"]: u for u in j["users"]}
        self.assertEqual(users["1d31cd9c"]["name"], "NeoX_ESP")
        self.assertEqual([(s["titleId"], s["kind"], s["files"]) for s in users["1d31cd9c"]["saves"]],
                         [("PPSA01234", "ps5", 2)])
        self.assertEqual(users["1d31cda3"]["saves"][0]["kind"], "ps4")
        self.assertEqual(sorted(p["id"] for p in j["places"]), ["console", "usb0"])

        # to the PC: a ZIP grouped by user
        st, body, _ = self.api.req("GET", "/api/saves/zip")
        self.assertEqual(st, 200)
        z = zipfile.ZipFile(io.BytesIO(body))
        self.assertIsNone(z.testzip())
        names = sorted(z.namelist())
        self.assertIn("NeoX_ESP (1d31cd9c)/PPSA01234/savedata_prospero/sdimg_SAVEDATA0", names)
        self.assertIn("NeoX_ESP (1d31cd9c)/PPSA01234/savedata_prospero_meta/s_icon0.png", names)
        self.assertIn("NeoX_ESP (1d31cd9c)/PPSA01234/info.json", names)
        self.assertIn("TaToR (1d31cda3)/CUSA00001/savedata/sdimg_SAVEDATA", names)
        with open(f"{self.save}/sdimg_SAVEDATA0", "rb") as f:
            original = f.read()
        self.assertEqual(z.read("NeoX_ESP (1d31cd9c)/PPSA01234/savedata_prospero/sdimg_SAVEDATA0"), original)
        # the ZIP comes back: it is unpacked into the console's copies
        st, _, _ = self.api.req("PUT", "/fs/put?path=/data/neohen/saves/incoming/a.zip", raw=body)
        self.assertEqual(st, 200)
        _, r = self.api.post("/api/saves/import", {"path": "/data/neohen/saves/incoming/a.zip"})
        job = self.api.wait_job(r["job"])
        self.assertEqual(job["state"], "done", job)
        self.assertFalse(os.path.exists(f"{self.root}/data/neohen/saves/incoming/a.zip"))
        _, b = self.api.get("/api/saves/backups")
        got = sorted((x["dest"], x["user"], x["titleId"], x["kind"]) for x in b["backups"])
        self.assertEqual(got, [("console", "1d31cd9c", "PPSA01234", "ps5"),
                               ("console", "1d31cda3", "CUSA00001", "ps4")])
        stamp = b["backups"][0]["stamp"]
        self.assertTrue(stamp.endswith("-pc"), stamp)
        base = f"{self.root}/data/neohen/saves/users/NeoX_ESP (1d31cd9c)/PPSA01234/{stamp}"
        with open(f"{base}/savedata_prospero/sdimg_SAVEDATA0", "rb") as f:
            self.assertEqual(f.read(), original)
        self.assertTrue(os.path.exists(f"{base}/savedata_prospero_meta/s_icon0.png"))
        # a damaged one is refused, and so is one that is not ours
        bad = bytearray(body)
        bad[len(bad) // 3] ^= 0xFF
        self.api.req("PUT", "/fs/put?path=/data/neohen/saves/incoming/b.zip", raw=bytes(bad))
        _, r = self.api.post("/api/saves/import", {"path": "/data/neohen/saves/incoming/b.zip"})
        self.assertEqual(self.api.wait_job(r["job"])["state"], "failed")
        evil = io.BytesIO()
        with zipfile.ZipFile(evil, "w") as zz:
            zz.writestr("../../etc/passwd", "x")
            zz.writestr("NeoX_ESP (1d31cd9c)/PPSA01234/elsewhere/x", "x")
        self.api.req("PUT", "/fs/put?path=/data/neohen/saves/incoming/c.zip", raw=evil.getvalue())
        _, r = self.api.post("/api/saves/import", {"path": "/data/neohen/saves/incoming/c.zip"})
        job = self.api.wait_job(r["job"])
        self.assertEqual(job["state"], "failed", job)
        self.assertFalse(os.path.exists(f"{self.root}/etc/passwd"))
        shutil.rmtree(f"{self.root}/data/neohen/saves/users")

        st, body, _ = self.api.req("GET", "/api/saves/zip?items=1d31cda3:ps4:CUSA00001")
        self.assertEqual(len(zipfile.ZipFile(io.BytesIO(body)).namelist()), 2)
        st, body, _ = self.api.req("GET", "/api/saves/zip?items=../x:ps4:CUSA00001")
        self.assertEqual(st, 400, body)

        # to the USB drive
        st, body, _ = self.api.req("POST", "/api/saves/backup", body={"dest": "usb3"})
        self.assertEqual(st, 404, body)
        _, j = self.api.post("/api/saves/backup", {"dest": "usb0", "user": "1d31cd9c"})
        job = self.api.wait_job(j["job"])
        self.assertEqual(job["state"], "done", job)
        kept = f"{self.root}/mnt/usb0/NeoHEN/saves/users/NeoX_ESP (1d31cd9c)/PPSA01234/{j['stamp']}"
        with open(f"{kept}/savedata_prospero/sdimg_SAVEDATA0", "rb") as f:
            self.assertEqual(f.read(), original)
        self.assertTrue(os.path.exists(f"{kept}/savedata_prospero_meta/s_icon0.png"))
        self.assertTrue(os.path.exists(f"{kept}/info.json"))
        self.assertFalse(os.path.exists(f"{self.root}/mnt/usb0/NeoHEN/saves/users/TaToR (1d31cda3)"))
        _, b = self.api.get("/api/saves/backups")
        self.assertEqual([(x["dest"], x["user"], x["titleId"], x["stamp"], x["kind"]) for x in b["backups"]],
                         [("usb0", "1d31cd9c", "PPSA01234", j["stamp"], "ps5")])

        # and back: the save that was there is kept first
        with open(f"{self.save}/sdimg_SAVEDATA0", "wb") as f:
            f.write(b"played on")
        _, r = self.api.post("/api/saves/restore", {"dest": "usb0", "folder": "NeoX_ESP (1d31cd9c)",
                                                    "titleId": "PPSA01234", "stamp": j["stamp"]})
        job = self.api.wait_job(r["job"])
        self.assertEqual(job["state"], "done", job)
        with open(f"{self.save}/sdimg_SAVEDATA0", "rb") as f:
            self.assertEqual(f.read(), original)
        _, b = self.api.get("/api/saves/backups")
        pre = [x["stamp"] for x in b["backups"] if x["stamp"].endswith("-pre")]
        self.assertEqual(len(pre), 1)
        with open(f"{os.path.dirname(kept)}/{pre[0]}/savedata_prospero/sdimg_SAVEDATA0", "rb") as f:
            self.assertEqual(f.read(), b"played on")
        st, body, _ = self.api.req("POST", "/api/saves/restore", body={
            "dest": "usb0", "folder": "../x (1d31cd9c)", "titleId": "PPSA01234", "stamp": j["stamp"]})
        self.assertEqual(st, 400, body)

    def test_menu_restart(self):
        import threading
        # nobody on NeoHEN's control port
        st, body, _ = self.api.req("POST", "/api/sys/menu", body={})
        self.assertEqual(st, 502, body)
        # a stand-in for NeoHEN: accepts the first request, refuses the second
        got = []
        srv = socket.socket()
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind(("127.0.0.1", self.ctl_port))
        srv.listen(2)

        def serve():
            for reply in (b"\0", b"\2"):
                c, _ = srv.accept()
                got.append(struct.unpack("<II", c.recv(8)))
                c.sendall(reply)
                c.close()

        t = threading.Thread(target=serve, daemon=True)
        t.start()
        st, body, _ = self.api.req("POST", "/api/sys/menu", body={})
        self.assertEqual(st, 200, body)
        st, body, _ = self.api.req("POST", "/api/sys/menu", body={})
        self.assertEqual(st, 409, body)
        t.join(5)
        srv.close()
        self.assertEqual(got, [(0x4F4E494F, 3)] * 2)

    def test_copy_job(self):
        _, j = self.api.post("/api/fs/copy", {"from": "/mnt/sandbox/pfsmnt/PPSA01234-app0",
                                              "to": "/data/copytest"})
        job = self.api.wait_job(j["job"])
        self.assertEqual(job["state"], "done", job)
        self.assertEqual(job["filesDone"], 2)
        self.assertEqual(job["bytesDone"], job["bytesTotal"])
        self.assertTrue(os.path.exists(f"{self.root}/data/copytest/data/big.bin"))

    def test_copy_into_itself(self):
        os.makedirs(f"{self.root}/data/selfcopy/sub", exist_ok=True)
        with open(f"{self.root}/data/selfcopy/f.txt", "w") as f:
            f.write("keep me")
        for src, dst in (("/data/selfcopy", "/data/selfcopy/sub/x"),
                         ("/data/selfcopy", "/data/selfcopy/"),
                         ("/data/selfcopy/f.txt", "/data//selfcopy/./f.txt"),
                         ("/", "/data/x")):
            for move in (False, True):
                st, j = self.api.post("/api/fs/copy", {"from": src, "to": dst, "move": move})
                self.assertEqual(st, 400, (src, dst, j))
        with open(f"{self.root}/data/selfcopy/f.txt") as f:
            self.assertEqual(f.read(), "keep me")
        # a name that only starts the same is another folder
        _, j = self.api.post("/api/fs/copy", {"from": "/data/selfcopy", "to": "/data/selfcopy2"})
        self.assertEqual(self.api.wait_job(j["job"])["state"], "done")

    def test_delete_protected_spellings(self):
        for p in ("/data/", "//data", "/data/.", "/./data//", "/.", "//", "/user/"):
            st, _ = self.api.post("/api/fs/delete", {"path": p})
            self.assertEqual(st, 403, p)
        self.assertTrue(os.path.isdir(f"{self.root}/data"))

    def test_path_too_long(self):
        st, j = self.api.get("/api/fs/stat?path=/data/" + "a" * 5000)
        self.assertEqual(st, 400, j)

    def test_cross_origin_refused(self):
        host = self.api.base.split("//", 1)[1]
        mk = json.dumps({"path": "/data/csrf"}).encode()
        # a page from elsewhere: refused, and nothing is done
        for hdrs in ({"Origin": "http://evil.example"}, {"Origin": "null"},
                     {"Origin": "http://" + host + ".evil.example"},
                     {"Host": "evil.example.com:8686"},
                     {"Host": "rebind.example.com", "Origin": "http://rebind.example.com"}):
            st, _, _ = self.api.req("POST", "/api/fs/mkdir", raw=mk,
                                    headers=dict(hdrs, **{"Content-Type": "text/plain"}))
            self.assertEqual(st, 403, hdrs)
        self.assertFalse(os.path.exists(f"{self.root}/data/csrf"))
        # the page served by NeoWEB itself, and local names, are fine
        for hdrs in ({"Origin": "http://" + host}, {"Host": "ps5.local:8686"},
                     {"Host": "ps5:8686", "Origin": "http://ps5:8686"}):
            st, _, _ = self.api.req("POST", "/api/fs/mkdir", raw=mk, headers=hdrs)
            self.assertEqual(st, 200, hdrs)
        self.assertTrue(os.path.isdir(f"{self.root}/data/csrf"))
        # an Origin header pushed past the headers that are kept is not lost:
        # too many headers is refused as a whole
        port = int(host.rsplit(":", 1)[1])
        with socket.create_connection(("127.0.0.1", port), timeout=5) as s:
            s.sendall(b"POST /api/fs/mkdir HTTP/1.1\r\nHost: " + host.encode() + b"\r\n" +
                      b"".join(b"X-%d: y\r\n" % i for i in range(70)) +
                      b"Origin: http://evil.example\r\nContent-Length: %d\r\n\r\n" % len(mk) + mk)
            self.assertIn(b" 400 ", s.recv(4096).split(b"\r\n")[0])

    def test_apps_store_install_checked(self):
        ok = {"action": "install", "url": "https://github.com/a/b/releases/download/v1/PPSA12345.zip",
              "id": "PPSA12345", "tag": "v1", "sha256": "ab" * 32}
        for bad in ({"url": "http://evil.example/x.zip"}, {"id": "../x"},
                    {"sha256": "abc"}, {"sha256": "zz" * 32}):
            st, _ = self.api.post("/api/apps/store", dict(ok, **bad))
            self.assertEqual(st, 400, bad)
        # no SHA-256: only when the user said yes to an unverified install
        self.assertEqual(self.api.post("/api/apps/store", dict(ok, sha256=""))[0], 400)
        self.assertEqual(self.api.post("/api/apps/store", {k: v for k, v in ok.items() if k != "sha256"})[0], 400)
        # NeoHEN's service is not there in the tests
        self.assertEqual(self.api.post("/api/apps/store", ok)[0], 502)
        self.assertEqual(self.api.post("/api/apps/store", dict(ok, sha256="", unverified=True))[0], 502)

    def test_apps_store_unverified_reaches_the_service(self):
        sock = f"{self.root}/system_tmp/neohen/ipc/util_service"
        seen = []
        srv = fake_util_service(sock, lambda cmd, ask: ('{"state":"download","id":"PPSA12345"}', 0), seen)
        try:
            base = {"action": "install", "url": "https://github.com/a/b/releases/download/v1/PPSA12345.zip",
                    "id": "PPSA12345", "tag": "v1", "size": 1234}
            st, j = self.api.post("/api/apps/store", dict(base, unverified=True))
            self.assertEqual(st, 200, j)
            self.assertEqual(j["job"]["state"], "download")
            cmd, ask = seen[-1]
            self.assertEqual(cmd, 0x800001B)
            self.assertEqual((ask["sha256"], ask["unverified"], ask["size"]), ("", True, 1234))
            # with a SHA-256 the flag never goes along
            st, _ = self.api.post("/api/apps/store", dict(base, sha256="cd" * 32, unverified=True))
            self.assertEqual(st, 200)
            self.assertEqual((seen[-1][1]["sha256"], seen[-1][1]["unverified"]), ("cd" * 32, False))
            # what the service says no to (e.g. no room) comes back as it is
            srv.close()
            srv = fake_util_service(sock, lambda cmd, ask: ("not enough free space: 5.2 GB needed, 1.0 GB free", -1), seen)
            st, j = self.api.post("/api/apps/store", dict(base, sha256="cd" * 32))
            self.assertEqual(st, 409)
            self.assertIn("not enough free space", j["error"])
        finally:
            srv.close()
            os.unlink(sock)

    def test_store_json_lock(self):
        """store.json has two writers (this and NeoHEN's util): NeoWEB takes
        store.json.lock, reads the file afresh and keeps what the other wrote."""
        sock = f"{self.root}/system_tmp/neohen/ipc/util_service"
        store = f"{self.root}/data/neohen/web/store.json"
        lock = store + ".lock"
        url = "https://github.com/a/b/releases/download/v1/x.elf"
        srv = fake_util_service(sock, lambda cmd, ask: ("4096", 0), [])
        try:
            with open(store, "w") as f:
                json.dump({"installed": {"y.elf": "v9"}, "sources": {"y.elf": url}}, f)
            # a lock left by a writer that died is taken over
            open(lock, "w").close()
            old = time.time() - 120
            os.utime(lock, (old, old))
            st, j = self.api.post("/api/store/fetch", {"url": url, "name": "x.elf", "tag": "v1"})
            self.assertEqual(st, 200, j)
            self.assertFalse(os.path.exists(lock))
            with open(store) as f:
                self.assertEqual(json.load(f)["installed"], {"x.elf": "v1", "y.elf": "v9"})
            # a live lock is waited for, and what its holder wrote is kept
            open(lock, "w").close()
            import threading
            def other_writer():
                time.sleep(0.6)
                with open(store) as f:
                    d = json.load(f)
                d["installed"]["z.elf"] = "v3"
                with open(store + ".util.tmp", "w") as f:
                    json.dump(d, f)
                os.rename(store + ".util.tmp", store)
                os.unlink(lock)
            t = threading.Thread(target=other_writer)
            t.start()
            st, j = self.api.post("/api/store/fetch", {"url": url, "name": "w.elf", "tag": "v2"})
            t.join()
            self.assertEqual(st, 200, j)
            with open(store) as f:
                self.assertEqual(json.load(f)["installed"],
                                 {"x.elf": "v1", "y.elf": "v9", "z.elf": "v3", "w.elf": "v2"})
            self.assertFalse(os.path.exists(lock))
        finally:
            srv.close()
            os.unlink(sock)
            for p in (store, lock):
                if os.path.exists(p):
                    os.unlink(p)

    def test_health(self):
        d = f"{self.root}/data/neohen"
        _, j = self.api.get("/api/health")
        self.assertEqual(j["safeMode"], {"active": False, "failedBoots": 0})
        self.assertNotIn("BackPork", j["conflicts"])
        # other homebrew beside NeoHEN: said, never stopped
        bindir = tempfile.mkdtemp(prefix="procs-")
        procs = []
        try:
            for name in ("ps5-backpork.elf", "kstuff.elf", "kstuff.elf"):
                exe = os.path.join(bindir, name)
                if not os.path.exists(exe):
                    shutil.copy(shutil.which("sleep"), exe)
                procs.append(subprocess.Popen([exe, "30"]))
            time.sleep(0.2)
            _, j = self.api.get("/api/health")
            self.assertIn("BackPork", j["conflicts"])
            self.assertIn("kstuff x2", j["conflicts"])
            for p in procs:
                self.assertIsNone(p.poll())
        finally:
            for p in procs:
                p.kill()
                p.wait()
            shutil.rmtree(bindir)
        # safe mode, as NeoHEN's bootstrapper leaves it, and turning it off
        with open(f"{d}/safe_mode", "w") as f:
            f.write("failed starts in a row: 3\n")
        with open(f"{d}/boot_attempts", "w") as f:
            f.write("4\n")
        _, j = self.api.get("/api/health")
        self.assertEqual(j["safeMode"], {"active": True, "failedBoots": 4})
        st, _ = self.api.post("/api/health/safemode", {})
        self.assertEqual(st, 200)
        self.assertFalse(os.path.exists(f"{d}/safe_mode"))
        self.assertFalse(os.path.exists(f"{d}/boot_attempts"))
        _, j = self.api.get("/api/health")
        self.assertFalse(j["safeMode"]["active"])

    # ------------------------------------------------------------ apps
    def test_apps_list(self):
        _, j = self.api.get("/api/apps/list")
        apps = {a["titleId"]: a for a in j["apps"]}
        self.assertEqual(apps["PPSA01234"]["format"], "ps5")
        self.assertEqual(apps["PPSA01234"]["contentId"], "UP0000-PPSA01234_00-GAMETEST00000000")
        self.assertTrue(apps["PPSA01234"]["hasIcon"])
        self.assertEqual(apps["PPSA01234"]["locations"][0]["location"], "internal")
        self.assertEqual(apps["CUSA00001"]["format"], "ps4")
        self.assertEqual(apps["CUSA00001"]["title"], "Old PS4 Game")
        self.assertEqual(apps["CUSA00001"]["version"], "01.00")

    def test_apps_param_icon(self):
        _, j = self.api.get("/api/apps/param?id=CUSA00001")
        self.assertEqual(j["format"], "sfo")
        self.assertEqual(j["param"]["CATEGORY"], "gd")
        _, j = self.api.get("/api/apps/param?id=PPSA01234")
        self.assertEqual(j["param"]["localizedParameters"]["es-ES"]["titleName"], "Juego de Prueba")
        st, body, hdr = self.api.req("GET", "/apps/icon?id=PPSA01234")
        self.assertEqual(st, 200)
        self.assertTrue(body.startswith(b"\x89PNG"))
        self.assertEqual(hdr["Content-Type"], "image/png")

    def test_rename(self):
        st, j = self.api.post("/api/apps/rename", {"titleId": "CUSA00001", "name": "Renamed PS4"})
        self.assertEqual(st, 200, j)
        _, j = self.api.get("/api/apps/list?id=CUSA00001")
        self.assertEqual(j["apps"][0]["title"], "Renamed PS4")
        st, j = self.api.post("/api/apps/rename", {"titleId": "PPSA01234", "name": "Nuevo Ñombre"})
        self.assertEqual(st, 200, j)
        self.assertEqual(len(j["updated"]), 2)
        _, j = self.api.get("/api/apps/param?id=PPSA01234")
        lp = j["param"]["localizedParameters"]
        self.assertEqual(lp["en-US"]["titleName"], "Nuevo Ñombre")
        self.assertEqual(lp["es-ES"]["titleName"], "Nuevo Ñombre")
        self.assertEqual(lp["defaultLanguage"], "en-US")
        self.assertEqual(j["param"]["applicationCategoryType"], 0)

    def test_launch_kill(self):
        self.assertEqual(self.api.post("/api/apps/launch", {"titleId": "PPSA01234"})[0], 200)
        _, j = self.api.get("/api/apps/running")
        self.assertEqual(j["titleId"], "PPSA01234")
        self.assertEqual(self.api.post("/api/apps/kill")[0], 200)
        _, j = self.api.get("/api/apps/running")
        self.assertFalse(j["running"])
        self.assertEqual(self.api.post("/api/apps/launch", {"titleId": "PPSA99999"})[0], 500)
        self.assertEqual(self.api.post("/api/apps/launch", {"titleId": "bad"})[0], 400)

    def test_install(self):
        st, j = self.api.post("/api/apps/install-pkg", {"url": "http://pc:9000/game.pkg"})
        self.assertEqual(st, 200)
        self.assertTrue(j["contentId"])
        st, _ = self.api.post("/api/apps/install-dir", {"titleId": "PPSA01234", "dir": "/user/app"})
        self.assertEqual(st, 200)

    # ------------------------------------------------------------ dump / backup
    def test_dump(self):
        _, j = self.api.get("/api/dump/sources?titleId=PPSA01234")
        self.assertTrue(j["mounted"])
        _, j = self.api.post("/api/dump/start", {"titleId": "PPSA01234", "dest": "/mnt/usb0/dumps"})
        job = self.api.wait_job(j["job"])
        self.assertEqual(job["state"], "done", job)
        self.assertTrue(os.path.exists(f"{self.root}/mnt/usb0/dumps/PPSA01234-app/data/big.bin"))
        self.assertTrue(os.path.exists(f"{self.root}/mnt/usb0/dumps/PPSA01234-meta/icon0.png"))
        _, j = self.api.post("/api/dump/start", {"titleId": "CUSA00001", "dest": "/mnt/usb0/dumps"})
        job = self.api.wait_job(j["job"])
        self.assertEqual(job["state"], "failed")
        self.assertIn("not mounted", job["error"])

    def test_backup(self):
        _, j = self.api.post("/api/backup/start", {"sources": ["/user/appmeta"],
                                                   "dest": "/mnt/usb0/bk", "name": "meta"})
        job = self.api.wait_job(j["job"])
        self.assertEqual(job["state"], "done", job)
        self.assertTrue(os.path.exists(f"{self.root}{j['dest']}/user/appmeta/PPSA01234/param.json"))

    # ------------------------------------------------------------ memory
    def test_memory_scan_write_freeze(self):
        # target: a buffer inside this python process
        buf = ctypes.create_string_buffer(64)
        val = ctypes.c_uint32.from_buffer(buf, 16)
        val.value = 0x5EED1234
        addr = ctypes.addressof(buf) + 16
        pid = os.getpid()

        _, j = self.api.get(f"/api/mem/maps?pid={pid}")
        self.assertTrue(any(int(m["start"], 16) <= addr < int(m["end"], 16) for m in j["maps"]))

        _, j = self.api.get(f"/api/mem/read?pid={pid}&addr={hex(addr)}&len=4")
        if "hex" not in j and ptrace_restricted():
            # e.g. GitHub's runners: the emulator may not read this process
            self.skipTest("the kernel forbids reading another process (yama ptrace_scope)")
        self.assertEqual(j["hex"], "3412ED5E")

        # first scan narrowed to the page around the value to keep the test fast
        lo, hi = addr & ~0xFFFF, (addr & ~0xFFFF) + 0x20000
        _, j = self.api.post("/api/mem/scan", {"pid": pid, "type": "u32", "op": "eq",
                                               "value": str(0x5EED1234), "first": True,
                                               "start": hex(lo), "end": hex(hi)})
        self.assertEqual(self.api.wait_job(j["job"])["state"], "done")
        _, r = self.api.get("/api/mem/results")
        self.assertIn(hex(addr), [x["addr"] for x in r["results"]])

        val.value = 0x5EED1235
        _, j = self.api.post("/api/mem/scan", {"op": "inc", "first": False})
        self.assertEqual(self.api.wait_job(j["job"])["state"], "done")
        _, r = self.api.get("/api/mem/results")
        self.assertEqual([x["addr"] for x in r["results"]], [hex(addr)])
        self.assertEqual(r["results"][0]["value"], str(0x5EED1235))

        st, _ = self.api.post("/api/mem/write", {"pid": pid, "addr": hex(addr),
                                                 "type": "u32", "value": "777"})
        self.assertEqual(st, 200)
        self.assertEqual(val.value, 777)

        _, j = self.api.post("/api/mem/freeze", {"pid": pid, "addr": hex(addr), "hex": "39 05 00 00",
                                                 "label": "vida"})
        fid = j["id"]
        val.value = 1
        time.sleep(0.4)
        self.assertEqual(val.value, 1337)
        _, j = self.api.get("/api/mem/freeze")
        self.assertEqual(j["entries"][0]["label"], "vida")
        self.api.post("/api/mem/unfreeze", {"id": fid})
        time.sleep(0.25)
        val.value = 5
        time.sleep(0.3)
        self.assertEqual(val.value, 5)
        self.assertEqual(self.api.post("/api/mem/reset")[0], 200)

    # ------------------------------------------------------------ trophies
    def test_trophies_local(self):
        _, j = self.api.get("/api/trophies?lang=es-ES")
        me = [u for u in j["users"] if u["id"] == "1d31cd9c"][0]
        s = [x for x in me["sets"] if x["id"] == "NPWR11111_00"][0]
        self.assertEqual(s["name"], "Juego de Prueba")
        self.assertTrue(s["local"] and s["localIcon"])
        self.assertEqual(s["total"], {"platinum": 1, "gold": 1, "silver": 0, "bronze": 1})
        self.assertEqual(s["got"], {"platinum": 0, "gold": 1, "silver": 0, "bronze": 1})
        self.assertEqual(s["progress"], 99)
        self.assertEqual(s["games"], "PPSA01234")
        self.assertEqual(s["updated"], time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(1791463600)))

        _, j = self.api.get("/api/trophies/set?user=1d31cd9c&id=NPWR11111_00&lang=es-ES")
        t = {x["id"]: x for x in j["trophies"]}
        self.assertEqual(len(t), 3)
        self.assertEqual((t["0001"]["grade"], t["0001"]["name"], t["0001"]["got"]), ("gold", "Oro", True))
        self.assertEqual(t["0001"]["when"], time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(1791460000)))
        self.assertTrue(t["0002"]["hidden"])
        self.assertFalse(t["0000"]["got"])
        self.assertEqual(t["0000"]["when"], "")
        # a user who has not played it has the set, with nothing earned
        _, j = self.api.get("/api/trophies/set?user=1d31cda3&id=NPWR11111_00")
        self.assertFalse(any(x["got"] for x in j["trophies"]))

        st, body, hdr = self.api.req("GET", "/api/trophies/icon?set=NPWR11111_00&id=0001")
        self.assertEqual((st, body, hdr["Content-Type"]), (200, b"\x89PNG\r\n\x1a\nGOLD", "image/png"))
        self.assertEqual(self.api.req("GET", "/api/trophies/icon?set=NPWR11111_00")[1], b"\x89PNG\r\n\x1a\nSET")
        self.assertEqual(self.api.req("GET", "/api/trophies/icon?set=NPWR11111_00&id=0009")[0], 404)
        self.assertEqual(self.api.req("GET", "/api/trophies/icon?set=../../etc&id=1")[0], 400)
        self.assertEqual(self.api.get("/api/trophies/set?user=1d31cd9c&id=NPWR22222_00")[0], 404)
        self.assertEqual(self.api.get("/api/trophies/set?user=nobody&id=NPWR11111_00")[0], 400)

    # ------------------------------------------------------------ games
    def game(self, tid):
        _, j = self.api.get(f"/api/games/images?id={tid}")
        return j["games"][0]

    def test_games_listing(self):
        _, j = self.api.get("/api/games/images")
        ids = {g["id"] for g in j["games"]}
        self.assertTrue({"PPSA01234", "PPSA05555"} <= ids)
        self.assertTrue(j["stub"])
        self.assertEqual({r["path"] for r in j["roots"]}, {"/data/homebrew", "/mnt/usb0"})
        g = self.game("PPSA05555")
        self.assertEqual((g["kind"], g["where"], g["size"]), ("image", "internal", 300 * 1024 + 5))
        self.assertEqual(g["playgo"], {"present": True, "chunks": 60, "scenarios": 2})
        self.assertFalse(g["backport"]["exists"])

    def test_games_fix(self):
        r = self.root
        # nobody has seen what this image brings: refused unless forced
        st, j = self.api.post("/api/games/fix", {"id": "PPSA05555"})
        self.assertEqual(st, 409)
        self.assertFalse(os.path.exists(f"{r}/data/homebrew/backports/PPSA05555"))
        st, j = self.api.post("/api/games/fix", {"id": "PPSA05555", "force": True})
        self.assertEqual(st, 200)
        self.assertTrue(j["game"]["backport"]["playgo"])
        self.assertEqual(j["game"]["backport"]["values"], "60/2")
        with open(f"{r}/data/homebrew/backports/PPSA05555/playgo_stub.dat") as f:
            self.assertEqual(f.read(), "60\n2\n")
        self.assertEqual(os.listdir(f"{r}/data/homebrew/backports/PPSA05555/fakelib"), ["libScePlayGo.sprx"])
        # the stub is kept where the next game will find it
        self.assertTrue(os.path.exists(f"{r}/data/neohen/fixes/libScePlayGo.sprx"))

        # the open game: its image's own library goes into the backport too
        self.assertEqual(self.api.post("/api/apps/launch", {"titleId": "PPSA01234"})[0], 200)
        try:
            st, j = self.api.post("/api/games/fix", {"id": "PPSA01234"})
            self.assertEqual(st, 200)
            self.assertEqual(sorted(j["game"]["backport"]["fakelib"]), ["libSceAmpr.sprx", "libScePlayGo.sprx"])
            self.assertEqual(j["game"]["image"], {"known": True, "fakelib": ["libSceAmpr.sprx"]})
            with open(f"{r}/data/homebrew/backports/PPSA01234/fakelib/libSceAmpr.sprx", "rb") as f:
                self.assertEqual(len(f.read()), 1003)
        finally:
            self.api.post("/api/apps/kill")
        # something of the user's in the backport survives taking the fix away
        with open(f"{r}/data/homebrew/backports/PPSA01234/eboot.bin", "wb") as f:
            f.write(b"mine")
        st, j = self.api.post("/api/games/unfix", {"id": "PPSA01234"})
        self.assertFalse(j["game"]["backport"]["playgo"])
        self.assertEqual(os.listdir(f"{r}/data/homebrew/backports/PPSA01234"), ["eboot.bin"])
        os.remove(f"{r}/data/homebrew/backports/PPSA01234/eboot.bin")
        os.rmdir(f"{r}/data/homebrew/backports/PPSA01234")
        st, j = self.api.post("/api/games/unfix", {"id": "PPSA05555"})
        self.assertFalse(os.path.exists(f"{r}/data/homebrew/backports/PPSA05555"))
        self.assertEqual(self.api.post("/api/games/fix", {"id": "CUSA00001"})[0], 404)

    def test_games_move(self):
        r = self.root
        src = f"{r}/data/homebrew/PPSA05555 Game.ffpfsc"
        dst = f"{r}/mnt/usb0/PPSA05555 Game.ffpfsc"
        with open(src, "rb") as f:
            want = f.read()
        os.makedirs(f"{r}/data/homebrew/backports/PPSA05555.off/fakelib")
        with open(f"{r}/data/homebrew/backports/PPSA05555.off/fakelib/a.sprx", "wb") as f:
            f.write(b"lib")
        self.assertEqual(self.api.post("/api/games/move", {"id": "PPSA05555", "to": "/data"})[0], 400)
        self.assertEqual(self.api.post("/api/games/move", {"id": "PPSA05555", "to": "/data/homebrew"})[0], 409)
        self.assertEqual(self.api.post("/api/games/note", {"id": "PPSA05555", "bad": "usb"})[0], 200)
        self.assertEqual(self.game("PPSA05555")["bad"], "usb")
        self.assertEqual(self.api.post("/api/games/note", {"id": "PPSA05555", "bad": "moon"})[0], 400)

        st, j = self.api.post("/api/games/move", {"id": "PPSA05555", "to": "/mnt/usb0"})
        self.assertEqual(st, 200)
        job = self.api.wait_job(j["job"])
        self.assertEqual((job["state"], job["result"]), ("done", "/mnt/usb0/PPSA05555 Game.ffpfsc"))
        self.assertFalse(os.path.exists(src))
        self.assertFalse(os.path.exists(dst + ".moving"))
        with open(dst, "rb") as f:
            self.assertEqual(f.read(), want)
        # its backport, switched off as it was, went along
        self.assertFalse(os.path.exists(f"{r}/data/homebrew/backports/PPSA05555.off"))
        with open(f"{r}/mnt/usb0/backports/PPSA05555.off/fakelib/a.sprx", "rb") as f:
            self.assertEqual(f.read(), b"lib")
        # ShadowMount+ would rewrite this when it sees the image in its new place
        with open(f"{r}/user/app/PPSA05555/mount_img.lnk", "w") as f:
            f.write("/mnt/usb0/PPSA05555 Game.ffpfsc\n")
        g = self.game("PPSA05555")
        self.assertEqual((g["where"], g["backport"]["exists"], g["backport"]["enabled"]), ("usb", True, False))
        # and back; a file already there is never written over
        open(src, "wb").close()
        self.assertEqual(self.api.post("/api/games/move", {"id": "PPSA05555", "to": "/data/homebrew"})[0], 409)
        os.remove(src)
        st, j = self.api.post("/api/games/move", {"id": "PPSA05555", "to": "/data/homebrew"})
        self.assertEqual(self.api.wait_job(j["job"])["state"], "done")
        with open(src, "rb") as f:
            self.assertEqual(f.read(), want)
        with open(f"{r}/user/app/PPSA05555/mount_img.lnk", "w") as f:
            f.write("/data/homebrew/PPSA05555 Game.ffpfsc\n")
        shutil.rmtree(f"{r}/data/homebrew/backports/PPSA05555.off")
        self.api.post("/api/games/note", {"id": "PPSA05555", "bad": ""})

    # ------------------------------------------------------------ cheats
    def test_cheats_live(self):
        r = self.root
        _, j = self.api.get("/api/cheats/live")
        self.assertFalse(j["running"])
        self.assertEqual(self.api.post("/api/apps/launch", {"titleId": "PPSA01234"})[0], 200)
        srv = None
        try:
            # no cheat file for the game: the service is not even asked
            _, j = self.api.get("/api/cheats/live")
            self.assertEqual((j["running"], j["titleId"], j["files"]), (True, "PPSA01234", 0))
            self.assertNotIn("list", j)
            os.makedirs(f"{r}/data/neohen/cheats", exist_ok=True)
            open(f"{r}/data/neohen/cheats/PPSA01234_01.000.000_abc.json", "w").close()
            # a file, but nobody listening
            _, j = self.api.get("/api/cheats/live")
            self.assertEqual(j["files"], 1)
            self.assertIn("not answering", j["error"])

            seen = []
            listing = {"local": f"{r}/user/data/neohen/PPSA01234_cheats_1",
                       "console": "/user/data/neohen/PPSA01234_cheats_1",
                       "json": {"mode": "runtime", "sessionId": "cheat-1-x", "canToggle": True, "name": "Test Game",
                                "authors": ["me"], "cheats": [], "groups": [{"format": "JSON", "sourceId": "abc",
                                "cheats": [{"key": "abc|/data/neohen/cheats/x.json|0", "name": "God mode",
                                            "enabled": False, "description": ""}]}]}}
            os.makedirs(f"{r}/user/data/neohen", exist_ok=True)
            srv = fake_cheat_service(f"{r}/system_tmp/neohen/ipc/util_service", listing, seen)
            _, j = self.api.get("/api/cheats/live")
            self.assertEqual(j["list"]["sessionId"], "cheat-1-x")
            self.assertEqual(j["list"]["groups"][0]["cheats"][0]["name"], "God mode")
            self.assertFalse(os.path.exists(listing["local"]))  # read and removed
            cmd, ask = seen[-1]
            self.assertEqual(cmd, 0x800000A)
            self.assertEqual((ask["tid"], ask["mode"]), ("PPSA01234", "runtime"))
            self.assertTrue(ask["pid"] > 0 and int(ask["generation"]) > 0)

            key = j["list"]["groups"][0]["cheats"][0]["key"]
            st, j = self.api.post("/api/cheats/toggle", {"session": "cheat-1-x", "key": key, "enabled": True})
            self.assertEqual((st, j["status"]), (200, "God mode -> enabled"))
            self.assertEqual(seen[-1], (0x800000B, {"session_id": "cheat-1-x", "cheat_key": key, "enabled": True}))
            st, j = self.api.post("/api/cheats/toggle", {"session": "old", "key": key, "enabled": True})
            self.assertEqual((st, j["error"]), (409, "stale"))
            self.assertEqual(self.api.post("/api/cheats/toggle", {"session": "cheat-1-x", "key": key})[0], 400)
        finally:
            self.api.post("/api/apps/kill")
            if srv:
                srv.close()
            shutil.rmtree(f"{r}/data/neohen/cheats", ignore_errors=True)

    def test_games_peek_and_smp(self):
        r = self.root
        # not mounted: nothing to look at
        self.assertEqual(self.api.post("/api/games/peek", {"id": "PPSA05555"})[0], 409)
        os.makedirs(f"{r}/mnt/shadowmnt/PPSA05555_abcd/sce_sys")
        os.makedirs(f"{r}/mnt/shadowmnt/PPSA05555_abcd/fakelib")
        with open(f"{r}/mnt/shadowmnt/PPSA05555_abcd/fakelib/libScePlayGo.sprx", "wb") as f:
            f.write(b"own stub")
        try:
            st, j = self.api.post("/api/games/peek", {"id": "PPSA05555"})
            self.assertEqual((st, j["game"]["image"]), (200, {"known": True, "fakelib": ["libScePlayGo.sprx"]}))
            # an image with a stub of its own is left alone
            self.assertEqual(self.api.post("/api/games/fix", {"id": "PPSA05555", "force": True})[0], 409)
        finally:
            shutil.rmtree(f"{r}/mnt/shadowmnt/PPSA05555_abcd")
            shutil.rmtree(f"{r}/data/neohen/web/fakelib/PPSA05555", ignore_errors=True)
        # its door takes only paths under /api/
        # the stand-in answers, and its answer comes back as it is
        st, j = self.api.post("/api/smp?path=/api/v1/version", {})
        self.assertEqual((st, j["status"]), (200, 0))
        self.assertEqual(self.api.get("/api/smp?path=/etc/passwd")[0], 400)
        self.assertEqual(self.api.get("/api/smp?path=/api/../x")[0], 400)

    def test_store(self):
        _, j = self.api.get("/api/store")
        self.assertEqual(j["installed"], {})
        ok = {"url": "https://github.com/a/b/releases/download/v1/x.elf", "name": "x.elf", "tag": "v1"}
        # only github.com, only a plain .elf or .bin name
        for bad in ({"url": "https://example.com/x.elf"}, {"url": "http://github.com/a/x.elf"},
                    {"name": "../x.elf"}, {"name": "x.sh"}, {"name": ".x.elf"}, {"name": "a/x.elf"}):
            self.assertEqual(self.api.post("/api/store/fetch", dict(ok, **bad))[0], 400)
        # NeoHEN's service is not there in the tests
        st, j = self.api.post("/api/store/fetch", ok)
        self.assertEqual(st, 502)
        self.assertEqual(self.api.get("/api/store")[1]["installed"], {})
        # what NeoHEN updated by itself comes back for the page
        with open(f"{self.root}/data/neohen/web/store.json", "w") as f:
            json.dump({"installed": {"x.elf": "v2"}, "sources": {"x.elf": ok["url"]},
                       "updates": [{"name": "x.elf", "from": "v1", "to": "v2", "time": 1792300000}]}, f)
        _, j = self.api.get("/api/store")
        self.assertEqual((j["installed"], j["updates"][0]["to"]), ({"x.elf": "v2"}, "v2"))
        os.unlink(f"{self.root}/data/neohen/web/store.json")
        # the automatic updates switch
        _, j = self.api.post("/api/store/auto", {"enabled": False})
        self.assertFalse(j["enabled"])
        self.assertTrue(os.path.exists(f"{self.root}/data/neohen/store_auto_off"))
        _, j = self.api.post("/api/store/auto", {"enabled": True})
        self.assertTrue(j["enabled"])

    def test_jobs_listing(self):
        _, j = self.api.get("/api/jobs")
        ids = [x["id"] for x in j["jobs"]]
        self.assertEqual(ids, sorted(ids, reverse=True))

    def test_extract(self):
        import io
        import tarfile
        import zipfile
        d = os.path.join(self.root, "data", "unz")
        os.makedirs(d, exist_ok=True)
        big = os.urandom(300000) * 4
        with zipfile.ZipFile(os.path.join(d, "a.zip"), "w", zipfile.ZIP_DEFLATED) as z:
            z.writestr("top.txt", "hola")
            z.writestr("sub/dir/big.bin", big)
            z.writestr("../evil.txt", "no")
        for name, mode in (("b.tar", "w"), ("c.tar.gz", "w:gz")):
            with tarfile.open(os.path.join(d, name), mode) as t:
                for n, data in (("x/" + "l" * 120 + ".txt", b"long"), ("x/big.bin", big),
                                ("/abs.txt", b"no")):
                    info = tarfile.TarInfo(n)
                    info.size = len(data)
                    t.addfile(info, io.BytesIO(data))
        for arc, check in (("a.zip", "sub/dir/big.bin"), ("b.tar", "x/big.bin"),
                           ("c.tar.gz", "x/big.bin")):
            out = "/data/unz/out-" + arc.split(".")[0]
            st, j = self.api.post("/api/fs/extract", {"path": "/data/unz/" + arc, "to": out})
            self.assertEqual(st, 200, j)
            job = self.api.wait_job(j["job"])
            self.assertEqual(job["state"], "done", job)
            local = os.path.join(self.root, out.lstrip("/"))
            with open(os.path.join(local, check), "rb") as f:
                self.assertEqual(f.read(), big)
            self.assertFalse(os.path.exists(os.path.join(self.root, "data", "unz", "evil.txt")))
            self.assertFalse(os.path.exists(os.path.join(local, "abs.txt")))
        self.assertTrue(os.path.exists(os.path.join(self.root, "data", "unz", "out-b",
                                                    "x", "l" * 120 + ".txt")))
        st, _ = self.api.post("/api/fs/extract", {"path": "/data/unz/a.rar", "to": "/data/x"})
        self.assertEqual(st, 400)

    def test_net(self):
        st, body, _ = self.api.req("GET", "/api/net/down?mb=2")
        self.assertEqual(st, 200)
        self.assertEqual(len(body), 2 * 1024 * 1024)
        st, body, _ = self.api.req("POST", "/api/net/up", raw=b"\0" * 3000000,
                                   headers={"Content-Type": "application/octet-stream"})
        self.assertEqual(st, 200)
        self.assertEqual(json.loads(body)["bytes"], 3000000)


if __name__ == "__main__":
    unittest.main(verbosity=2)
