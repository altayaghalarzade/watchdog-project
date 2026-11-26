from contextlib import contextmanager
from pathlib import Path
import os
import pty
import re
import signal
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]


def require(condition, message):
    if not condition:
        raise AssertionError(message)


class Run:
    def __init__(self, process, log):
        self.process = process
        self.log = log

    def read(self):
        return self.log.read_text() if self.log.exists() else ""

    def expect(self, pattern):
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            match = re.search(pattern, self.read(), re.MULTILINE)
            if match:
                return match
            if self.process.poll() is not None:
                break
            time.sleep(0.02)
        raise AssertionError(f"Missing {pattern!r}\n{self.read()}")

    def worker_pid(self, name):
        self.expect(rf"START [^\n]*worker={name}\b")
        line = next(line for line in self.read().splitlines()
                    if "START " in line and f"worker={name} " in line + " ")
        pid = int(re.search(r"\bpid=(\d+)", line)[1])
        return pid

    def finish(self):
        require(self.process.wait(timeout=8) == 0, self.read())
        require("SHUTDOWN complete" in self.read(), self.read())


@contextmanager
def launch(directory, name, *modes):
    log = directory / f"{name}.log"
    process = subprocess.Popen(
        ["./watchdog", str(log), *modes], cwd=ROOT, start_new_session=True
    )
    try:
        yield Run(process, log)
    finally:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=6)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait(timeout=2)


def verify_gone(pid):
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return
    raise AssertionError(f"Worker PID {pid} still exists")


def ctrl_c(directory):
    log = directory / "interrupt.log"
    pid, terminal = pty.fork()
    if pid == 0:
        os.chdir(ROOT)
        os.execl("./watchdog", "watchdog", str(log))

    try:
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            if log.exists() and log.read_text().count("START ") >= 2:
                break
            time.sleep(0.02)
        else:
            raise AssertionError("PTY workers did not start")
        os.write(terminal, b"\x03")
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            finished, status = os.waitpid(pid, os.WNOHANG)
            if finished:
                pid = None
                require(os.waitstatus_to_exitcode(status) == 0, log.read_text())
                require("SHUTDOWN begin" in log.read_text() and
                        "SHUTDOWN complete" in log.read_text(), log.read_text())
                return
            time.sleep(0.02)
        raise AssertionError("Ctrl+C did not shut down the watchdog")
    finally:
        os.close(terminal)
        if pid is not None:
            os.killpg(pid, signal.SIGKILL)
            os.waitpid(pid, 0)


def main():
    with tempfile.TemporaryDirectory() as temporary:
        directory = Path(temporary)
        with launch(directory, "normal") as run:
            alpha, beta = (run.worker_pid(name) for name in ("alpha", "beta"))
            run.expect(r"CHECK active=2")
            run.process.send_signal(signal.SIGUSR1)
            for name, pid in (("alpha", alpha), ("beta", beta)):
                run.expect(rf"STATUS worker={name} pid={pid} state=running")
            run.process.terminate()
            run.finish()
            for pid in (alpha, beta):
                verify_gone(pid)
            require("SHUTDOWN escalate" not in run.read(), run.read())
            print("PASS two workers, periodic check, SIGUSR1, graceful SIGTERM")

        with launch(directory, "completed", "normal", "normal") as run:
            run.finish()
            require(len(re.findall(r"EXIT [^\n]*result=normal", run.read())) == 2 and
                    "RESTART " not in run.read(), run.read())
            print("PASS natural exit without restart")

        with launch(directory, "killed") as run:
            alpha = run.worker_pid("alpha")
            run.expect(r"CHECK active=2")
            info = subprocess.check_output(["ps", "-o", "ppid=,comm=", "-p", str(alpha)], text=True)
            require(info.split() == [str(run.process.pid), "watchdog-worker"], info)
            os.kill(alpha, signal.SIGKILL)
            run.expect(r"RESTART [^\n]*worker=alpha\b")
            run.process.terminate()
            run.finish()
            print("PASS verified worker SIGKILL and restart")

        with launch(directory, "limited", "error", "normal") as run:
            run.finish()
            require(run.read().count("RESTART ") == 3 and
                    run.read().count("LIMIT ") == 1, run.read())
            print("PASS restart limit of three")

        with launch(directory, "timeout", "stubborn", "idle") as run:
            alpha = run.worker_pid("alpha")
            run.expect(r"CHECK active=2")
            start = time.monotonic()
            run.process.terminate()
            run.finish()
            require("SHUTDOWN escalate" in run.read() and
                    time.monotonic() - start >= 2.8, run.read())
            verify_gone(alpha)
            print("PASS timeout, SIGKILL escalation and reap")

        ctrl_c(directory)
        print("PASS terminal Ctrl+C")

        sample = (directory / "normal.log").read_text()
        require(all(re.match(r"\[?\d{4}-\d{2}-\d{2}", line)
                    for line in sample.splitlines()), sample)
        (ROOT / "docs" / "sample.log").write_text(sample)
        print("PASS timestamps and sample log")


if __name__ == "__main__":
    main()
