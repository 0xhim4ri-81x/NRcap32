"""
receiver.py - Background thread reading raw bytes from ESP32 bulk IN endpoint.
"""

import threading
import queue
import time
import usb.core

# How many raw-byte chunks to buffer before dropping oldest.
# 512 × 16 KB = 8 MB max memory - comfortable on any modern phone.
_BYTE_QUEUE_DEPTH = 512
_LINE_QUEUE_DEPTH = 256
_READ_SIZE        = 16 * 1024   # 16 KB per bulk read
_USB_TIMEOUT_MS   = 100         # tighter poll → lower latency


class ReceiverThread(threading.Thread):
    def __init__(self, device, ep_in_addr: int, log_func=None):
        super().__init__(daemon=True, name="ESP32-Receiver")
        self.device      = device
        self.ep_in_addr  = ep_in_addr
        self.log         = log_func or print
        self._line_queue = queue.Queue(maxsize=_LINE_QUEUE_DEPTH)
        self._byte_queue = queue.Queue(maxsize=_BYTE_QUEUE_DEPTH)
        self._stop_event = threading.Event()
        self._buf        = b""          # only used for line splitting
        self._error_count = 0

    def stop(self):
        self._stop_event.set()

    def readline(self, timeout: float = None) -> "str | None":
        """Get next text line (for READY handshake / debug). Non-blocking with timeout."""
        try:
            return self._line_queue.get(timeout=timeout)
        except queue.Empty:
            return None

    def readline_bytes(self, timeout: float = None) -> "bytes | None":
        """Get next raw USB chunk for the framed protocol parser."""
        try:
            return self._byte_queue.get(timeout=timeout)
        except queue.Empty:
            return None

    def drain_stale(self, duration: float = 0.5):
        """
        Discard any bytes sitting in our internal queues that were filled by
        the receiver thread from a previous session.

        IMPORTANT: Do NOT call device.read() here - the receiver thread owns
        the USB endpoint exclusively. Calling read() from a second thread
        causes libusb on Android to return corrupt data or lock up the
        endpoint, which is what causes the 30-second hang on the second run.

        Instead we just drain the software queues. The receiver thread is
        already running and will have consumed any hardware FIFO leftovers
        within a few hundred ms via its normal poll loop.
        """
        deadline = time.monotonic() + duration
        drained = 0
        while time.monotonic() < deadline:
            drained_this_round = 0
            try:
                self._byte_queue.get_nowait()
                drained += 1
                drained_this_round += 1
            except queue.Empty:
                pass
            try:
                self._line_queue.get_nowait()
                drained_this_round += 1
            except queue.Empty:
                pass
            if drained_this_round == 0:
                time.sleep(0.02)
        self._buf = b""


    def run(self):
        self.log(f"[receiver] started EP=0x{self.ep_in_addr:02X}")
        while not self._stop_event.is_set():
            try:
                chunk = bytes(self.device.read(self.ep_in_addr, _READ_SIZE,
                                               timeout=_USB_TIMEOUT_MS))
                if not chunk:
                    continue

                self._error_count = 0
                try:
                    self._byte_queue.put_nowait(chunk)
                except queue.Full:
                    try:
                        self._byte_queue.get_nowait()
                    except queue.Empty:
                        pass
                    self._byte_queue.put_nowait(chunk)

                self._buf += chunk
                self._flush_lines()

            except usb.core.USBTimeoutError:
                continue

            except usb.core.USBError as e:
                code = getattr(e, 'errno', None)

                if code in (None, 32, 75):
                    self._error_count += 1
                    if self._error_count <= 5:
                        self.log(f"[receiver] USB error (recoverable, #{self._error_count}): {e}")
                    time.sleep(min(0.05 * self._error_count, 0.3))
                    continue
                else:
                    self.log(f"[receiver] fatal USB error: {e}")
                    break

        self.log("[receiver] stopped")

    def _flush_lines(self):
        """Split accumulated buffer into newline-delimited text lines."""
        while b"\n" in self._buf:
            line, self._buf = self._buf.split(b"\n", 1)
            text = line.rstrip(b"\r").decode("utf-8", errors="replace").strip()
            if text:
                try:
                    self._line_queue.put_nowait(text)
                except queue.Full:
                    pass   # don't block on debug lines
