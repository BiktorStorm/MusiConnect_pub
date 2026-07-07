import sounddevice as sd

print("=== All Audio Devices ===")
print(sd.query_devices())
print()

print("=== Host APIs ===")
for i, api in enumerate(sd.query_hostapis()):
    print(f"  {i}: {api['name']} (default in: {api['default_input_device']}, default out: {api['default_output_device']})")

print()
print("=== ASIO Devices ===")
asio_hostapi = None
for i, api in enumerate(sd.query_hostapis()):
    if 'ASIO' in api['name']:
        asio_hostapi = i
        break

if asio_hostapi is not None:
    print(f"ASIO host API found at index {asio_hostapi}")
    for d in range(len(sd.query_devices())):
        dev = sd.query_devices(d)
        if dev['hostapi'] == asio_hostapi:
            print(f"  Device {d}: {dev['name']} ({dev['max_input_channels']} in, {dev['max_output_channels']} out)")
else:
    print("NO ASIO HOST API FOUND!")
    print("The Focusrite ASIO driver may not be loaded. Make sure the Scarlett is plugged in via USB.")
