# truce-vst2

VST2 format wrapper for the truce audio plugin framework.

## Overview

Bridges a truce `PluginExport` implementation to the legacy VST2 plugin API.
Uses a clean-room C shim that implements the `AEffect` interface without
including any Steinberg SDK headers, avoiding licensing issues entirely. All
plugin logic is delegated to Rust via C FFI callbacks.

## What it handles

- `AEffect` struct initialization and host callback
- Audio processing (replacing and accumulating modes)
- Parameter get/set and display formatting
- Editor window open/close and idle
- State (chunk) save and restore
- Plugin category and I/O configuration reporting

VST2 has one flat input and output pin list, not runtime audio buses. Truce
preserves declared main/sidechain channel ranges in
`ProcessContext::bus_routing`, but reports their activation as `Unknown`
instead of guessing from samples or host-provided storage.

## Architecture

The C shim (compiled via `cc`) implements the `AEffect` dispatcher and
forwards opcodes to Rust through a C FFI boundary. No Steinberg SDK code is
linked or referenced.

Part of [truce](https://github.com/truce-audio/truce). [Docs](https://truce.audio/docs/).
