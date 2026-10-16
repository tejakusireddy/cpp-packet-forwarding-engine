# Routing IPC version 1

Each stream message starts with a four-byte unsigned big-endian payload length.
Zero or a length greater than 1,048,576 is invalid. All following integers are
unsigned and big-endian. Byte arrays retain their network order. There is no
native-struct alignment, implicit padding, or host-endian data on the wire.

## Batch payload

| Offset | Bytes | Field |
| --- | --- | --- |
| 0 | 4 | Magic `PFE1` (`0x50464531`) |
| 4 | 2 | Protocol version, 1 |
| 6 | 2 | Reserved, zero |
| 8 | 8 | Expected base routing version |
| 16 | 8 | Proposed new routing version |
| 24 | 4 | Route-change count, at most 32,768 |
| 28 | 4 | Group count, at most 1,024 |
| 32 | variable | Route changes, then groups |

Each route change is 24 bytes:

| Relative offset | Bytes | Field |
| --- | --- | --- |
| 0 | 1 | Family, 4 or 6 |
| 1 | 1 | Prefix length, 0–32 or 0–128 |
| 2 | 1 | Remove flag, 0 or 1 |
| 3 | 1 | Reserved, zero |
| 4 | 4 | Next-hop group ID; ignored for removal |
| 8 | 16 | Prefix bytes; IPv4 uses the first four |

A group has an eight-byte header: group ID (4), member count (2, 1–256), reserved
zero (2). Each following member is 24 bytes: ID (4), interface ID (4), source MAC
(6), destination MAC (6), availability (1, 0/1), reserved zero (3).

The byte limit applies in addition to entry-count limits. Duplicate member/group
IDs and missing route references are semantic errors checked by snapshot
construction. Prefix host bits are ignored. Route changes apply in wire order;
group upserts apply before route changes. There is no group-delete operation.

## Acknowledgment payload

| Offset | Bytes | Field |
| --- | --- | --- |
| 0 | 4 | Magic `ACK1` (`0x41434b31`) |
| 4 | 2 | Protocol version, 1 |
| 6 | 2 | Reserved, zero |
| 8 | 8 | Requested new version |
| 16 | 8 | Current version when acknowledgment was created |
| 24 | 1 | Status: 0 applied, 1 stale base, 2 invalid, 3 resource exhausted |
| 25 | 7 | Reserved, zero |

An applied acknowledgment confirms atomic publication, not that every worker has
begun using the version. A worker reports its actual processing version in each
completion. Current version may already be higher if another publisher advanced it.

The library provides framing and blocking transport on a caller-owned Unix socket.
An embedding process supplies listeners, file permissions, peer checks, deadlines,
and reconnect policy. Close after malformed framing, EOF, or partial transfer error.
Neither malformed input nor a failed batch may alter the published snapshot.
