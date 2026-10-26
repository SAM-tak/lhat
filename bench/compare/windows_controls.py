"""Identical per-process scheduling plus diagnostic CPU counters, Windows only."""
import ctypes
from ctypes import wintypes
import subprocess

last_info = {}

def configure(affinity_mask=None):
    global last_info
    k = ctypes.WinDLL('kernel32', use_last_error=True)
    k.GetCurrentProcess.restype = wintypes.HANDLE
    k.SetProcessAffinityMask.argtypes = [wintypes.HANDLE, ctypes.c_size_t]
    k.SetProcessAffinityMask.restype = wintypes.BOOL
    if affinity_mask is not None and not k.SetProcessAffinityMask(k.GetCurrentProcess(), affinity_mask):
        raise ctypes.WinError(ctypes.get_last_error())
    class PowerState(ctypes.Structure):
        _fields_ = [('Version', wintypes.ULONG), ('ControlMask', wintypes.ULONG), ('StateMask', wintypes.ULONG)]
    k.SetProcessInformation.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p, wintypes.DWORD]
    k.SetProcessInformation.restype = wintypes.BOOL
    k.GetProcessTimes.argtypes = [wintypes.HANDLE] + [ctypes.POINTER(wintypes.FILETIME)] * 4
    k.GetProcessTimes.restype = wintypes.BOOL
    k.QueryProcessCycleTime.argtypes = [wintypes.HANDLE, ctypes.POINTER(ctypes.c_ulonglong)]
    k.QueryProcessCycleTime.restype = wintypes.BOOL
    k.GetPriorityClass.argtypes = [wintypes.HANDLE]
    k.GetPriorityClass.restype = wintypes.DWORD
    k.GetProcessAffinityMask.argtypes = [wintypes.HANDLE, ctypes.POINTER(ctypes.c_size_t), ctypes.POINTER(ctypes.c_size_t)]
    k.GetProcessAffinityMask.restype = wintypes.BOOL
    original_popen = subprocess.Popen
    class ControlledPopen(original_popen):
        def __init__(self, args, *positional, **kwargs):
            kwargs['creationflags'] = kwargs.get('creationflags', 0) | subprocess.ABOVE_NORMAL_PRIORITY_CLASS
            super().__init__(args, *positional, **kwargs)
            handle = wintypes.HANDLE(int(self._handle))
            # SDK processthreadsapi.h: ProcessPowerThrottling == 4;
            # Version 1, execution-speed ControlMask 1, StateMask 0 opts out.
            state = PowerState(1, 1, 0)
            self._bench_info = {'power_throttling_opt_out': bool(k.SetProcessInformation(handle, 4, ctypes.byref(state), ctypes.sizeof(state))),
                                'priority_class': int(k.GetPriorityClass(handle))}
            process_mask, system_mask = ctypes.c_size_t(), ctypes.c_size_t()
            if k.GetProcessAffinityMask(handle, ctypes.byref(process_mask), ctypes.byref(system_mask)):
                self._bench_info['process_affinity_mask'] = hex(process_mask.value)
        def __exit__(self, *args):
            global last_info
            status = super().__exit__(*args)
            handle = wintypes.HANDLE(int(self._handle))
            created, exited, kernel, user = (wintypes.FILETIME() for _ in range(4))
            def ticks(ft):
                return (ft.dwHighDateTime << 32) | ft.dwLowDateTime
            info = dict(self._bench_info)
            if k.GetProcessTimes(handle, ctypes.byref(created), ctypes.byref(exited), ctypes.byref(kernel), ctypes.byref(user)):
                info['process_cpu_ns'] = (ticks(kernel) + ticks(user)) * 100
            cycles = ctypes.c_ulonglong()
            if k.QueryProcessCycleTime(handle, ctypes.byref(cycles)):
                info['process_cycles'] = cycles.value
            last_info = info
            return status
    subprocess.Popen = ControlledPopen
    import run
    original_invoke = run.invoke
    def invoke(*args, **kwargs):
        result = original_invoke(*args, **kwargs)
        result.update(last_info)
        return result
    run.invoke = invoke
