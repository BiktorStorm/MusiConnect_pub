"""
Low-latency audio backend selector for the MusiConnect POC.

Picks the lowest-latency backend available, in this order:
  1. ASIO              — best (~1-3ms), needs a real interface driver (e.g. Focusrite)
  2. WASAPI exclusive  — near-ASIO (~3-5ms), works with the stock pip 'sounddevice' wheel
  3. Default/shared     — fallback so the POC still runs anywhere (higher latency)

Usage (in sender/receiver):

    from low_latency_backend import select_backend, make_stream

    be = select_backend('input')   # or 'output'
    stream = make_stream('input', be, samplerate=48000, channels=1,
                         dtype='int16', blocksize=96)
"""

import sounddevice as sd


def _find_hostapi(substring):
    """Return the index of the first host API whose name contains `substring`."""
    for i, api in enumerate(sd.query_hostapis()):
        if substring.lower() in api['name'].lower():
            return i
    return None


def _devices_for_hostapi(hostapi_index, direction):
    """List (index, name) of devices on a host API that support the direction."""
    key = 'max_input_channels' if direction == 'input' else 'max_output_channels'
    out = []
    for d in range(len(sd.query_devices())):
        dev = sd.query_devices(d)
        if dev['hostapi'] == hostapi_index and dev[key] > 0:
            out.append((d, dev['name']))
    return out


def select_backend(direction):
    """
    Interactively select a device + backend for the given direction
    ('input' or 'output').

    Returns a dict:
        {
          'backend':       'ASIO' | 'WASAPI-exclusive' | 'default',
          'device':        int device index (or None for default),
          'extra_settings': sd.WasapiSettings | None,
        }
    """
    assert direction in ('input', 'output')

    print("=== Available Audio Devices ===")
    print(sd.query_devices())
    print()

    asio_api = _find_hostapi('ASIO')
    wasapi_api = _find_hostapi('WASAPI')

    if asio_api is not None:
        backend = 'ASIO'
        hostapi = asio_api
        extra_settings = None
        print(f"[backend] ASIO available (host API index {asio_api}) — lowest latency")
    elif wasapi_api is not None:
        backend = 'WASAPI-exclusive'
        hostapi = wasapi_api
        extra_settings = sd.WasapiSettings(exclusive=True)
        print("[backend] No ASIO found — using WASAPI EXCLUSIVE mode (near-ASIO latency)")
        print("          (bypasses the Windows mixer; other apps can't share the device)")
    else:
        backend = 'default'
        hostapi = None
        extra_settings = None
        print("[backend] No ASIO or WASAPI found — using default device (higher latency)")

    # Show the candidate devices for the chosen backend
    default_device = None
    if hostapi is not None:
        candidates = _devices_for_hostapi(hostapi, direction)
        if candidates:
            print(f"\n{backend} {direction} devices:")
            for idx, name in candidates:
                print(f"  [{idx}] {name}")
            default_device = candidates[0][0]
        else:
            print(f"\n(no {direction}-capable devices on {backend}; will use default)")

    prompt = f"\nSelect {direction} device index"
    if default_device is not None:
        prompt += f" (Enter = {default_device})"
    else:
        prompt += " (Enter = system default)"
    prompt += ": "

    raw = input(prompt).strip()
    if raw:
        device = int(raw)
    else:
        device = default_device  # may be None -> system default

    print(f"[backend] Using {backend}, device={device}")
    return {'backend': backend, 'device': device, 'extra_settings': extra_settings}


def open_stream(direction, backend_info, **stream_kwargs):
    """
    Open an Input/OutputStream with automatic backend fallback.

    `stream_kwargs` are passed straight to sd.InputStream / sd.OutputStream —
    e.g. samplerate, channels, dtype, blocksize, latency, callback.

    For WASAPI, tries EXCLUSIVE mode first (lowest latency); if the device
    rejects the requested format, falls back to SHARED mode so the POC still runs.

    Returns (stream, label).
    """
    device = backend_info['device']
    backend = backend_info['backend']

    attempts = []
    if backend == 'WASAPI-exclusive':
        attempts.append(('WASAPI exclusive', sd.WasapiSettings(exclusive=True)))
        attempts.append(('WASAPI shared', None))
    elif backend == 'ASIO':
        attempts.append(('ASIO', None))
    else:
        attempts.append(('default', None))

    cls = sd.InputStream if direction == 'input' else sd.OutputStream

    last_err = None
    for label, extra in attempts:
        try:
            stream = cls(device=device, extra_settings=extra, **stream_kwargs)
            print(f"[backend] Stream opened via: {label}")
            return stream, label
        except sd.PortAudioError as e:
            print(f"[backend] {label} failed ({e}); trying next option...")
            last_err = e

    raise last_err


def make_stream(direction, backend_info, samplerate, channels, dtype, blocksize):
    """Backward-compatible blocking-stream opener (no callback)."""
    stream, _ = open_stream(
        direction, backend_info,
        samplerate=samplerate, channels=channels, dtype=dtype,
        blocksize=blocksize, latency='low',
    )
    return stream


# =============================================================================
# Jitter buffer — decouples the network from the audio clock.
#
# The network thread pushes received frames in; the audio callback pulls
# fixed-size blocks out. A small target fill (prebuffer) absorbs network jitter
# so the output callback rarely underruns (which is what causes the crackle/
# distortion). Latency is bounded by `max_ms` so it can't balloon.
# =============================================================================

import threading
import numpy as np


class JitterBuffer:
    def __init__(self, channels, rate, target_ms=25, max_ms=120, dtype=np.int16):
        self.channels = channels
        self.dtype = dtype
        self.lock = threading.Lock()
        self.buf = np.zeros((0, channels), dtype=dtype)
        self.target = int(rate * target_ms / 1000)
        self.max = int(rate * max_ms / 1000)
        self.filling = True   # (re)buffering until we reach target
        self.underruns = 0
        self.overflows = 0

    def push(self, frame):
        """frame: np.ndarray shape (n, channels), same dtype."""
        with self.lock:
            self.buf = np.concatenate((self.buf, frame), axis=0)
            if len(self.buf) > self.max:
                # Drop oldest to bound latency
                drop = len(self.buf) - self.max
                self.buf = self.buf[drop:]
                self.overflows += 1

    def pull(self, n):
        """Return exactly n frames (n, channels), zero-filled on underrun."""
        with self.lock:
            if self.filling:
                if len(self.buf) < self.target:
                    return np.zeros((n, self.channels), dtype=self.dtype)
                self.filling = False  # enough buffered, start playing

            if len(self.buf) >= n:
                out = self.buf[:n].copy()
                self.buf = self.buf[n:]
                return out

            # Underrun: hand back what we have + silence, then re-buffer
            out = np.zeros((n, self.channels), dtype=self.dtype)
            have = len(self.buf)
            if have:
                out[:have] = self.buf
            self.buf = np.zeros((0, self.channels), dtype=self.dtype)
            self.underruns += 1
            self.filling = True
            return out

    def fill_frames(self):
        with self.lock:
            return len(self.buf)

