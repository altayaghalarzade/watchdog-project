# POSIX Watchdog

A C11 supervisor that starts and monitors two worker processes on Linux. It records process events in a timestamped log, restarts workers after unexpected exits, and shuts down its children when it receives a termination signal.

## Requirements

- Linux with a C11 compiler and Make
- Python 3.9 or later and `ps` for the automated tests

On Ubuntu or Debian, install the dependencies with:

```sh
sudo apt install build-essential python3 procps
```

## Build and run

Run the following from the project directory. Make builds both `watchdog` and `watchdog-worker`. The supervisor looks for `./watchdog-worker` in its current directory.

```sh
make
./watchdog
```

The program writes to `watchdog.log` by default. To request a status report, open a second terminal in the project directory while the watchdog is running:

```sh
watchdog_pid=$(sed -n 's/.* WATCHDOG pid=//p' watchdog.log | tail -n 1)
kill -USR1 "$watchdog_pid"
tail -n 8 -f watchdog.log
```

`SIGUSR1` adds a `STATUS` entry for each worker with its PID, state, and restart count. In the second terminal, press **Ctrl+C** to stop following the log. To shut down the watchdog, press **Ctrl+C** in the first terminal or run `kill -TERM "$watchdog_pid"` in the second terminal.

## Worker modes

```text
./watchdog [LOG [MODE1 [MODE2]]]
```

The modes apply to workers `alpha` and `beta`, respectively. Both default to `idle`.

| Mode | What the worker does |
| --- | --- |
| `idle` | Waits for signals. |
| `normal` | Exits with code `0`. |
| `error` | Exits with code `1`. |
| `stubborn` | Waits for signals and ignores `SIGTERM`. |

To see the restart limit, run the following. Worker `alpha` fails after each launch. The log records three `RESTART` entries followed by `LIMIT`. Worker `beta` exits normally.

```sh
./watchdog restart-demo.log error normal
tail -n 12 restart-demo.log
```

## How it works

- The supervisor starts both workers with `fork()` and `exec()`. When a child exits, it handles `SIGCHLD` and collects the exit status with `waitpid()`.
- The signal handlers set flags. The main loop reads the flags, handles process events, and logs a periodic check every two seconds while running.
- On `SIGINT` or `SIGTERM`, the supervisor stops restarting workers, sends them `SIGTERM`, and collects their exit statuses. After a three-second grace period, it sends `SIGKILL` to any remaining worker.
- If a worker cannot start, the supervisor logs an error and exits with code `1` after shutdown.

See the [sample log](docs/sample.log) for timestamped startup, status, and shutdown records.

## Tests

```sh
make test
```

The suite checks worker startup, normal and unexpected exits, the restart limit, status reporting, shutdown via `SIGTERM` and Ctrl+C, timeout escalation, and timestamped logs. It also refreshes [docs/sample.log](docs/sample.log). See the [test scenarios](docs/TESTS.md) and [recorded results](docs/test-results.txt).

Run `make clean` to remove the compiled executables.

## Project layout

| Path | Contents |
| --- | --- |
| `src/` and `include/` | Main loop, signal handling, process management, and logging |
| `workers/` | Worker program and its test modes |
| `tests/` | Shell entry point and Python test suite |
| `docs/` | [Project report](docs/REPORT.pdf), test documentation, and sample log |
