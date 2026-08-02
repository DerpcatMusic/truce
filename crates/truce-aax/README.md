# truce-aax

AAX format wrapper for the truce audio plugin framework.

## Overview

Bridges a truce `PluginExport` implementation to the Avid AAX plugin API used
by Pro Tools. Instead of linking against the AAX SDK, the Rust code exports C
ABI functions that a pre-built AAX template binary loads at runtime via
`dlopen`. This means no AAX SDK dependency exists in the Rust source.

User plugins opt into AAX by adding `truce-aax = { workspace = true,
optional = true }` and gating it behind an `aax` Cargo feature. Building
the actual `.aaxplugin` bundle (which invokes the AAX template build, PACE
wrapping, and codesigning) is driven by `cargo truce build --aax` /
`install --aax` / `package --formats aax`.

## What it handles

- C ABI export functions matching the AAX template's expected interface
- Audio processing and parameter bridge
- State save/restore
- Custom native NSView rendering on macOS (avoids Pro Tools autorelease pool
  crashes that occur with standard compositor-based approaches)

AAX exposes one mono sidechain port. Truce maps it only to the plugin's first
declared auxiliary bus, duplicating the mono source across that bus's width.
Later declared auxiliary buses remain present in `ProcessContext::bus_routing`
as zero-width `Inactive` entries, so their `BusLayout` indices are never
collapsed or shifted. AAX cannot route those additional buses independently.

## Architecture

The pre-built AAX template binary implements the real AAX SDK interfaces and
loads the Rust dylib at runtime. The Rust side only knows about the C bridge
types defined in `truce_aax_bridge.h`, keeping the build free of proprietary
SDK dependencies.

Part of [truce](https://github.com/truce-audio/truce). [Docs](https://truce.audio/docs/).
