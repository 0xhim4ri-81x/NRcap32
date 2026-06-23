"""
protocol.py - Framed binary protocol for ESP32 ↔ Termux communication.

Frame layout:
  [MAGIC 2B][TYPE 1B][ID 1B][LENGTH 4B LE][PAYLOAD NB]

Types:
  0x01  CMD        Termux -> ESP32   JSON command
  0x02  RESP       ESP32 -> Termux   JSON response (mirrors CMD id)
  0x03  EVENT      ESP32 -> Termux   JSON async event (id=0)
  0x04  PCAP       ESP32 -> Termux   binary chunk  (id = chunk index)
  0x05  ACK        Termux -> ESP32   JSON {"chunk": N}
"""

import json
import struct
import threading
import queue

MAGIC        = b'\xAD\xDE'
TYPE_CMD     = 0x01
TYPE_RESP    = 0x02
TYPE_EVENT   = 0x03
TYPE_PCAP    = 0x04
TYPE_ACK     = 0x05
HEADER_SIZE  = 8          # 2 magic + 1 type + 1 id + 4 length (LE)
MAX_PAYLOAD  = 65536      # sanity cap; real chunks are ≤512 B


#  Frame building 

def build_frame(ptype: int, pid: int, payload: bytes) -> bytes:
    header = MAGIC + struct.pack('<BBL', ptype, pid, len(payload))
    return header + payload

def build_cmd(cmd: str, args: dict = None, pid: int = 0) -> bytes:
    payload = json.dumps({"cmd": cmd, "args": args or {}}).encode()
    return build_frame(TYPE_CMD, pid, payload)

def build_ack(chunk_index: int) -> bytes:
    payload = json.dumps({"chunk": chunk_index}).encode()
    return build_frame(TYPE_ACK, 0, payload)


#  Frame parsing ─

class FrameParser:
    """
    Stateful streaming parser.
    Feed raw bytes in any chunk size, get complete frame dicts out.
    Uses bytearray internally to avoid O(n²) bytes concatenation.
    """

    def __init__(self):
        self._buf = bytearray()

    def reset(self):
        self._buf = bytearray()

    def feed(self, data: bytes) -> list:
        """Feed raw bytes. Returns list of parsed frame dicts (may be empty)."""
        self._buf += data
        frames = []
        while True:
            frame = self._try_parse()
            if frame is None:
                break
            frames.append(frame)
        return frames

    def _try_parse(self):
        buf = self._buf

        if len(buf) < HEADER_SIZE:
            return None

        # Resync on bad magic
        if buf[0] != 0xAD or buf[1] != 0xDE:
            idx = buf.find(b'\xAD\xDE', 1)
            if idx == -1:
                self._buf = bytearray(buf[-1:]) if buf else bytearray()
                return None
            del self._buf[:idx]
            buf = self._buf
            if len(buf) < HEADER_SIZE:
                return None

        ptype, pid, length = struct.unpack_from('<BBL', buf, 2)

        if length > MAX_PAYLOAD:
            del self._buf[:2]
            return None

        total = HEADER_SIZE + length
        if len(buf) < total:
            return None

        payload = bytes(buf[HEADER_SIZE:total])
        del self._buf[:total]

        return {
            "type":    ptype,
            "id":      pid,
            "length":  length,
            "payload": payload,
            "json":    _safe_json(payload) if ptype != TYPE_PCAP else None,
        }


def _safe_json(data: bytes):
    try:
        return json.loads(data)
    except Exception:
        return None



class Protocol:
    """
    Wraps a Sender + ReceiverThread with framed protocol logic.

    Usage:
        proto = Protocol(sender, receiver)
        proto.start()

        resp = proto.send_cmd("SCAN_WIFI", timeout=30)

        proto.on_event = lambda ev: ...     # called in dispatch thread
        proto.on_pcap  = lambda data, idx: ...
    """

    def __init__(self, sender, receiver_thread):
        self._tx       = sender
        self._rx       = receiver_thread
        self._parser   = FrameParser()
        self._pending  = {}              # pid -> {"event": Event, "result": any}
        self._lock     = threading.Lock()
        self._id_ctr   = 1
        self._running  = False

        self._dispatch_q  = queue.Queue(maxsize=2048)

        self.on_event  = None   # fn(frame_dict)
        self.on_pcap   = None   # fn(data: bytes, chunk_idx: int)
        self.log       = print

    def start(self):
        self._running = True
        self._parser.reset()

        t_rx = threading.Thread(target=self._read_loop,
                                daemon=True, name="proto-rx")
        t_rx.start()

        t_cb = threading.Thread(target=self._callback_loop,
                                daemon=True, name="proto-cb")
        t_cb.start()

    def stop(self):
        self._running = False
        with self._lock:
            for entry in self._pending.values():
                entry["event"].set()

    def reset(self):
        """Clear parser state and pending commands. Call between retries."""
        self._parser.reset()
        with self._lock:
            for entry in self._pending.values():
                entry["event"].set()
            self._pending.clear()

    def send_cmd(self, cmd: str, args: dict = None, timeout: float = 5.0):
        """
        Send a CMD frame, block until the matching RESP arrives.
        Returns the response JSON dict, or None on timeout.
        """
        with self._lock:
            # Pick an ID not currently in use
            pid = self._id_ctr & 0xFF
            while pid in self._pending or pid == 0:
                self._id_ctr = (self._id_ctr % 254) + 1
                pid = self._id_ctr & 0xFF
            self._id_ctr = (self._id_ctr % 254) + 1

            evt = threading.Event()
            self._pending[pid] = {"event": evt, "result": None}

        frame = build_cmd(cmd, args or {}, pid)
        self._tx.send_bytes(frame)
        self.log(f"[proto] >> CMD id={pid} cmd={cmd}")

        evt.wait(timeout=timeout)

        with self._lock:
            entry = self._pending.pop(pid, None)

        if entry and entry["result"] is not None:
            return entry["result"]

        self.log(f"[proto] CMD id={pid} '{cmd}' timed out after {timeout}s")
        return None

    def send_ack(self, chunk_index: int):
        self._tx.send_bytes(build_ack(chunk_index))

    def _read_loop(self):
        while self._running:
            raw = self._rx.readline_bytes(timeout=0.01)   # 10 ms poll
            if not raw:
                continue
            for frame in self._parser.feed(raw):
                self._dispatch(frame)

    def _dispatch(self, frame: dict):
        t = frame["type"]

        if t == TYPE_RESP:
            pid = frame["id"]
            with self._lock:
                entry = self._pending.get(pid)
            if entry:
                entry["result"] = frame["json"]
                entry["event"].set()

        elif t == TYPE_EVENT:
            try:
                self._dispatch_q.put_nowait(("event", frame["json"]))
            except queue.Full:
                pass   # drop events under extreme backpressure

        elif t == TYPE_PCAP:
            idx = frame["id"]
            # ACK immediately in the read loop for minimum round-trip time
            self.send_ack(idx)
            try:
                self._dispatch_q.put_nowait(("pcap", frame["payload"], idx))
            except queue.Full:
                pass

    def _callback_loop(self):
        while self._running:
            try:
                item = self._dispatch_q.get(timeout=0.05)
            except queue.Empty:
                continue

            kind = item[0]
            try:
                if kind == "event" and self.on_event:
                    self.on_event(item[1])
                elif kind == "pcap" and self.on_pcap:
                    self.on_pcap(item[1], item[2])
            except Exception as e:
                self.log(f"[proto] callback error: {e}")
