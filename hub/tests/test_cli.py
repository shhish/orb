"""The CLI with no hub running, and `orb-hub serve` as a real process."""

import os
import resource
import signal
import stat
import subprocess
import sys
import time

from orb_hub import cli, control
from orb_hub.config import Config
from orb_hub.store import Store


def test_commands_need_the_hub(capsys):
    for argv in (["pair", "123456"], ["identify", "orb-020000abcdef"], ["config", "orb-020000abcdef", "--name", "x"]):
        assert cli.main(argv) == 1
        assert "orb-hub isn't running" in capsys.readouterr().err


def test_devices_and_unpair_work_offline(capsys):
    cfg = Config.load()
    cfg.data_dir.mkdir(parents=True)
    store = Store(cfg.db_path)
    store.pair("orb-020000abcdef", "Orb", "waveshare-s3-amoled-1.75c", "0.2.0", "ab" * 32, "dev")
    store.close()

    assert cli.main(["devices"]) == 0
    out = capsys.readouterr().out
    assert "isn't running" in out and "orb-020000abcdef" in out and "never seen" in out
    assert cli.main(["unpair", "orb-020000abcdef"]) == 0
    assert cli.main(["unpair", "orb-020000abcdef"]) == 1
    capsys.readouterr()
    assert cli.main(["devices"]) == 0
    assert "No paired devices." in capsys.readouterr().out


def test_serve_process(short_tmp):
    env = dict(os.environ)
    proc = subprocess.Popen(
        [
            sys.executable,
            "-m",
            "orb_hub",
            "serve",
            "--bind",
            "127.0.0.1",
            "--port",
            "0",
            "--pair-port",
            "0",
            "--no-mdns",
            "--log-file",
            str(short_tmp / "logs" / "hub.log"),
        ],
        env=env,
        stderr=subprocess.PIPE,
        text=True,
    )
    sock = Config.load().control_socket
    try:
        deadline = time.monotonic() + 20
        while not sock.exists():
            assert proc.poll() is None, proc.stderr.read()
            assert time.monotonic() < deadline, "hub didn't start"
            time.sleep(0.05)
        assert stat.S_IMODE(os.stat(sock).st_mode) == 0o600
        assert stat.S_IMODE(os.stat(sock.parent).st_mode) == 0o700
        assert control.call(sock, "devices") == {"devices": [], "pending": []}
        # A second hub refuses to start rather than stealing the socket.
        second = subprocess.run(
            [
                sys.executable,
                "-m",
                "orb_hub",
                "serve",
                "--bind",
                "127.0.0.1",
                "--port",
                "0",
                "--pair-port",
                "0",
                "--no-mdns",
            ],
            env=env,
            capture_output=True,
            text=True,
            check=False,
            timeout=30,
        )
        assert second.returncode == 1 and "already running" in second.stderr
    finally:
        proc.send_signal(signal.SIGTERM)
        _, err = proc.communicate(timeout=20)
    assert proc.returncode == 0, err
    log_text = (short_tmp / "logs" / "hub.log").read_text()
    # Logging went to the file, not stderr.
    assert "link and firmware on" in log_text and "stopping" in log_text and "stopping" not in err
    assert not sock.exists()


def test_serve_raises_the_open_file_limit():
    # launchd and Terminal start processes with 256, which a few hundred idle connections would use up.
    code = (
        "import resource\n"
        "from orb_hub.cli import raise_fd_limit\n"
        "resource.setrlimit(resource.RLIMIT_NOFILE, (256, resource.getrlimit(resource.RLIMIT_NOFILE)[1]))\n"
        "raise_fd_limit()\n"
        "print(resource.getrlimit(resource.RLIMIT_NOFILE)[0])\n"
    )
    out = subprocess.run([sys.executable, "-c", code], capture_output=True, text=True, check=True, timeout=30)
    hard = resource.getrlimit(resource.RLIMIT_NOFILE)[1]
    assert int(out.stdout) == (cli.FD_LIMIT if hard == resource.RLIM_INFINITY else min(cli.FD_LIMIT, hard))
