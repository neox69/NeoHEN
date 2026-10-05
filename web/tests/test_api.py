#!/usr/bin/env python3
"""End-to-end tests for the full NeoHEN Web API using the host emulator.

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

        cls.port = free_port()
        env = dict(os.environ, PS5AIO_ROOT=cls.root)
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
        self.assertEqual(j["name"], "NeoHEN Web")
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
        self.assertIn(b"PS5AIO", body)
        self.assertIn("text/html", hdr["Content-Type"])

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
        self.assertEqual(j["name"], "NeoHEN Web")
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

    def test_copy_job(self):
        _, j = self.api.post("/api/fs/copy", {"from": "/mnt/sandbox/pfsmnt/PPSA01234-app0",
                                              "to": "/data/copytest"})
        job = self.api.wait_job(j["job"])
        self.assertEqual(job["state"], "done", job)
        self.assertEqual(job["filesDone"], 2)
        self.assertEqual(job["bytesDone"], job["bytesTotal"])
        self.assertTrue(os.path.exists(f"{self.root}/data/copytest/data/big.bin"))

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

    def test_jobs_listing(self):
        _, j = self.api.get("/api/jobs")
        ids = [x["id"] for x in j["jobs"]]
        self.assertEqual(ids, sorted(ids, reverse=True))


if __name__ == "__main__":
    unittest.main(verbosity=2)
