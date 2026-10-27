"""Records the console's kernel log (klogsrv, TCP 3232) for N seconds into a file.
Usage: python3 klog.py <out-file> [seconds] [console-ip]"""
import socket, sys, time

out = sys.argv[1]
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 60
host = sys.argv[3] if len(sys.argv) > 3 else '192.168.100.22'
end = time.time() + secs
with open(out, 'wb') as f:
    s = socket.create_connection((host, 3232), timeout=10)
    s.settimeout(1)
    while time.time() < end:
        try:
            data = s.recv(65536)
            if not data:
                break
            f.write(data)
            f.flush()
        except socket.timeout:
            pass
