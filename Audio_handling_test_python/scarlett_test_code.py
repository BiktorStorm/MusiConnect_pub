import sounddevice as sd
import numpy as np

print(sd.query_devices())

# Replace with your Focusrite device index
device_index = 8  # ← change this to match your output

sd.default.device = device_index
sd.default.latency = 'low'
sd.default.samplerate = 48000

print("\nRecording 3 seconds from Focusrite and playing back...")
audio = sd.rec(int(3 * 48000), channels=1, dtype='int16')
sd.wait()
print("Playing back...")
sd.play(audio)
sd.wait()
print("Done!")