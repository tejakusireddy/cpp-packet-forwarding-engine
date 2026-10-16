"""Run inside an isolated network namespace with four preconfigured veth devices."""
import socket
import struct
import subprocess
import sys
import tempfile
import json


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
live = len(sys.argv) > 2
temporary = tempfile.TemporaryDirectory(prefix="pfe-live-", dir="/tmp") if live else None
control_path = temporary.name + "/routing.sock" if live else None
command = [sys.argv[1], "ingress", "egress", mac_text, "2", "5" if live else "2"]
if live:
    command += ["--control", control_path]
process = subprocess.Popen(
    command,
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
    if live:
        def update(base, version, *arguments, expected="applied"):
            result = subprocess.run([sys.argv[2], control_path, str(base), str(version), *arguments],
                                    capture_output=True, text=True, timeout=5)
            assert result.returncode == (0 if expected == "applied" else 1), result.stderr
            ack = json.loads(result.stdout)
            assert ack["version"] == version and ack["status"] == expected, ack
            return ack

        # Remove the IPv4 default while the same process continues forwarding.
        update(1, 2, "erase", "0.0.0.0/0")
        source.send(frame.ljust(60, b"\0"))
        sink.settimeout(0.5)
        try:
            sink.recv(65536)
            raise AssertionError("Removed route still forwarded")
        except socket.timeout:
            pass
        sink.settimeout(5)
        update(1, 3, "upsert", "0.0.0.0/0", "0", expected="stale_base")
        update(2, 3, "upsert", "198.51.100.2/32", "999", expected="invalid")
        update(2, 3, "upsert", "0.0.0.0/0", "0")
        source.send(frame.ljust(60, b"\0"))
        restored = sink.recv(65536)
        assert restored[:6] == destination_mac and restored[22] == 63

        changed_mac = destination_mac[:-1] + bytes([destination_mac[-1] ^ 4])
        changed_text = ":".join(f"{byte:02x}" for byte in changed_mac)
        source_text = ":".join(f"{byte:02x}" for byte in source_mac)
        egress_id = str(socket.if_nametoindex("egress"))
        update(3, 4, "group", "1", egress_id, source_text, changed_text)
        update(4, 5, "upsert", "198.51.100.2/32", "1")
        source.send(frame.ljust(60, b"\0"))
        changed = sink.recv(65536)
        assert changed[:6] == changed_mac and changed[6:12] == source_mac
        assert changed[22] == 63 and checksum(changed[14:34]) == 0
        update(5, 6, "group", "2", str(int(egress_id) + 1000), source_text, mac_text, expected="invalid")
        # The runtime must cancel this incomplete session during finite shutdown.
        idle = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        idle.connect(control_path)
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
    assert ("forwarded=4 dropped=1 routing_version=5" if live else "forwarded=2 dropped=0") in output, output
    if live:
        import os
        assert not os.path.exists(control_path), "Control socket leaked after runtime shutdown"
    print("Linux live routing updates passed" if live else "Linux veth forwarding passed")
finally:
    if process.poll() is None:
        process.kill()
        process.wait()
    source.close()
    sink.close()
    egress.close()
    if live:
        if "idle" in globals():
            idle.close()
        temporary.cleanup()
