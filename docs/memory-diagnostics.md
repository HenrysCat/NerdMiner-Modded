# RAM diagnostics and mail-task cleanup

This follow-up addresses mail delivery deferred by low free heap or a small
contiguous allocation. It adds measurements and a targeted scheduler fix; it
does not establish that every possible long-term memory issue is resolved.

## Measurements

`GET /api/memory-diagnostics` is a read-only LAN endpoint, like the existing
operational diagnostic endpoints. It contains no credentials, addresses or HTTP
response bodies. It reports free 8-bit heap, the largest free block and the
since-boot minimum, in bytes. The current sample is taken before allocating its
JSON response. Other concurrent requests can still affect that measurement.

A fixed ring holds 32 samples (768 bytes on the tested classic ESP32 build;
reported as `ring_bytes`). Baselines are taken once a minute. HTTP begin/end
points span local client/response lifetimes; the global network operation also
contains fee-specific points. Mail begin/end span transport work, while
`mail_settled` is sampled 30 seconds after the Arduino loop processes completion.
The worker task may still exist at `mail_end`.

The ring is bounded and never persisted to flash. The JSON response allocates
approximately 6.5 KiB on demand, plus HTTP overhead. This is a short diagnostic
window, not a multi-day archive. Overlapping operations mean a pair's difference
alone is not proof of a leak. The minimum remains low after a transient peak;
it is not the amount currently free.

## HTTP allocation pressure

JSON is parsed directly from a bounded stream into the existing fixed-size
ArduinoJson documents, retaining pool filters. Full response String copies and
full HTTP error-body logging are removed. Reading is capped at 32,768 bytes,
with a five-second read budget and up to two additional seconds for an individual
Stream timeout. Invalid, oversized or unsupported encoded responses follow the
existing failure/backoff paths rather than update statistics from invalid JSON.
The plain-text height response is limited to 16 characters and validated.

Requests use HTTP/1.0, connection-close framing and identity encoding. Raw
HTTPClient streams do not decode chunking, so transfer-encoded or compressed
responses are rejected. APIs that ignore these request preferences may therefore
stop updating. Broader server compatibility remains to be tested. TLS body reads
also hold the existing hardware-SHA mutex; the Stratum path is unchanged.

## Reclaiming terminated task stacks

Each mail creates a task with a 12,288-byte stack and then self-deletes. FreeRTOS
defers reclamation to IDLE. The always-ready priority-1 software miner could
starve IDLE on its core. A one-tick blocking delay after every eight software
chunks, outside shared locks, gives lower-priority work an opportunity to run.
`taskYIELD()` alone would not do this. The hardware loop is unchanged. The
software miner gives up a small amount of execution time.

Local evidence before this fix: a completed mail left roughly 13 KiB less free
heap after the 30-second settling period. After this fix, the first unit's two
consecutive mail tests settled at 94,868 and 94,792 free bytes; the second unit's
test settled at 94,864 bytes. Largest blocks recovered to approximately 48 and
52 KiB. Receipt was confirmed by the operator, and sampled total mining rates
were approximately 985–990 KH/s. These observations support delayed task-stack
reclamation as the cause of that repeated loss, not a general leak-free claim.

The tested units ran the local V1.5.1-based development firmware. This PR ports
the changes onto its existing current-upstream contribution; that exact port
requires its own hardware regression. Multi-day heap stability, unattended daily
reports after this fix, other boards and low-margin TLS operation remain open.
Observed transient minima during TLS were only about 2 KiB on the test devices;
successful delivery does not establish comfortable peak-memory headroom.

The mail guard remains unchanged (70,000 free bytes and a 40,000-byte largest
block). No automatic reboot, guard reduction, certificate-verification change,
new mail trigger or Wi-Fi disruption is introduced.
