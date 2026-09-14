# Local ESP HTTPS OTA component

Origin: Espressif ESP-IDF v6.0.2, commit
`7101770dc6db2667b3c477cc31365dd1acd6db4e`, directory
[`components/esp_https_ota`](https://github.com/espressif/esp-idf/tree/v6.0.2/components/esp_https_ota).
The original component was copied from an unchanged checkout of that commit.
All five original component files are included. `LICENSE` is the unchanged
Apache-2.0 license from the root of the same ESP-IDF checkout; original source
copyright and SPDX notices are preserved. No global ESP-IDF file is modified.

`UPSTREAM_SHA256.json` records every original build/configuration/header/source
input and the license. `PATCHED_SHA256.json` records the corresponding current
files. Only `src/esp_https_ota.c` differs. The difference is completely represented
by `0001-return-header-transport-errors.patch`; reverse it with `patch -R -p1`
from this directory to recover the original source, then verify upstream hashes.

The patch returns `ESP_ERR_HTTP_EAGAIN` when reading the image's initial 1024-byte
header times out, instead of looping forever inside the SDK. Zero-length reads
or a completed response shorter than that header return
`ESP_ERR_HTTP_INCOMPLETE_DATA`. Other negative reads return
`ESP_ERR_HTTP_CONNECTION_CLOSED`. The description and perform APIs preserve these
transport codes; ordinary body reads distinguish incomplete EOF and connection
failure in the same way. Body EAGAIN continues to return IN_PROGRESS, as upstream.
Flash-write, erase, image validation and verification error handling is unchanged.

After a header error, the caller must abort the attempt and create a new handle.
Calling perform again on that handle would use its IN_PROGRESS state, without
re-reading and validating the incomplete header. The application owns bounded
retry/backoff, checkpoints and the total elapsed-time policy. No timer closes a
TLS context concurrently with an active read.

This change removes the repeated no-data spin. It is not an absolute wall-clock
deadline for every SDK operation: a single HTTP read can internally gather data
through multiple transport reads, and a sufficiently slow but continuing stream
can delay return. The application checks its deadline when SDK calls return.

`../../../tools/test_fw_ota_transport.py` extracts and compiles the exact
three affected functions with deterministic host I/O stubs. It reverses the patch
in a temporary directory, verifies original hashes, demonstrates original spins
using a test-only read-call ceiling, and checks patched results and successful
complete/fragmented reads. These are host control-flow tests, not hardware,
real-server, TLS, flash or end-to-end deadline evidence.
