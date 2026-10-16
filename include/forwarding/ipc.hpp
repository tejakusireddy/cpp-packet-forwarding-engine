#pragma once

#include "forwarding/control.hpp"

#include <chrono>
#include <stop_token>

namespace forwarding::ipc {

inline constexpr std::size_t max_message_size = 1024 * 1024;
inline constexpr std::size_t max_routes = 32768;
inline constexpr std::size_t max_groups = 1024;
inline constexpr std::size_t max_members = 256;

struct IoOptions {
    std::chrono::milliseconds timeout{5000};
    std::stop_token stop;
};

// Version-one network-byte-order format. Decoding validates framing/counts;
// routing semantics are validated by ControlPlane before publication.
[[nodiscard]] std::vector<std::uint8_t> encode(const UpdateBatch& batch);
[[nodiscard]] UpdateBatch decode(std::span<const std::uint8_t> message);

// Blocking Unix stream transport. Retries EINTR and short reads/writes. Throws
// system_error for I/O and invalid_argument for malformed/oversized messages.
// One writer and one reader per direction. Each message has one absolute I/O
// deadline, including its prefix and payload. Stop cancels within a 20ms poll.
// On an error, close the connection; a partial message cannot be retried safely.
void send_batch(int socket, const UpdateBatch& batch, IoOptions options = {});
[[nodiscard]] UpdateBatch receive_batch(int socket, IoOptions options = {});
void send_ack(int socket, const UpdateAck& ack, IoOptions options = {});
[[nodiscard]] UpdateAck receive_ack(int socket, IoOptions options = {});

} // namespace forwarding::ipc
