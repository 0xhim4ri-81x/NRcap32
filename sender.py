"""
sender.py - Thread-safe sender for ESP32 USB bulk OUT endpoint.
"""

import threading
import time
import usb.core

_DEFAULT_TIMEOUT_MS = 3000   # USB write timeout
_MAX_RETRIES        = 2


class Sender:
    def __init__(self, device, ep_out_addr: int, log_func=None,
                 timeout_ms: int = _DEFAULT_TIMEOUT_MS):
        self.device       = device
        self.ep_out_addr  = ep_out_addr
        self.log          = log_func or print
        self._timeout_ms  = timeout_ms
        self._lock        = threading.Lock()


    def send_bytes(self, data: bytes) -> bool:
        """
        Send raw bytes with retry on recoverable USB errors.
        Thread-safe. Returns True on success.
        """
        with self._lock:
            for attempt in range(_MAX_RETRIES):
                try:
                    self.device.write(self.ep_out_addr, data,
                                      timeout=self._timeout_ms)
                    return True
                except usb.core.USBError as e:
                    code = getattr(e, 'errno', None)
                    if code in (32, 75) and attempt < _MAX_RETRIES - 1:
                        time.sleep(0.02 * (attempt + 1))
                        continue
                    self.log(f"[sender] write error (attempt {attempt+1}): {e}")
                    return False
        return False

    def send(self, message: str, newline: bool = True) -> bool:
        """Send a UTF-8 string (legacy text mode, still used for debug)."""
        data = (message + "\n" if newline else message).encode("utf-8")
        return self.send_bytes(data)
