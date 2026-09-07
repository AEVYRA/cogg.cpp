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
        command = [cli, "run-model", db, "s", model, "--once", "--timeout-ms", "60000",
                   "--checkpoint-dir", folder]
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
        assert resumed["checkpoint"]["write"] == "saved"
        history = json.loads(run("inspect", db, "s"))
        assert len(history["attempts"]) == 2
        assert history["attempts"][1]["body"]["prior_unsettled"]
        assert sum(bool(e["consumed"]) for e in history["occasions"]) == 1
        run("verify", db, "s")
        run("send", db, "s", "hello-1", "Say hello briefly, then wait for my next message.")
        subsequent = json.loads(run(*command[1:]))
        assert subsequent["tick"] == 2
        assert subsequent["checkpoint"]["read"] == "restored"
        assert subsequent["inference"]["reused_tokens"] > 0
        checkpoint = next(pathlib.Path(folder).glob("*.coggkv"))
        data = bytearray(checkpoint.read_bytes())
        data[-1] ^= 1
        checkpoint.write_bytes(data)
        run("send", db, "s", "corrupt-recovery", "Say hello briefly.")
        recovered = json.loads(run(*command[1:]))
        assert recovered["tick"] == 3 and recovered["checkpoint"]["read"] == "rejected"
        assert recovered["checkpoint"]["write"] == "saved"
        checkpoint.unlink()
        run("send", db, "s", "missing-recovery", "Say hello briefly.")
        cold = json.loads(run(*command[1:]))
        assert cold["tick"] == 4 and cold["checkpoint"]["read"] == "missing"
        run("send", db, "s", "changed-context", "Say hello briefly.")
        changed = json.loads(run(*command[1:], "--ctx", "2048"))
        assert changed["tick"] == 5 and changed["checkpoint"]["read"] == "rejected"
        assert "compatibility" in changed["checkpoint"]["detail"]
        assert changed["checkpoint"]["write"] == "saved"
        run("verify", db, "s")
        print(json.dumps({"result": "PASS", "killed_tick": 0, "recovered_tick": 1,
                          "restored_checkpoint_tick": 2, "corrupt_checkpoint_tick": 3,
                          "deleted_checkpoint_tick": 4, "changed_context_tick": 5,
                          "proposal": changed["proposal"]}))


if __name__ == "__main__":
    main()
