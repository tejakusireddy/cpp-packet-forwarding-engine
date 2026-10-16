#pragma once

#include "forwarding/control.hpp"

namespace forwarding::ipc {

inline constexpr std::size_t max_message_size = 1024 * 1024;
inline constexpr std::size_t max_routes = 32768;
inline constexpr std::size_t max_groups = 1024;
inline constexpr std::size_t max_members = 256;

// Version-one network-byte-order format. Decoding validates framing/counts;
// routing semantics are validated by ControlPlane before publication.
[[nodiscard]] std::vector<std::uint8_t> encode(const UpdateBatch& batch);
[[nodiscard]] UpdateBatch decode(std::span<const std::uint8_t> message);

// Blocking Unix stream transport. Retries EINTR and short reads/writes. Throws
// system_error for I/O and invalid_argument for malformed/oversized messages.
// One writer and one reader per direction. Caller owns the socket and timeout.
// On an error, close the connection; a partial message cannot be retried safely.
void send_batch(int socket, const UpdateBatch& batch);
[[nodiscard]] UpdateBatch receive_batch(int socket);
void send_ack(int socket, const UpdateAck& ack);
[[nodiscard]] UpdateAck receive_ack(int socket);

} // namespace forwarding::ipc
