# include/ — vendored protocol headers

This directory holds the **byte-for-byte vendored copies** of the OpenDisplay
wire-protocol headers. Their canonical source lives in the `opendisplay-protocol`
repo:

| File | Canonical source |
|------|------------------|
| `opendisplay_protocol.h` | `opendisplay-protocol/src/opendisplay_protocol.h` |
| `opendisplay_structs.h`  | `opendisplay-protocol/src/opendisplay_structs.h`  |

**Do not hand-edit these files.** Edit the canonical source, then propagate with
`opendisplay-protocol/tools/sync_protocol_header.py --push` and verify with
`--check`. Keeping them isolated in `include/` (instead of alongside the
firmware-owned sources at the repo root) makes that "managed, do not touch"
boundary explicit.

Firmware-local headers (`opendisplay_constants.h`, `opendisplay_runtime.h`,
`opendisplay_config_parser.h`, …) stay at the repo root — they are owned by this
firmware, not by the protocol repo.
