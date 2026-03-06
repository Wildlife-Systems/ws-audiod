#!/usr/bin/env python3
"""
Audio Daemon Python Client Library

Provides a Python interface for interacting with the ws-audiod daemon.
"""

import socket
import struct
import mmap
import os
import time
import json
from dataclasses import dataclass
from typing import Optional, Tuple, Any

try:
    import numpy as np
    HAS_NUMPY = True
except ImportError:
    HAS_NUMPY = False

__version__ = "1.0.0"
__all__ = [
    "AudioClient",
    "SampleSubscriber",
    "Response",
    "capture_clip",
    "get_status",
    "get_levels",
    "DEFAULT_SOCKET_PATH",
    "DEFAULT_SHM_NAME",
]

DEFAULT_SOCKET_PATH = "/run/ws-audiod/control.sock"
DEFAULT_SHM_NAME = "/ws_audiod_samples"
HEADER_SIZE = 64


@dataclass
class Response:
    """Response from the daemon (JSON format).

    Response format:
        Success: {"ok": true, "path": "..."}  or  {"ok": true, "data": {...}}
        Error:   {"ok": false, "error": "..."}
    """
    success: bool
    path: Optional[str] = None
    data: Optional[Any] = None
    error: Optional[str] = None
    raw: str = ""

    @classmethod
    def from_string(cls, s: str) -> 'Response':
        s = s.strip()
        try:
            obj = json.loads(s)
            return cls(
                success=obj.get("ok", False),
                path=obj.get("path"),
                data=obj.get("data"),
                error=obj.get("error"),
                raw=s
            )
        except json.JSONDecodeError:
            return cls(success=False, error=f"Invalid JSON: {s}", raw=s)


class AudioClient:
    """Control interface client for ws-audiod.

    Usage:
        with AudioClient() as client:
            resp = client.capture_clip(-5, 5)
            print(resp.path)
    """

    def __init__(self, socket_path: str = DEFAULT_SOCKET_PATH):
        self._socket_path = socket_path
        self._sock: Optional[socket.socket] = None

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()

    def _connect(self) -> socket.socket:
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.settimeout(30.0)
        sock.connect(self._socket_path)
        return sock

    def _send_command(self, cmd: str) -> Response:
        sock = self._connect()
        try:
            sock.sendall((cmd + "\n").encode())
            data = b""
            while True:
                chunk = sock.recv(4096)
                if not chunk:
                    break
                data += chunk
                if b"\n" in data:
                    break
            return Response.from_string(data.decode())
        finally:
            sock.close()

    def capture_clip(self, start_offset: int = -5, end_offset: int = 5,
                     fmt: str = "") -> Response:
        cmd = f"CLIP {start_offset} {end_offset}"
        if fmt:
            cmd += f" {fmt}"
        return self._send_command(cmd)

    def get_status(self) -> Response:
        return self._send_command("GET STATUS")

    def get_levels(self) -> Response:
        return self._send_command("GET LEVEL")

    def set_parameter(self, key: str, value: str) -> Response:
        return self._send_command(f"SET {key} {value}")

    def close(self):
        if self._sock:
            self._sock.close()
            self._sock = None


