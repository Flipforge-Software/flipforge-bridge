# Flipforge Bridge protocol v1

## Connection

- Join the WPA2 SoftAP advertised as `Flipforge-XXXX`.
- Connect TCP to the SoftAP gateway, normally `192.168.4.1`, port `4242`.
- Only one client is accepted. A second receives a `Busy` response and is closed.
- Management I/O has a 5-second socket timeout and a 30-second idle timeout.

## Management frame

All integer fields are little-endian. TCP segmentation is arbitrary; clients must buffer until the complete 14-byte header and declared payload are present.

| Offset | Size | Field |
|---:|---:|---|
| 0 | 4 | ASCII magic `FBRG` |
| 4 | 1 | Protocol version (`1`) |
| 5 | 1 | Command |
| 6 | 1 | Flags; bit 0 means response |
| 7 | 1 | Status; requests use `0` |
| 8 | 4 | Request ID |
| 12 | 2 | Payload length, maximum 512 |
| 14 | N | Payload |

Commands:

| Value | Command | Request payload | Response payload |
|---:|---|---|---|
| 1 | Hello | 32-byte client nonce | 32-byte Bridge nonce + 8-byte session ID |
| 2 | Authenticate | 32-byte HMAC-SHA256 | Empty |
| 3 | GetBridgeInfo | Empty | UTF-8 JSON safe metadata |
| 4 | GetStatus | Empty | UTF-8 JSON counters/state |
| 5 | BeginRPCProxy | Empty | Empty, then raw mode |
| 6 | EndRPCProxy | Empty | Empty; idempotent in management mode |
| 7 | Ping | Up to 512 bytes | Same bytes |

Statuses: `0 Ok`, `1 Malformed`, `2 UnsupportedProtocol`, `3 Unauthorized`, `4 Busy`, `5 Unavailable`, `6 TooLarge`, `7 InvalidState`.

## Authentication

The client generates a cryptographically random 32-byte nonce. Bridge answers `Hello` with a fresh random 32-byte nonce and random 8-byte session ID.

The HMAC input is the exact byte concatenation:

```text
ASCII("Flipforge Bridge Auth v1")
|| UInt8(1)
|| client_nonce[32]
|| bridge_nonce[32]
|| session_id[8]
```

The key is the 32-byte pairing secret and the algorithm is HMAC-SHA256. Nonces and session ID must be treated as opaque bytes. A failed authentication closes the connection; authentication cannot be replayed within the same session state.

## Raw RPC mode

After an authenticated `BeginRPCProxy` receives `Ok`, every following TCP byte is part of the normal Flipper protobuf RPC stream. No management framing is valid in raw mode.

Bridge preserves byte order and content, splits TCP input into official Expansion DATA frames of at most 64 bytes, waits for each required STATUS, and applies backpressure. Flipper DATA is acknowledged and streamed to TCP with partial-send handling.

Close TCP to end raw mode. Bridge then sends official STOP RPC when the Expansion link is still available, discards the temporary authenticated session, and remains available for a new authenticated client.

If the Flipper Expansion link fails, Bridge closes the raw TCP session and performs bounded Expansion-only recovery. It does not restart Wi-Fi or erase pairing.

## iOS shape

```swift
protocol FlipperRPCByteTransport {
    func connect() async throws
    func write(_ bytes: Data) async throws
    func read() async throws -> Data
    func close() async
}
```

`BridgeRPCTransport` should own Wi-Fi/TCP management and expose only raw reads/writes after authentication. The existing protobuf `FlipperRPCClient` remains above this interface alongside the existing BLE transport.
