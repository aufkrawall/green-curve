# SPDX-FileCopyrightText: Copyright (c) 2026 aufkrawall
# SPDX-License-Identifier: MIT
"""Stop and start the Green Curve Windows service from tooling.

The release simulation's teardown edits a value the running service owns: the
service loads `[updates] highest_seen_version` once at start and writes its
in-memory copy back on every settings save, including a failed check.  A file
edit made while it runs is therefore undone at its next save.  The only edit
that sticks is one made while the service is stopped, so the restore is a
stop -> write -> start transaction (simulate_release_guards.py).

The wait follows the same documented SCM protocol as the product
(source/service_scm_wait_policy.h): a pending state is alive while dwCheckPoint
advances within dwWaitHint, settling in a different state ends the wait at
once, and one wait never exceeds 60 s.  A checkpoint change raises no
notification, so the status is sampled at an interval derived from the hint,
exactly as the product's waiter does.  tests: simulate_release_tests.py.
"""

import ctypes
import os
import time

# installer_common.h GC_SETUP_SERVICE_NAME (asserted by the tests).
SERVICE_NAME = "GreenCurveService"

# The SERVICE_* dwCurrentState values.
STATE_STOPPED = 1
STATE_START_PENDING = 2
STATE_STOP_PENDING = 3
STATE_RUNNING = 4
STATE_CONTINUE_PENDING = 5
STATE_PAUSE_PENDING = 6
STATE_PAUSED = 7
_PENDING = (STATE_START_PENDING, STATE_STOP_PENDING, STATE_CONTINUE_PENDING,
            STATE_PAUSE_PENDING)
STATE_NAMES = {STATE_STOPPED: "stopped", STATE_START_PENDING: "start-pending",
               STATE_STOP_PENDING: "stop-pending", STATE_RUNNING: "running",
               STATE_CONTINUE_PENDING: "continue-pending",
               STATE_PAUSE_PENDING: "pause-pending", STATE_PAUSED: "paused"}

# service_scm_wait_policy.h, value for value (asserted by the tests).
NO_HINT_STALL_MS = 30000
MIN_STALL_MS = 2000
MAX_TOTAL_MS = 60000

WAIT_CONTINUE = "continue"
WAIT_REACHED = "reached"
WAIT_SETTLED_ELSEWHERE = "settled-elsewhere"
WAIT_STALLED = "stalled"
WAIT_OVERALL_TIMEOUT = "overall-timeout"


def state_name(state):
    return STATE_NAMES.get(state, f"state-{state}")


def stall_budget_ms(wait_hint_ms):
    if wait_hint_ms == 0:
        return NO_HINT_STALL_MS
    return min(max(wait_hint_ms, MIN_STALL_MS), MAX_TOTAL_MS)


def poll_interval_ms(wait_hint_ms):
    """A tenth of the hint, clamped so a short hint does not spin."""
    return min(max(wait_hint_ms // 10, 250), 500)


class WaitTracker:
    """gc_scm_wait_step(): progress is ANY change of state or checkpoint."""

    def __init__(self, now_ms):
        self.start_ms = now_ms
        self.last_progress_ms = now_ms
        self.last = None

    def step(self, now_ms, state, check_point, wait_hint_ms, desired):
        if state == desired:
            return WAIT_REACHED
        if state not in _PENDING:
            return WAIT_SETTLED_ELSEWHERE
        if self.last != (state, check_point):
            self.last = (state, check_point)
            self.last_progress_ms = now_ms
        if now_ms - self.start_ms >= MAX_TOTAL_MS:
            return WAIT_OVERALL_TIMEOUT
        if now_ms - self.last_progress_ms > stall_budget_ms(wait_hint_ms):
            return WAIT_STALLED
        return WAIT_CONTINUE


class ServiceStatus(ctypes.Structure):
    _fields_ = [(name, ctypes.c_uint32) for name in (
        "service_type", "current_state", "controls_accepted", "win32_exit_code",
        "service_specific_exit_code", "check_point", "wait_hint")]


class ServiceStatusProcess(ctypes.Structure):
    _fields_ = ServiceStatus._fields_ + [("process_id", ctypes.c_uint32),
                                         ("service_flags", ctypes.c_uint32)]


_SC_MANAGER_CONNECT = 0x0001
_SERVICE_QUERY_STATUS = 0x0004
_SERVICE_START = 0x0010
_SERVICE_STOP = 0x0020
_SERVICE_CONTROL_STOP = 1
_SC_STATUS_PROCESS_INFO = 0
_ERROR_SERVICE_ALREADY_RUNNING = 1056
_ERROR_SERVICE_DOES_NOT_EXIST = 1060
_ERROR_SERVICE_NOT_ACTIVE = 1062


def _advapi():
    api = ctypes.WinDLL("advapi32", use_last_error=True)
    api.OpenSCManagerW.argtypes = [ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_uint32]
    api.OpenSCManagerW.restype = ctypes.c_void_p
    api.OpenServiceW.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p, ctypes.c_uint32]
    api.OpenServiceW.restype = ctypes.c_void_p
    api.CloseServiceHandle.argtypes = [ctypes.c_void_p]
    api.CloseServiceHandle.restype = ctypes.c_int
    api.QueryServiceStatusEx.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p,
                                         ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint32)]
    api.QueryServiceStatusEx.restype = ctypes.c_int
    api.ControlService.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                   ctypes.POINTER(ServiceStatus)]
    api.ControlService.restype = ctypes.c_int
    api.StartServiceW.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_void_p]
    api.StartServiceW.restype = ctypes.c_int
    return api


