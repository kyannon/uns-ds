# Writes GT911-style multitouch evdev events into a FIFO: taps at given
# bottom-screen pixel positions (1024x768 panel), at given wall-clock delays.
import os, struct, sys, time
fifo = sys.argv[1]
taps = [(float(a), int(b), int(c)) for a, b, c in (t.split(',') for t in sys.argv[2:])]
fd = os.open(fifo, os.O_WRONLY)
def ev(t, c, v):
    os.write(fd, struct.pack('qqHHi', 0, 0, t, c, v))
start = time.time()
tid = 1
for delay, x, y in taps:
    while time.time() - start < delay: time.sleep(0.01)
    ev(3, 0x2f, 0); ev(3, 0x39, tid); ev(3, 0x35, x); ev(3, 0x36, y); ev(1, 0x14a, 1); ev(0, 0, 0)
    time.sleep(0.03); ev(3, 0x35, x + 1); ev(0, 0, 0)
    time.sleep(0.15); ev(3, 0x39, -1); ev(1, 0x14a, 0); ev(0, 0, 0)
    tid += 1
time.sleep(1); os.close(fd)
