#!/usr/bin/env python3
"""Tests for the default ("safe") NeoHEN Web build using the host emulator.

    make -C web host && python3 web/tests/test_safe.py

The safe build may read the console's state and launch or close a game.
Nothing that writes, deletes, installs, reads memory or powers the console
off may be reachable, whatever the method.
"""
import json
import os
import shutil
import subprocess
import tempfile
import time
import unittest

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
        self.assertEqual(j["name"], "NeoHEN Web")

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

    def test_secret_file_stays_on_disk(self):
        self.api.req("POST", "/api/fs/delete", body={"path": "/secret.txt"})
        self.assertTrue(os.path.exists(f"{self.root}/secret.txt"))


if __name__ == "__main__":
    unittest.main()
