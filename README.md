# CypherClaw

[![CI](https://github.com/atiti/espclaw/actions/workflows/ci.yml/badge.svg)](https://github.com/atiti/espclaw/actions/workflows/ci.yml)
[![Release](https://github.com/atiti/espclaw/actions/workflows/release.yml/badge.svg)](https://github.com/atiti/espclaw/actions/workflows/release.yml)
[![Site Build](https://github.com/atiti/espclaw/actions/workflows/pages.yml/badge.svg)](https://github.com/atiti/espclaw/actions/workflows/pages.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-ffd866.svg)](LICENSE)
[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-5.5.2-ef4444.svg)](https://github.com/espressif/esp-idf)
[![Boards](https://img.shields.io/badge/boards-ESP32%20%7C%20ESP32--S3-38bdf8.svg)](docs/support-matrix.md)

> Bring the agent closer to the hardware.
>
> **Rebrand note:** the codebase, package names, and device/tool namespaces may still use `espclaw` while the product identity moves to **CypherClaw**.

CypherClaw is an ESP32-native agent runtime for boards with real sensors, real I/O, and real constraints.
It combines:

- an iterative LLM tool loop
- a local workspace and admin UI
- installable Lua apps and reusable Lua components
- persistent tasks, behaviors, and events
- OTA-capable firmware for supported boards

The goal is simple: make an embedded board feel more like a programmable runtime at the hardware boundary than a fixed-function firmware image.

## What's New In CypherClaw

- **Unified operator surfaces**: UART, web admin, simulator, and Telegram now share the same slash-command + agent flow for more predictable operations.
- **Reusable app architecture**: installable Lua components, app manifests, persisted behaviors, and local events are now first-class and model-accessible.
- **Large artifact pipelines**: chunked blob upload plus file/blob/url installs make it practical to ship large Lua apps and markdown context on-device.
- **Better OTA resilience**: rollback-aware OTA behavior and delayed confirmation improve recovery safety on constrained boards like `esp32cam`.
- **Richer web experience**: the public site now includes a browser flasher and WebAssembly browser lab backed by the real runtime core.

## Flash It

- Browser landing page, flasher, and WebAssembly browser lab: [espclaw.dev](https://espclaw.dev/)
- Latest binaries: [GitHub Releases](https://github.com/atiti/espclaw/releases/latest)
- Manual flashing and OTA notes: [docs/ota.md](docs/ota.md)

## Why CypherClaw

Most LLM agents assume a server, a filesystem, and effectively unbounded memory.
CypherClaw assumes:

- a microcontroller
- tight RAM budgets
- real GPIO, I2C, PWM, camera, Wi-Fi, and storage
- an operator who may be using a browser, UART console, simulator, or Telegram chat

That leads to a different design:

- remote models, local tool execution
- hot-swappable Lua logic instead of reflashing for every iteration
- explicit board profiles and hardware capability discovery
- chunked data paths for large markdown and Lua artifacts
- in-memory log tails over HTTP and tool calls for debugging without serial
- recoverable, inspectable, operator-visible runtime behavior

## Core Ideas

### 1. Apps are features
An app is an installable Lua bundle under `/workspace/apps/<app_id>/`.

Use apps for:
- weather stations
- camera automations
- robot behaviors
- diagnostics

### 2. Components are reusable building blocks
A component is a reusable Lua module with metadata, installable independently from apps and published into `/workspace/lib`.

Use components for:
- sensor drivers
- protocol helpers
- control math
- shared business logic

### 3. Tasks are live execution
A task is a running instance of an app.

Use tasks for:
- periodic loops
- event-driven handlers
- “run this now” automation

### 4. Behaviors are persisted task definitions
A behavior is a saved task configuration that can autostart after reboot.

Use behaviors for:
- always-on monitoring
- boot-time automation
- persistent schedules

### 5. Events are local signals
Events connect producers and consumers without hard-wiring everything together.

Use events for:
- sensor fan-out
- UART-triggered flows
- hardware watches
- decoupled app coordination

## Architecture

```text
                +----------------------+
                |  Telegram / Web /    |
                |  UART / Simulator    |
                +----------+-----------+
                           |
                           v
                +----------------------+
                |   Agent Loop         |
                |   tool schemas       |
                |   iterative runs     |
                |   safety / yolo      |
                +----------+-----------+
                           |
            +--------------+--------------+
            |                             |
            v                             v
 +----------------------+      +----------------------+
 |   Workspace          |      |   Tool Runtime       |
 |   apps               |      |   fs / wifi / gpio   |
 |   components         |      |   i2c / camera / ota |
 |   sessions / memory  |      |   app / task / event |
 +----------+-----------+      +----------+-----------+
            |                             |
            +--------------+--------------+
                           |
                           v
                +----------------------+
                |   ESP32 Hardware     |
                +----------------------+
```

## What Works Today

CypherClaw is already usable for real local operator workflows.

Working areas:
- admin UI served directly from the device
- UART serial chat with slash commands
- simulator with the same app/chat/runtime model
- Lua apps, components, tasks, events, and behaviors
- OTA updates on supported boards
- chunked blob upload and install-from-file/blob/url flows
- web search and fetch tools
- Telegram bot control and photo replies on camera boards

See the full [support matrix](docs/support-matrix.md) for target-by-target status.

## Supported Boards

Primary targets:
- `esp32s3`
- AI Thinker `esp32cam`

Design bias:
- PSRAM-capable boards first
- SD-backed workspace where available
- OTA-safe partitioning on supported builds

Board configuration is explicit and board-aware. Apps can use named hardware aliases through `espclaw.board.*` instead of hard-coding raw pins.

See:
- [Support Matrix](docs/support-matrix.md)
- [Board Configuration](docs/board-config.md)

## Operator Surfaces

CypherClaw exposes one shared operator model across:

- admin web UI
- UART console
- simulator console
- Telegram

Slash commands are available on UART and the shared console path:

- `/help`
- `/status`
- `/tools`
- `/tool <name> [json]`
- `/wifi status`
- `/wifi scan`
- `/wifi join <ssid> [password]`
- `/telegram status`
- `/telegram token <token>`
- `/telegram poll <seconds>`
- `/telegram enable`
- `/telegram disable`
- `/telegram clear-token`
- `/yolo status`
- `/yolo on`
- `/yolo off`
- `/reboot`
- `/factory-reset`

Everything else becomes a normal iterative LLM turn.

## Quick Start

### Simulator

Build and run the host simulator:

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
./build/espclaw_simulator --workspace /tmp/espclaw-dev --port 8080 --profile esp32s3
```

Then open:

```text
http://127.0.0.1:8080/
```

This is the fastest way to explore:
- the admin UI
- the shared chat path
- app install/run flows
- components
- chunked blob uploads

### Firmware

Bootstrap a repo-local ESP-IDF:

```bash
mkdir -p .deps
git clone -b v5.5.2 --recursive https://github.com/espressif/esp-idf.git .deps/esp-idf-v5.5.2
IDF_TOOLS_PATH=$PWD/.deps/.espressif ./.deps/esp-idf-v5.5.2/install.sh esp32s3,esp32
source scripts/use-idf.sh
```

Build for `esp32s3`:

```bash
cd firmware
idf.py -B build-esp32s3 set-target esp32s3
idf.py -B build-esp32s3 build
```

Build for `esp32cam` / classic `esp32`:

```bash
cd firmware
idf.py -B build-esp32 set-target esp32
idf.py -B build-esp32 build
```

For onboarding and runtime setup, see [docs/onboarding.md](docs/onboarding.md).

## Reusable Components

CypherClaw has a first-class reusable component model.

A component can be:
- installed from inline source
- installed from a workspace file
- installed from a committed blob
- installed from a URL
- installed from a manifest

That means the intended open-source reuse shape is:

```text
component -> shared driver/helper/module
app       -> end-user feature
task      -> live schedule
behavior  -> persisted schedule
event     -> decoupled signal
```

A concrete example:

- `ms5611_driver` component
- `weather_station` app using `require("sensors.ms5611")`
- `paragliding_vario` app using the same component

See:
- [Components](docs/components.md)
- [Component Registry Manifests](docs/component-registry.md)
- [App Patterns](docs/app-patterns.md)

## Large Artifacts And Context

CypherClaw has chunk-aware flows for content that does not fit comfortably in one request buffer.

Supported patterns:
- chunked blob upload into the workspace
- app install from file/blob/url
- component install from file/blob/url/manifest
- bounded context selection and summarization for large markdown files

Key tools and APIs:
- `context.chunks`
- `context.load`
- `context.search`
- `context.select`
- `context.summarize`
- `/api/blobs/*`

See [docs/blob-transfer.md](docs/blob-transfer.md).

## Security And Privacy

CypherClaw is a networked embedded system, not a toy shell script.
If you run it on hardware, treat it like software that can:

- connect to cloud providers
- store auth credentials
- talk to Telegram
- fetch remote URLs
- modify local device state

Start here:
- [Security Policy](SECURITY.md)
- [Security And Privacy Notes](docs/security-and-privacy.md)

## Documentation Map

Start here:
- [Onboarding](docs/onboarding.md)
- [Support Matrix](docs/support-matrix.md)
- [Architecture](docs/architecture.md)
- [Simulator](docs/simulator.md)

Runtime model:
- [Apps](docs/apps.md)
- [Components](docs/components.md)
- [App Patterns](docs/app-patterns.md)
- [Events](docs/events.md)
- [Lua API](docs/lua-api.md)
- [Workspace Memory](docs/workspace-memory.md)

Operator surfaces:
- [Admin UI](docs/admin-ui.md)
- [Console Chat](docs/console-chat.md)
- [Web Tools](docs/web-tools.md)
- [OTA](docs/ota.md)

Hardware and validation:
- [Board Configuration](docs/board-config.md)
- [Hardware Lua](docs/hardware-lua.md)
- [Vehicle Control](docs/vehicle-control.md)
- [Real Hardware Bench](docs/real-hardware-bench.md)

Project docs:
- [Contributing](CONTRIBUTING.md)
- [Release Process](docs/release-process.md)
- [Web Flasher](docs/web-flasher.md)
- [Code of Conduct](CODE_OF_CONDUCT.md)

## Development

Host test loop:

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Real-device bench:

```bash
python3 scripts/real_device_bench.py --device http://<device-ip>:8080
```

Releases:
- GitHub Actions builds host artifacts and firmware artifacts
- tagged `v*` pushes publish GitHub Releases
- Cloudflare Pages serves the public site at `espclaw.dev`
- the `pages.yml` workflow builds and validates the static site + WASM runtime bundle used for deployment

## Open Source Contract

CypherClaw is MIT-licensed and intended to be hackable.

What that means in practice:
- Lua apps and components are expected to be edited and shared
- simulator and firmware flows should stay aligned
- CI should prove both host and firmware builds
- the repo should be operable from a fresh checkout

If you build something cool with it, the most useful contribution is usually one of:
- a reusable component
- a board definition
- a polished example app
- a reproducible bug report with board/runtime context

## Contributing

Please read:
- [CONTRIBUTING.md](CONTRIBUTING.md)
- [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md)
- [SECURITY.md](SECURITY.md)

Pull requests that improve clarity, reproducibility, hardware support, and operator ergonomics are especially valuable.
