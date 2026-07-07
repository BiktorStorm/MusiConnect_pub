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


def _open_stream(direction, device, samplerate, channels, dtype, blocksize, extra_settings):
    common = dict(
        samplerate=samplerate,
        channels=channels,
        dtype=dtype,
        blocksize=blocksize,
        latency='low',
        device=device,
        extra_settings=extra_settings,
    )
    if direction == 'input':
        return sd.InputStream(**common)
    else:
        return sd.OutputStream(**common)


def make_stream(direction, backend_info, samplerate, channels, dtype, blocksize):
    """
    Create an InputStream or OutputStream configured for low latency.

    For WASAPI, tries EXCLUSIVE mode first (lowest latency); if the device
    rejects the requested format (common on stereo-only endpoints asked for
    mono), automatically falls back to SHARED mode so the POC still runs.
    Returns the (unstarted) stream.
    """
    device = backend_info['device']
    backend = backend_info['backend']

    # Build ordered list of (label, extra_settings) attempts
    attempts = []
    if backend == 'WASAPI-exclusive':
        attempts.append(('WASAPI exclusive', sd.WasapiSettings(exclusive=True)))
        attempts.append(('WASAPI shared', None))
    elif backend == 'ASIO':
        attempts.append(('ASIO', None))
    else:
        attempts.append(('default', None))

    last_err = None
    for label, extra in attempts:
        try:
            stream = _open_stream(direction, device, samplerate,
                                  channels, dtype, blocksize, extra)
            print(f"[backend] Stream opened via: {label}")
            return stream
        except sd.PortAudioError as e:
            print(f"[backend] {label} failed ({e}); trying next option...")
            last_err = e

    # Nothing worked
    raise last_err
