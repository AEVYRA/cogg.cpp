#!/usr/bin/env python3
"""Optional real-process recovery probe: CLI path, GGUF path. No services needed."""
import json
import pathlib
import sqlite3
import subprocess
import sys
import tempfile
import time


def main():
    cli, model = map(str, map(pathlib.Path.resolve, map(pathlib.Path, sys.argv[1:3])))
    with tempfile.TemporaryDirectory(prefix="cogg-model-crash-") as folder:
        db = str(pathlib.Path(folder) / "subject.db")

        def run(*args):
            result = subprocess.run([cli, *args], capture_output=True, text=True, timeout=120)
            if result.returncode:
                raise RuntimeError(result.stderr[-2000:])
            return result.stdout

        run("init", db, "s", "1", "100", "3600000")
        command = [cli, "run-model", db, "s", model, "--once", "--timeout-ms", "60000"]
        with open(pathlib.Path(folder) / "child.log", "w") as log:
            child = subprocess.Popen(command, stdout=log, stderr=log)
            try:
                deadline = time.monotonic() + 60
                while time.monotonic() < deadline:
                    with sqlite3.connect(db) as connection:
                        reserved = connection.execute("SELECT count(*) FROM attempts WHERE status='reserved'").fetchone()[0]
                    if reserved:
                        child.kill()  # SIGKILL with an admitted real inference, before commit.
                        child.wait(timeout=10)
                        break
                    if child.poll() is not None:
                        raise RuntimeError("child exited before admitted inference could be killed")
                    time.sleep(0.005)
                else:
                    raise RuntimeError("child never admitted inference")
            finally:
                if child.poll() is None:
                    child.kill()
                    child.wait()
        before = json.loads(run("inspect", db, "s"))
        assert before["tick"] == 0, "killed computation committed unexpectedly"
        assert before["attempts"][0]["status"] == "reserved"
        run("verify", db, "s")
        resumed = json.loads(run(*command[1:]))
        assert resumed["tick"] == 1
        history = json.loads(run("inspect", db, "s"))
        assert len(history["attempts"]) == 2
        assert history["attempts"][1]["body"]["prior_unsettled"]
        assert sum(bool(e["consumed"]) for e in history["occasions"]) == 1
        run("verify", db, "s")
        run("send", db, "s", "hello-1", "Say hello briefly, then wait for my next message.")
        subsequent = json.loads(run(*command[1:]))
        assert subsequent["tick"] == 2
        run("verify", db, "s")
        print(json.dumps({"result": "PASS", "killed_tick": 0, "recovered_tick": 1,
                          "next_process_tick": 2, "proposal": subsequent["proposal"]}))


if __name__ == "__main__":
    main()
