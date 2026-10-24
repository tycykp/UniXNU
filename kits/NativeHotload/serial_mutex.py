"""Cross-process ownership of one physical debug device (serial AND USB).

Acquire before opening a port and hold until the handle closes. Cooperating
transports must use the same key. POSIX lock files are never unlinked.
"""
import hashlib
import os
from pathlib import Path
import tempfile
import time

DEFAULT_DEVICE_KEY = 'unixnu-giulia-debug-device'


class DeviceMutex:
    def __init__(self, key=DEFAULT_DEVICE_KEY, timeout=0):
        if not key or not 0 <= timeout <= 3600:
            raise ValueError('Device key and lock timeout (0..3600) required')
        self.key, self.timeout, self.handle = key, timeout, None

    def __enter__(self):
        if self.handle is not None:
            raise RuntimeError('DeviceMutex instance already acquired')
        digest = hashlib.sha256(self.key.encode('utf-8')).hexdigest()[:32]
        if os.name == 'nt':
            import ctypes
            from ctypes import wintypes
            api = ctypes.WinDLL('kernel32', use_last_error=True)
            api.CreateMutexW.argtypes = (ctypes.c_void_p, wintypes.BOOL, wintypes.LPCWSTR)
            api.CreateMutexW.restype = wintypes.HANDLE
            api.WaitForSingleObject.argtypes = (wintypes.HANDLE, wintypes.DWORD)
            api.WaitForSingleObject.restype = wintypes.DWORD
            api.ReleaseMutex.argtypes = (wintypes.HANDLE,)
            api.CloseHandle.argtypes = (wintypes.HANDLE,)
            handle = api.CreateMutexW(None, False, 'Local\\UniXNU.Device.' + digest)
            if not handle:
                raise ctypes.WinError(ctypes.get_last_error())
            result = api.WaitForSingleObject(handle, int(self.timeout * 1000))
            if result not in (0, 0x80):
                api.CloseHandle(handle)
                if result == 0x102:
                    raise TimeoutError('Debug device is owned by another process')
                raise ctypes.WinError(ctypes.get_last_error())
            self.handle, self.api = handle, api
        else:
            import fcntl
            path = Path(tempfile.gettempdir()) / ('unixnu-device-' + digest + '.lock')
            handle = path.open('a+b')
            deadline = time.monotonic() + self.timeout
            while True:
                try:
                    fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    break
                except BlockingIOError:
                    if time.monotonic() >= deadline:
                        handle.close()
                        raise TimeoutError('Debug device is owned by another process') from None
                    time.sleep(min(0.05, max(0, deadline - time.monotonic())))
            self.handle = handle
        return self

    def __exit__(self, *unused):
        handle, self.handle = self.handle, None
        if handle is None:
            return
        if os.name == 'nt':
            try:
                if not self.api.ReleaseMutex(handle):
                    import ctypes
                    raise ctypes.WinError(ctypes.get_last_error())
            finally:
                self.api.CloseHandle(handle)
        else:
            import fcntl
            try:
                fcntl.flock(handle, fcntl.LOCK_UN)
            finally:
                handle.close()
