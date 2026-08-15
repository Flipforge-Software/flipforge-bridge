# Physical hardware test plan

No physical result is implied by this document. Record board revision, Flipper firmware, Bridge commit, timestamp, negotiated baud, and actual logs for every run.

## Setup

1. Build with ESP-IDF v5.5.5 for `esp32s2`.
2. Flash the official Wi-Fi Devboard using `idf.py` and the generated build metadata.
3. Reboot the Devboard and attach it to the Flipper.
4. On Flipper, enable **Settings → Expansion Modules → Listen UART** for LPUART.
5. With the board mounted and powered by the Flipper, connect USB-C and verify the TinyUSB CDC runtime port enumerates without rebooting either device.
6. Monitor the Devboard USB log without exposing its pairing response.

## Protocol proof

1. Confirm module detection at 9600 baud.
2. Confirm a BAUD RATE request is the first module frame.
3. Record accepted negotiated baud and 25 ms switch dead time.
4. Leave the connection idle for five minutes; verify heartbeats keep it alive.
5. Disable Listen UART; verify the proxy closes and only the Expansion layer performs at most three reconnect attempts.
6. Re-enable Listen UART and explicitly request reconnect/reset the Devboard; verify recovery without credential loss.

## Auth and client behavior

1. Pair through the physical USB procedure within the 120-second window.
2. Join the WPA2 SoftAP and authenticate with a Mac test client.
3. Verify wrong HMAC, old HMAC replay, malformed frame, oversized frame, and unsupported version are rejected.
4. While authenticated, connect a second client and verify it receives `Busy` and closes.
5. Leave an unauthenticated client idle for over 30 seconds and verify timeout.

## Standard RPC proof

Use a valid Flipper protobuf client over the raw proxy:

1. Send Flipper RPC ping.
2. Read device properties.
3. List `/ext`.
4. Launch `flipforge_core`.
5. Open App DataExchange and call Core `GetCapabilities`.
6. Transfer a controlled test file through normal Storage RPC and verify byte/hash equality.

The v0.1 acceptance proof is `GetCapabilities → success` through iPhone/Mac → Wi-Fi → Bridge → Expansion UART → Flipper RPC → Flipforge Core.

## Reliability and measurement

1. Repeat TCP connect/auth/proxy/disconnect 50 times.
2. Disconnect the iPhone during traffic; verify STOP RPC or bounded link cleanup and a usable next session.
3. Remove the Flipper during traffic; verify TCP closes, SoftAP remains, and retries are bounded.
4. Test each preferred baud by temporarily changing the centralized list and record errors/throughput.
5. Run one large valid RPC transfer and a many-small-operation workload.
6. Record actual bytes, duration, effective KiB/s, ACK mean, queue peak, retries, and errors.
7. Inspect for secret/raw-payload leakage in logs.

Do not publish a performance claim until this plan has measurements from physical hardware.
