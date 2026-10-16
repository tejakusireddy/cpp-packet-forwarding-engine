#!/usr/bin/env bash
set -euo pipefail
workspace=$(cd "$(dirname "$0")/.." && pwd)
namespace="pfe-test-$$"
trap 'ip netns del "$namespace"' EXIT
ip netns add "$namespace"
ip netns exec "$namespace" sysctl -q -w net.ipv6.conf.all.disable_ipv6=1
ip netns exec "$namespace" sysctl -q -w net.ipv6.conf.default.disable_ipv6=1
ip netns exec "$namespace" ip link add ingress type veth peer name source
ip netns exec "$namespace" ip link add egress type veth peer name sink
for interface in ingress source egress sink lo; do
  ip netns exec "$namespace" ip link set "$interface" up
done
ip netns exec "$namespace" python3 "$workspace/tests/linux_forwarding.py" "$workspace/build/packet_forwarder"
ip netns exec "$namespace" python3 "$workspace/tests/linux_forwarding.py" \
  "$workspace/build/packet_forwarder" "$workspace/build/route_ctl"
