"""Run inside an isolated network namespace with four preconfigured veth devices."""
import socket
import struct
import subprocess
import sys


def checksum(data):
    total = sum(struct.unpack(f"!{len(data) // 2}H", data))
    while total >> 16:
        total = (total & 65535) + (total >> 16)
    return (~total) & 65535


source = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(3))
sink = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(3))
source.bind(("source", 0))
sink.bind(("sink", 0))
sink.settimeout(5)
egress = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(3))
egress.bind(("egress", 0))
source_mac = egress.getsockname()[4]
destination_mac = sink.getsockname()[4]
mac_text = ":".join(f"{byte:02x}" for byte in destination_mac)
process = subprocess.Popen(
    [sys.argv[1], "ingress", "egress", mac_text, "2", "2"],
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
    text=True,
)
try:
    assert process.stdout.readline().strip() == "ready", "forwarder did not start"
    header = struct.pack("!BBHHHBBH4s4s", 0x45, 0, 28, 42, 0, 64, 17, 0,
                         socket.inet_aton("192.0.2.1"), socket.inet_aton("198.51.100.2"))
    header = header[:10] + struct.pack("!H", checksum(header)) + header[12:]
    ethernet = b"\x02\0\0\0\0\1" + source.getsockname()[4] + b"\x08\0"
    frame = ethernet + header + struct.pack("!HHHH", 1234, 5678, 8, 0)
    source.send(frame.ljust(60, b"\0"))
    forwarded = sink.recv(65536)
    assert forwarded[:6] == destination_mac
    assert forwarded[6:12] == source_mac and forwarded[12:14] == b"\x08\0"
    assert forwarded[22] == 63 and checksum(forwarded[14:34]) == 0
    assert forwarded[34:42] == frame[34:42]
    assert len(forwarded) >= 60 and not any(forwarded[42:60])
    ipv6 = struct.pack("!IHBB16s16s", 6 << 28, 8, 17, 64,
                       socket.inet_pton(socket.AF_INET6, "2001:db8::1"),
                       socket.inet_pton(socket.AF_INET6, "2001:db8::2"))
    udp = struct.pack("!HHHH", 4321, 8765, 8, 0)
    pseudo = ipv6[8:40] + struct.pack("!I3xB", len(udp), 17)
    udp = udp[:6] + struct.pack("!H", checksum(pseudo + udp) or 65535)
    frame6 = ethernet[:12] + b"\x86\xdd" + ipv6 + udp
    source.send(frame6)
    forwarded6 = sink.recv(65536)
    assert forwarded6[:6] == destination_mac and forwarded6[6:12] == source_mac
    assert forwarded6[12:14] == b"\x86\xdd" and forwarded6[21] == 63
    assert forwarded6[54:62] == udp
    output, errors = process.communicate(timeout=5)
    assert process.returncode == 0, errors
    assert "forwarded=2 dropped=0" in output, output
    print("Linux veth forwarding passed")
finally:
    if process.poll() is None:
        process.kill()
        process.wait()
    source.close()
    sink.close()
    egress.close()
