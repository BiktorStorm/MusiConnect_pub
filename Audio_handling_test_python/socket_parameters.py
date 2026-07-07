import pyaudio
import socket
import numpy
import time

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)

sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 1024)

sock.settimeout(0.005)

# Connection
IP = '192.168.10.1'
PORT = 12345

# Sound
FORMAT = pyaudio.paInt32
CHUNK = 4800
RATE = 48000
CHANNELS = 1

DURATION = 3
FREQ = 220.0

for i in range(1000):
    sock.sendto(str(i).encode(), (IP, PORT))
    time.sleep(0.005)

