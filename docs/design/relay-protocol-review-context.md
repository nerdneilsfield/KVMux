# Relay ASCII Paste: Review Context

**Review checkpoint:** `107b4d5 feat(relay): add authorized chunked ASCII paste flow`
**Scope:** unreleased relay protocol v4 and remote US-ASCII paste.

## Function

The App accepts printable US-ASCII, Tab, and LF. It normalizes and validates the complete text before sending it. A local serial connection submits one `AsciiPasteJob` to the CH9329 worker. A remote connection uploads one transaction to the relay. The relay maps the validated text to an `AsciiPasteJob` and the serial worker sends one HID report per CH9329 ACK.

The old remote path sent individual key edges from the App. Long text stalled because App-level pacing did not own CH9329 ACK timing. The current code uses a relay-owned transaction.

## Current protocol

```text
PasteBegin(tx, byte_count, crc32)
PasteChunk(tx, index, payload)*
PasteAuthorize(tx, challenge, request_id)
PasteAuthorized(tx, request_id, token)
PasteCommit(tx, token)
PasteStatus(uploading | complete | executing | completed | canceled | rejected | expired)
PasteCancel(tx, reason)
PasteKeepalive(tx)
```

Upload size is bounded to 65,536 bytes. Chunk payload is at most 960 bytes. Upload state is held in memory. The server validates chunk order, size, CRC-32, and supported ASCII before execution.

`PasteAuthorize` requires a current raw-video proof. The relay submits a private serial synchronization fence and waits for its applied snapshot. The private fence uses a high-bit revision and is separate from normal GUI `Sync` / `StateAck` revision state. The server returns a one-time token in `PasteAuthorized`. `PasteCommit` must carry that token before the relay starts the serial job.

## Current ownership

| Component | Current role |
| --- | --- |
| `InputRouter` / `KvmSession` | Starts text from the GUI, controls local input intent, releases on normal lifecycle events. |
| `RelayClient` | Uploads chunks, requests authorization, commits the token, exposes paste status. |
| `RelayServer` / `ServerSession` | Validates upload/proof/token, owns authorization and transaction state, starts/cancels relay serial work. |
| `Ch9329ControlSink` / serial worker | Owns `AsciiPasteJob` execution and waits for every CH9329 ACK. |
| KCP | Carries reliable control messages. Raw UDP carries media proof messages. |

## Safety facts

- No serial job starts before complete upload, CRC/ASCII validation, proof, private fence, and token commit.
- Host, focus loss, minimize, explicit cancel, disconnect, epoch/intent change, serial fault, video stale, capture generation change, or actual capture-mode change cancel text and release input.
- The implementation must not log pasted text, HID payload, or file paths.
- KCP output is bounded. The relay/client retain a held UDP batch across `would_block` and do not call `KcpChannel::update()` before that batch drains.
- Server progress `PasteStatus` messages are coalesced.

## Observed implementation/debug facts

1. The private authorization fence has applied without modifying the ordinary GUI revision / `StateAck` namespace.
2. The server now processes decoded KCP controls and the serial snapshot before applying transaction deadline checks in its loop.
3. A raw proof may refer to a frame that is still being paced. Pending proof state records its receipt time. After the frame becomes eligible, proof validation uses that recorded receipt time; a proof received after its challenge window remains invalid.
4. The transaction has been observed to reach upload, authorization, private fence, token commit, serial execution, serial progress, and completed status.
5. During some timing-sensitive integration runs, a client has observed `completed` and later observed `expired/deadline` for the same transaction id. The current client status mapper allows later same-transaction statuses to overwrite a terminal snapshot.
6. Integration tests have also shown timing-sensitive raw proof/control readiness failures. Video frames can continue while client control snapshot becomes `stalled` when its raw challenge/proof freshness condition expires.
7. A release followed by text paste can require a new control synchronization and active raw proof for the new intent. This path has been timing-sensitive in integration tests.
8. Tests include relay wire/session/server/client coverage, KCP `would_block` coverage, a 961-byte two-chunk paste scenario, serial HID report counting, and relative-mouse behavior after paste.

## Relevant files

- `src/network/relay_wire.{hpp,cpp}`
- `src/network/relay_session.{hpp,cpp}`
- `src/network/relay_server.cpp`
- `src/network/relay_client.{hpp,cpp}`
- `src/network/kcp_channel.{hpp,cpp}`
- `src/control/ascii_paste_job.hpp`
- `src/control/serial_worker.cpp`
- `src/input/input_router.cpp`
- `src/app/kvm_session.cpp`
- `tests/relay_wire_test.cpp`
- `tests/relay_session_test.cpp`
- `tests/relay_server_test.cpp`
- `tests/relay_client_test.cpp`
- `tests/udp_kcp_test.cpp`
