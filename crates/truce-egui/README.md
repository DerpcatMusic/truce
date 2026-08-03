# truce-egui

## Outbound file drag

On Linux, an editor UI can call `truce_egui::external_drag::request_file()`
with an absolute, already-rendered export path. The baseview event loop starts
the Matari XDND session only while the initiating primary-pointer gesture is
still owned by that editor; `drain_events()` exposes the native lifecycle.

The current baseview backend has no AppKit/OLE outbound adapter and no native
Wayland event-authority path. Those targets intentionally do not expose this
module until their event loops can provide the same initiating-event contract;
there is no parent-window or timer fallback.

egui GUI backend for truce audio plugins.

## Overview

Provides `EguiEditor`, an implementation of `truce_core::Editor` that renders
using [egui](https://github.com/emilk/egui)'s immediate-mode UI via
egui-wgpu. This gives plugin developers full access to egui's widget library,
layout system, and ecosystem while retaining truce's parameter binding and
host integration. Supports custom fonts and themes.

Use this backend when you want fine-grained control over your plugin's UI
using egui's immediate-mode paradigm.

## Key types

- **`EguiEditor`** -- the `Editor` implementation
- **`EditorUi`** -- trait you implement to define your plugin's UI
- **`PluginContext`** -- parameter bridge for reading/writing truce
  params from egui widgets (re-exported from `truce-core`)

## Usage

```rust
struct MyUi;

impl<P: Params> EditorUi<P> for MyUi {
    fn ui(&mut self, ui: &mut egui::Ui, state: &PluginContext<P>) {
        // bind widgets via the PluginContext
    }
}
```

Part of [truce](https://github.com/truce-audio/truce). [Docs](https://truce.audio/docs/).