class SampleSubscriber:
    """Zero-copy shared memory reader for raw audio samples.

    Reads the latest audio period from the daemon's shared memory region.

    Usage:
        with SampleSubscriber() as sub:
            samples, meta = sub.read_samples()
    """

    def __init__(self, shm_name: str = DEFAULT_SHM_NAME):
        self._shm_name = shm_name
        self._fd: Optional[int] = None
        self._mm: Optional[mmap.mmap] = None
        self._sample_rate = 0
        self._channels = 0
        self._bits = 0
        self._period_frames = 0

    def __enter__(self):
        self._open()
        return self

    def __exit__(self, *args):
        self.close()

    def _open(self):
        path = f"/dev/shm{self._shm_name}"
        if not os.path.exists(path):
            raise FileNotFoundError(
                f"Shared memory {self._shm_name} not found. "
                "Is ws-audiod running with enable_sample_sharing=true?"
            )

        self._fd = os.open(path, os.O_RDONLY)
        size = os.fstat(self._fd).st_size
        self._mm = mmap.mmap(self._fd, size, access=mmap.ACCESS_READ)

        # Read header
        header = self._mm[:HEADER_SIZE]
        magic = struct.unpack_from('<I', header, 0)[0]
        if magic != 0x41554449:
            raise ValueError(f"Invalid magic: 0x{magic:08X}")

        self._sample_rate = struct.unpack_from('<I', header, 4)[0]
        self._channels = struct.unpack_from('<H', header, 8)[0]
        self._bits = struct.unpack_from('<H', header, 10)[0]
        self._period_frames = struct.unpack_from('<I', header, 12)[0]

    @property
    def sample_rate(self) -> int:
        return self._sample_rate

    @property
    def channels(self) -> int:
        return self._channels

    @property
    def period_frames(self) -> int:
        return self._period_frames

    def read_samples(self, timeout_ms: int = 1000) -> Tuple[Optional[Any], dict]:
        """Read the latest period of samples.

        Returns (samples_array, metadata_dict).
        If numpy is available, samples is an ndarray; otherwise a bytes object.
        """
        if not self._mm:
            return None, {}

        # Read write counter
        counter = struct.unpack_from('<Q', self._mm, 16)[0]
        timestamp = struct.unpack_from('<Q', self._mm, 24)[0]

        sample_count = self._period_frames * self._channels
        byte_count = sample_count * (self._bits // 8)
        raw = self._mm[HEADER_SIZE:HEADER_SIZE + byte_count]

        meta = {
            "counter": counter,
            "timestamp_us": timestamp,
            "sample_rate": self._sample_rate,
            "channels": self._channels,
            "period_frames": self._period_frames,
        }

        if HAS_NUMPY:
            dtype = np.int16 if self._bits == 16 else np.int32
            samples = np.frombuffer(raw, dtype=dtype).copy()
            if self._channels > 1:
                samples = samples.reshape(-1, self._channels)
            return samples, meta

        return raw, meta

    def close(self):
        if self._mm:
            self._mm.close()
            self._mm = None
        if self._fd is not None:
            os.close(self._fd)
            self._fd = None


# Convenience functions

def capture_clip(start_offset: int = -5, end_offset: int = 5,
                 fmt: str = "",
                 socket_path: str = DEFAULT_SOCKET_PATH) -> Optional[str]:
    """Capture an audio clip.  Returns the file path or None on error."""
    with AudioClient(socket_path) as client:
        resp = client.capture_clip(start_offset, end_offset, fmt)
        return resp.path if resp.success else None


def get_status(socket_path: str = DEFAULT_SOCKET_PATH) -> Optional[dict]:
    """Get daemon status as a dictionary."""
    with AudioClient(socket_path) as client:
        resp = client.get_status()
        return resp.data if resp.success else None


def get_levels(socket_path: str = DEFAULT_SOCKET_PATH) -> Optional[dict]:
    """Get current audio levels."""
    with AudioClient(socket_path) as client:
        resp = client.get_levels()
        return resp.data if resp.success else None


if __name__ == "__main__":
    import sys

    def print_usage():
        print("Audio Daemon Python Client")
        print()
        print("Usage:")
        print(f"  {sys.argv[0]} clip <start> <end> [format]  - Extract an audio clip")
        print(f"  {sys.argv[0]} status                       - Get daemon status")
        print(f"  {sys.argv[0]} levels                       - Get audio levels")
        print(f"  {sys.argv[0]} stream                       - Stream samples from shm")
        print()
        print("Examples:")
        print(f"  {sys.argv[0]} clip -5 5          # 10s clip centred on now")
        print(f"  {sys.argv[0]} clip -30 0 flac    # 30s FLAC from buffer")
        print()

    if len(sys.argv) < 2:
        print_usage()
        sys.exit(0)

    command = sys.argv[1].lower()

    if command == "clip":
        if len(sys.argv) < 4:
            print("Error: clip requires start and end offsets")
            sys.exit(1)
        start = int(sys.argv[2])
        end = int(sys.argv[3])
        fmt = sys.argv[4] if len(sys.argv) > 4 else ""
        path = capture_clip(start, end, fmt)
        if path:
            print(f"Clip saved: {path}")
        else:
            print("Clip extraction failed")

    elif command == "status":
        status = get_status()
        if status:
            print(json.dumps(status, indent=2))
        else:
            print("Failed to get status")

    elif command == "levels":
        levels = get_levels()
        if levels:
            print(json.dumps(levels, indent=2))
        else:
            print("Failed to get levels")

    elif command == "stream":
        print("Streaming samples from shared memory (Ctrl+C to stop)...")
        with SampleSubscriber() as sub:
            print(f"Rate: {sub.sample_rate} Hz, Channels: {sub.channels}, "
                  f"Period: {sub.period_frames} frames")
            count = 0
            start_time = time.time()
            last_counter = 0
            try:
                while True:
                    samples, meta = sub.read_samples()
                    if samples is not None and meta["counter"] != last_counter:
                        last_counter = meta["counter"]
                        count += 1
                        elapsed = time.time() - start_time
                        if count % 100 == 0:
                            print(f"  periods: {count}, "
                                  f"elapsed: {elapsed:.1f}s, "
                                  f"rate: {count/elapsed:.1f} periods/s")
                    else:
                        time.sleep(0.001)
            except KeyboardInterrupt:
                print(f"\nStopped after {count} periods")
    else:
        print_usage()
