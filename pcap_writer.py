"""
pcap_writer.py - Writes IEEE 802.11 + radiotap captures in pcap format.

Two modes:
  Buffered (default)  - large write buffer, flush every ~50 packets or
                         128 KB. Best throughput for "capture to a file,
                         inspect afterward" use (the original behavior).
  Stream   (stream_mode=True) - flush every single packet immediately,
                         no internal buffering delay. Required for live
                         tailing - e.g. writing into a FIFO that termshark
                         or Wireshark is reading from on the other end
                         (`termshark -i /path/to/fifo` or `termshark -i -` via
                         stdout). Buffered mode would make termshark sit
                         there showing nothing for tens of packets at a
                         time, then show a burst - not "live" in any
                         useful sense.

Output target can be:
  - a regular file path (string)            -> opened here, buffered or not
  - a FIFO path (string, pre-created with mkfifo)
                                              -> opened here; NOTE: open()
                                                for writing on a FIFO BLOCKS
                                                until a reader (e.g. termshark)
                                                has the other end open. This
                                                is normal Unix FIFO behavior,
                                                not a hang - start the reader
                                                first.
  - an already-open binary file object (e.g. sys.stdout.buffer)
                                              -> used directly, never closed
                                                (caller owns it)
"""
import struct
import sys
import time

PCAP_MAGIC = 0xA1B2C3D4
LINKTYPE_IEEE802_11_RADIOTAP = 127


class PcapWriter:
    def __init__(self, path_or_stream, snaplen: int = 65535,
                 buffer_size: int = 256 * 1024, stream_mode: bool = False,
                 on_blocking_open=None):
        """
        path_or_stream: file path (str) to open, or an already-open
                         binary file-like object (must support .write()
                         and ideally .flush()).
        stream_mode:     if True, every packet is written + flushed
                          immediately (no batching). Use this for live
                          capture (FIFO / stdout pipe to termshark).
        on_blocking_open: optional zero-arg callback invoked just before
                          opening a FIFO path, since that open() call can
                          block until a reader attaches. Use this to log
                          "waiting for termshark to attach..." so it's clear
                          the process isn't stuck.
        """
        self.stream_mode = stream_mode
        self._owns_handle = isinstance(path_or_stream, str)

        if self._owns_handle:
            if on_blocking_open is not None:
                on_blocking_open()
            self._f = open(path_or_stream, "wb",
                            buffering=(0 if stream_mode else buffer_size))
        else:
            self._f = path_or_stream

        self._write_global_header(snaplen)
        self.packet_count = 0
        self._buffer = bytearray()

    def _write_global_header(self, snaplen: int):
        hdr = struct.pack("<IHHiIII", PCAP_MAGIC, 2, 4, 0, 0, snaplen,
                           LINKTYPE_IEEE802_11_RADIOTAP)
        self._f.write(hdr)
        if self.stream_mode:
            self._flush()

    def _flush(self):
        try:
            self._f.flush()
        except (AttributeError, ValueError):
            pass

    def write_packet(self, data: bytes):
        now = time.time()
        sec = int(now)
        usec = int((now - sec) * 1_000_000)
        rec_hdr = struct.pack("<IIII", sec, usec, len(data), len(data))

        if self.stream_mode:
            packet_chunk = rec_hdr + data
            self._f.write(packet_chunk)
            self._flush()
            
            if hasattr(self._f, "flush"):
                try:
                    import os
                    os.fsync(self._f.fileno())
                except (OSError, AttributeError):
                    pass
        else:
            self._buffer.extend(rec_hdr)
            self._buffer.extend(data)
            if len(self._buffer) > 128 * 1024 or self.packet_count % 50 == 0:
                self._f.write(self._buffer)
                self._buffer.clear()

        self.packet_count += 1
    def close(self):
        if self._buffer:
            self._f.write(self._buffer)
            self._buffer.clear()
        if self._owns_handle:
            try:
                self._f.close()
            except (BrokenPipeError, OSError):
                pass

