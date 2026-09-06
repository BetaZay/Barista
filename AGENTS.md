# Repository Guidelines

## Project Structure & Module Organization

Barista is a C++20/CMake project that connects a Wii U GamePad to a desktop application. `app/` contains the Qt 6 GUI and D-Bus service; `core/` holds portable shared interfaces. The Linux radio and streaming stack lives in `drcd/`, `drcctl/`, `libdrc-host/`, and `libdrc-ipc/`, each with public headers in `include/`, implementation in `src/`, and tests in `tests/` where applicable. Linux integration files are in `platform/linux/` and `packaging/`. Keep changes to bundled upstream code in `third_party/` deliberate and isolated. Utility and hardware-analysis scripts live in `scripts/`.

## Build, Test, and Development Commands

Use the documented Ninja build flow (see `COMPILING.md`):

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

For UI or portable-core work, avoid Linux radio dependencies with `-DBARISTA_BUILD_ENGINE=OFF` and a separate build directory such as `build-desktop`. Build release packages with a Release configuration and run `cpack --config build/CPackConfig.cmake -G DEB -B dist` (or `RPM`).

## Coding Style & Naming Conventions

Write C++20 with four-space indentation, braces on their own lines, and no required compiler extensions. Follow nearby code: `PascalCase` for types and public methods, `camelCase` for local variables and parameters, and `m_`-prefixed instance members. Name implementation and test files after the unit they cover, e.g. `single_instance.cpp` and `single_instance_test.cpp`. No repository-wide formatter or linter is configured; preserve the style of the edited file and avoid unrelated reformatting.

## Testing Guidelines

CTest drives the C++ test executables. Add focused tests alongside the relevant module, register them with `add_test`, and use descriptive names such as `drcd_packet_schedule_tests`. Run the full CTest suite after changes; for GUI work, include the existing smoke tests. Hardware/radio scripts are supplementary and must not replace deterministic unit coverage.

## Commit & Pull Request Guidelines

Recent history uses short imperative subjects, commonly conventional prefixes: `feat:`, `fix:`, `refactor:`, `docs:`, `packaging:`, and `ui:`. Keep commits narrowly scoped. Pull requests should explain the behavioral change, list verification commands, link related issues when present, and include screenshots for GUI changes. Call out hardware, privilege, service, or Wi-Fi-adapter testing explicitly.

## Security & Configuration

The GUI runs unprivileged while the installed service owns privileged radio work. Never add a workflow that launches the GUI via `sudo` or `pkexec`; keep AppHook socket permissions scoped to the desktop user.
