#!/usr/bin/env python3
"""Tests for the default ("safe") NeoWEB build using the host emulator.

    make -C web host && python3 web/tests/test_safe.py

The safe build may read the console's state, launch or close a game, and
manage the payloads in NeoHEN's payloads folder by bare file name. The file
manager, installer, memory tools, dumper and power routes must not be
reachable, whatever the method.
"""
import json
import os
import shutil
import subprocess
import tempfile
import time
import unittest
import urllib.parse

from test_api import Api, free_port

HERE = os.path.dirname(os.path.abspath(__file__))
BIN = os.environ.get("NEOWEB_BIN") or os.path.join(HERE, "..", "build", "neoweb-host")

FORBIDDEN = [
    "/api/sys/kill", "/api/sys/power", "/api/sys/browser",
    "/api/jobs", "/api/jobs/cancel", "/api/jobs/clear",
    "/api/fs/list", "/api/fs/stat", "/api/fs/walk", "/api/fs/size",
    "/api/fs/text", "/api/fs/mkdir", "/api/fs/delete", "/api/fs/rename",
    "/api/fs/copy", "/api/fs/chmod", "/fs/get", "/fs/put",
    "/api/apps/rename", "/api/apps/install-dir", "/api/apps/install-pkg",
    "/api/apps/uninstall",
    "/api/mem/maps", "/api/mem/read", "/api/mem/write", "/api/mem/scan",
    "/api/mem/results", "/api/mem/reset", "/api/mem/freeze", "/api/mem/unfreeze",
    "/api/dump/sources", "/api/dump/start", "/api/backup/start",
    "/api/launcher", "/api/launcher/install", "/api/launcher/remove",
    "/api/usb/eject", "/api/games/autofix",
    "/api/remoteplay",
]


class SafeBuildTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.root = tempfile.mkdtemp(prefix="neoweb-safe-")
        r = cls.root
        os.makedirs(f"{r}/user/app/PPSA01234/sce_sys")
        os.makedirs(f"{r}/user/appmeta/PPSA01234")
        param = {
            "titleId": "PPSA01234",
            "contentVersion": "01.000.000",
            "localizedParameters": {
                "defaultLanguage": "en-US",
                "en-US": {"titleName": "Test Game"},
            },
        }
        with open(f"{r}/user/appmeta/PPSA01234/param.json", "w") as f:
            json.dump(param, f)
        with open(f"{r}/user/appmeta/PPSA01234/icon0.png", "wb") as f:
            f.write(b"\x89PNG\r\n\x1a\nFAKEICON")
        with open(f"{r}/secret.txt", "w") as f:
            f.write("do not serve me")

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

    def test_ping(self):
        st, j = self.api.get("/api/ping")
        self.assertEqual(st, 200)
        self.assertEqual(j["name"], "NeoWEB")

    def test_state_is_readable(self):
        st, j = self.api.get("/api/sys/info")
        self.assertEqual(st, 200)
        self.assertIn("firmware", j)
        st, _ = self.api.get("/api/sys/sensors")
        self.assertEqual(st, 200)
        st, j = self.api.get("/api/sys/mounts")
        self.assertEqual(st, 200)
        self.assertTrue(j["mounts"])

    def test_no_personal_data(self):
        _, j = self.api.get("/api/sys/info")
        for key in ("serial", "userName", "userId"):
            self.assertNotIn(key, j)

    def test_library_and_icon(self):
        _, j = self.api.get("/api/apps/list")
        titles = {a["titleId"]: a for a in j["apps"]}
        self.assertEqual(titles["PPSA01234"]["title"], "Test Game")
        st, body, _ = self.api.req("GET", "/apps/icon?id=PPSA01234")
        self.assertEqual(st, 200)
        self.assertTrue(body.startswith(b"\x89PNG"))

    def test_launch_and_close(self):
        st, _ = self.api.post("/api/apps/launch", {"titleId": "PPSA01234"})
        self.assertEqual(st, 200)
        st, j = self.api.get("/api/apps/running")
        self.assertEqual(st, 200)
        self.assertEqual(j["titleId"], "PPSA01234")
        st, _ = self.api.post("/api/apps/kill", {})
        self.assertEqual(st, 200)

    def test_everything_else_is_missing(self):
        for path in FORBIDDEN:
            for method in ("GET", "POST", "PUT", "DELETE"):
                st, _, _ = self.api.req(method, path + "?path=/secret.txt", raw=b"{}")
                self.assertEqual(st, 404, f"{method} {path} answered {st}")

    # ------------------------------------------------------------ payloads
    ELF = bytes([0x7F]) + b"ELF" + bytes(60)

    def pay(self):
        _, j = self.api.get("/api/payloads/list")
        return {p["name"]: p for p in j["payloads"]}

    def upload(self, name, data=None):
        st, body, _ = self.api.req("PUT", "/api/payloads/upload?name=" + urllib.parse.quote(name, safe=""),
                                   raw=self.ELF if data is None else data)
        return st, json.loads(body)

    def test_payload_lifecycle(self):
        name = "demo-payload_v1.2.elf"   # longer than the kernel's 15 chars
        st, j = self.upload(name)
        self.assertEqual(st, 200, j)
        self.assertEqual(j["size"], len(self.ELF))
        p = self.pay()[name]
        self.assertFalse(p["running"])
        self.assertFalse(p["autoStart"])
        self.assertFalse(p["bundled"])

        st, j = self.api.post("/api/payloads/autostart", {"name": name, "enabled": True})
        self.assertEqual(st, 200)
        self.assertTrue(os.path.exists(f"{self.root}/data/neohen/payloads/{name}.auto_start"))
        self.assertTrue(self.pay()[name]["autoStart"])
        self.api.post("/api/payloads/autostart", {"name": name, "enabled": False})
        self.assertFalse(self.pay()[name]["autoStart"])

        st, j = self.api.post("/api/payloads/start", {"name": name})
        self.assertEqual(st, 200, j)
        self.assertTrue(j["running"])
        p = self.pay()[name]
        self.assertTrue(p["running"])
        self.assertEqual(p["pid"], j["pid"])

        st, _ = self.api.post("/api/payloads/delete", {"name": name})
        self.assertEqual(st, 409)             # running: stop it first
        st, j = self.api.post("/api/payloads/stop", {"name": name})
        self.assertEqual(st, 200, j)
        for _ in range(40):
            if not self.pay()[name]["running"]:
                break
            time.sleep(0.05)
        self.assertFalse(self.pay()[name]["running"])
        st, _ = self.api.post("/api/payloads/stop", {"name": name})
        self.assertEqual(st, 409)             # not running any more

        st, _ = self.api.post("/api/payloads/delete", {"name": name})
        self.assertEqual(st, 200)
        self.assertNotIn(name, self.pay())

    def test_payload_names_cannot_escape_the_folder(self):
        for bad in ("../secret.elf", "a/b.elf", ".hidden.elf",
                    "noext", "script.sh", "x" * 80 + ".elf", "sp ace.elf", ""):
            st, _ = self.upload(bad)
            self.assertEqual(st, 400, bad)
            for route in ("start", "stop", "delete", "autostart"):
                st, _ = self.api.post("/api/payloads/" + route, {"name": bad})
                self.assertEqual(st, 400, f"{route} {bad!r}")
        self.assertFalse(os.path.exists(f"{self.root}/data/neohen/secret.elf"))

    def test_payload_must_be_an_elf(self):
        st, _ = self.upload("fake.elf", b"MZ not an elf at all")
        self.assertEqual(st, 400)
        self.assertNotIn("fake.elf", self.pay())
        st, _ = self.upload("raw.bin", b"anything goes in a .bin")
        self.assertEqual(st, 200)
        self.api.post("/api/payloads/delete", {"name": "raw.bin"})

    def test_upload_takes_ownership_from_the_bundle(self):
        d = f"{self.root}/data/neohen/payloads"
        os.makedirs(d, exist_ok=True)
        with open(f"{d}/owned.elf", "wb") as f:
            f.write(self.ELF)
        with open(f"{d}/owned.elf.bundled", "w") as f:
            f.write("0123456789abcdef")
        self.assertTrue(self.pay()["owned.elf"]["bundled"])
        self.upload("owned.elf")
        self.assertFalse(self.pay()["owned.elf"]["bundled"])
        # deleting a bundled payload keeps its stamp, so it is not reinstalled
        with open(f"{d}/owned.elf.bundled", "w") as f:
            f.write("0123456789abcdef")
        self.api.post("/api/payloads/delete", {"name": "owned.elf"})
        self.assertFalse(os.path.exists(f"{d}/owned.elf"))
        self.assertTrue(os.path.exists(f"{d}/owned.elf.bundled"))
        os.unlink(f"{d}/owned.elf.bundled")

    def test_kstuff_and_the_service_itself_cannot_be_stopped(self):
        st, _ = self.api.post("/api/payloads/stop", {"name": "kstuff.elf"})
        self.assertEqual(st, 400)
        self.upload("neoweb.elf")             # named like this very process
        st, _ = self.api.post("/api/payloads/stop", {"name": "neoweb.elf"})
        self.assertEqual(st, 400)
        self.assertIsNone(self.proc.poll())
        self.assertTrue(self.pay()["neoweb.elf"]["self"])
        st, _ = self.api.post("/api/payloads/delete", {"name": "neoweb.elf"})
        self.assertEqual(st, 409)             # it is running: this is it
        os.unlink(f"{self.root}/data/neohen/payloads/neoweb.elf")

    def test_read_only_switch(self):
        flag = f"{self.root}/data/neohen/web/readonly"
        self.upload("ro.elf")
        open(flag, "w").close()
        try:
            self.assertTrue(self.api.get("/api/payloads/list")[1]["readOnly"])
            self.assertEqual(self.upload("blocked.elf")[0], 403)
            for route in ("start", "stop", "delete", "autostart"):
                st, _ = self.api.post("/api/payloads/" + route, {"name": "ro.elf"})
                self.assertEqual(st, 403, route)
            self.assertIn("ro.elf", self.pay())
        finally:
            os.unlink(flag)
        self.api.post("/api/payloads/delete", {"name": "ro.elf"})

    def test_secret_file_stays_on_disk(self):
        self.api.req("POST", "/api/fs/delete", body={"path": "/secret.txt"})
        self.assertTrue(os.path.exists(f"{self.root}/secret.txt"))


if __name__ == "__main__":
    unittest.main()