class WindowsServiceController:
    """One named service through the SCM.  Stop/start need an elevated token."""

    def __init__(self, name=SERVICE_NAME, clock=time.monotonic, sleep=time.sleep):
        self.name = name
        self._clock = clock
        self._sleep = sleep

    def _open(self, access):
        """(api, scm, service) or (api, scm, None) when not installed."""
        api = _advapi()
        scm = api.OpenSCManagerW(None, None, _SC_MANAGER_CONNECT)
        if not scm:
            raise ctypes.WinError(ctypes.get_last_error())
        service = api.OpenServiceW(scm, self.name, access)
        if not service:
            error = ctypes.get_last_error()
            if error == _ERROR_SERVICE_DOES_NOT_EXIST:
                return api, scm, None
            api.CloseServiceHandle(scm)
            raise ctypes.WinError(error)
        return api, scm, service

    @staticmethod
    def _query(api, service):
        status = ServiceStatusProcess()
        needed = ctypes.c_uint32()
        if not api.QueryServiceStatusEx(service, _SC_STATUS_PROCESS_INFO,
                                        ctypes.byref(status), ctypes.sizeof(status),
                                        ctypes.byref(needed)):
            raise ctypes.WinError(ctypes.get_last_error())
        return status

    def _wait(self, api, service, desired):
        tracker = WaitTracker(int(self._clock() * 1000))
        while True:
            status = self._query(api, service)
            verdict = tracker.step(int(self._clock() * 1000), status.current_state,
                                   status.check_point, status.wait_hint, desired)
            if verdict != WAIT_CONTINUE:
                return verdict, status
            self._sleep(poll_interval_ms(status.wait_hint) / 1000.0)

    def query_state(self):
        """The current state, or None when the service is not installed."""
        api, scm, service = self._open(_SERVICE_QUERY_STATUS)
        try:
            return None if service is None else self._query(api, service).current_state
        finally:
            if service:
                api.CloseServiceHandle(service)
            api.CloseServiceHandle(scm)

    def _transition(self, access, request, desired):
        api, scm, service = self._open(_SERVICE_QUERY_STATUS | access)
        if service is None:
            api.CloseServiceHandle(scm)
            raise RuntimeError(f"service {self.name} is not installed")
        try:
            request(api, service, self._query(api, service).current_state)
            verdict, status = self._wait(api, service, desired)
            if verdict != WAIT_REACHED:
                raise RuntimeError(
                    f"service {self.name} did not reach {state_name(desired)} ({verdict}, "
                    f"state {state_name(status.current_state)}, win32 exit "
                    f"{status.win32_exit_code}, service exit "
                    f"{status.service_specific_exit_code})")
        finally:
            api.CloseServiceHandle(service)
            api.CloseServiceHandle(scm)

    def stop(self):
        def request(api, service, state):
            # Already stopping or stopped: a second stop control is refused
            # (ERROR_SERVICE_CANNOT_ACCEPT_CTRL); the wait alone is enough.
            if state in (STATE_STOPPED, STATE_STOP_PENDING):
                return
            status = ServiceStatus()
            if not api.ControlService(service, _SERVICE_CONTROL_STOP, ctypes.byref(status)):
                error = ctypes.get_last_error()
                if error != _ERROR_SERVICE_NOT_ACTIVE:
                    raise ctypes.WinError(error)
        self._transition(_SERVICE_STOP, request, STATE_STOPPED)

    def start(self):
        def request(api, service, state):
            if state in (STATE_RUNNING, STATE_START_PENDING):
                return
            if not api.StartServiceW(service, 0, None):
                error = ctypes.get_last_error()
                if error != _ERROR_SERVICE_ALREADY_RUNNING:
                    raise ctypes.WinError(error)
        self._transition(_SERVICE_START, request, STATE_RUNNING)


class NoServiceController:
    """Hosts without an SCM: there is no service to hold the value."""

    name = SERVICE_NAME

    def query_state(self):
        return None

    def stop(self):
        raise RuntimeError("no service control on this host")

    def start(self):
        raise RuntimeError("no service control on this host")


def default_controller():
    return WindowsServiceController() if os.name == "nt" else NoServiceController()
