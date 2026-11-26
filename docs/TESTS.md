# Test scenarios

Run `make test` from the project directory on Linux. The tests use temporary logs and signal only processes they spawn. The external SIGKILL case checks the worker executable name and parent PID before sending the signal.

| Scenario | Expected result |
| --- | --- |
| Normal operation | Two worker PIDs and a periodic check with two active workers |
| Normal termination | Exit 0 recorded for each worker and no restarts |
| Unexpected termination | The externally killed worker is restarted |
| Restart limit | Exactly three restart attempts and one limit event |
| Status reporting | SIGUSR1 logs both running workers and their PIDs |
| SIGTERM shutdown | Both children are reaped and escalation is absent |
| Timeout | The SIGTERM-ignoring worker is killed and reaped after the grace period |
| Ctrl+C | A real terminal interrupt initiates and completes shutdown |
| Log verification | The sample shows time-stamped start, status, check, exit and shutdown events |
