# CI: what runs and why

The Linux/macOS/Windows workflows retain their existing check names and all
formatting, Clippy, builds, tests, screenshot comparisons, SIMD performance
checks, plugin validators, and install/package round trips. iOS, Miri, fuzzing,
rust-analyzer, CLI compatibility, security, and docs workflows remain separate.
Nothing in this change enables deployment, changes repository rules, or adds
paid runners.

## Keep caches useful

Each platform job now includes the nested Cargo workspaces it builds in the
Rust cache. A root `cargo test --workspace` does not include
`crates/truce-slint`, `crates/truce-vizia`, or `crates/truce-gpu-examples`, and the
root `target/` cache does not include their `target/` directories either. The
explicit mappings make the expensive Skia/Vizia dependency caches available on
later runs. Toolchain selection remains before cache restoration; keys remain
separate by platform and purpose. Rustup proxies are excluded. Only `main`
writes caches so PR-specific copies do not evict the useful main entries.

Stale PR runs are cancelled; main-branch runs finish. Formatting is bounded to
10 minutes, the window smoke to 30, and full cold platform jobs to 90 minutes.
These limits stop a hung process from consuming the default six hours. They
are ceilings, not target performance or reasons to ignore failures.

## Reproduce the core gates

```sh
cargo fmt --all -- --check
cargo clippy --workspace --all-targets -- -D warnings
cargo build --workspace --all-targets
cargo test --workspace -- --skip gui_screenshot
cargo test --workspace --exclude truce-example-eq -- gui_screenshot
cargo test -p truce-example-eq -- gui_screenshot
cargo test --release -p truce-simd --test gain_perf_regression -- --ignored --nocapture
```

Repeat the manifest-scoped commands from the platform workflow for each nested
workspace. Read that workflow for native packages, validators and platform
packaging requirements. CPU and GPU screenshot passes stay separate to avoid
feature unification changing the rendering backend. Full builds are retained:
later steps execute `target/debug/cargo-truce` and validate real plugin bundles,
so replacing them with a compile-only check would lose coverage or break the
validation pipeline. Clippy catches code-quality mistakes, tests check behavior,
validators check plugin-format contracts, and the ratio guard catches large SIMD
regressions. None proves real-host sound or editor behavior; see CONTRIBUTING.md
for the required manual DAW testing matrix.

This patch does not claim the currently inherited source passes every gate.
Failures in existing tests or dependencies must be fixed explicitly, never
hidden by deleting/skipping checks. Required check names are unchanged; branch
protection and merge-queue configuration remain owner-managed. This repository
currently has no merge-group triggers, so do not enable a queue without adding
its required checks together.
