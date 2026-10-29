# C++ Modules and mcpp

Use this reference for a project whose `mcpp.toml` depends on `huxerui.huxerui` and whose `build.mcpp` imports `huxerui.rules`. Generated projects use `import huxerui;` for the same public entities exposed by the umbrella header. Preserve the dependency version and build setup selected by the project.

The HuxerUI mcpp path supports Linux, Windows, and macOS. Android, iOS, and Web use CMake. Generated mcpp projects have no CMake platform shells; use `mcpp build` and `mcpp run` rather than `huxerui build`, `run`, or `package`.

For an existing project that manually links prebuilt SDK binaries instead of using the package rules, read [mcpp-build.md](mcpp-build.md).

## Project structure

```text
mcpp.toml       package, dependency, target, and language standard
build.mcpp      HuxerUI build-rule configuration
src/main.cpp    application entry
src/app.cppm    application module and composables
resources/     optional resource input
```

Create a package application with `huxerui create app <name> --build mcpp`; library creation accepts the same build selection. `--template` selects an mcpp package template, and `--platform` is not accepted for this path. Inspect the generated manifest rather than copying a fixed dependency version into an existing project.

The build program imports the HuxerUI rules:

```cpp
import mcpp;
import huxerui.rules;

int main() {
  return huxerui::rules::configure({.resources = "resources"}) ? 0 : 1;
}
```

Keep `[build] sources = []` when the rules own source discovery. They transform composables and add the resulting sources; also globbing the originals can link duplicate definitions. Preserve the generated binary target and its `main` path.

The rules compile and stage resource packages through `hrc`. The resource namespace defaults to a sanitized package name unless `resource_namespace` is supplied; do not assume the CMake application's `app` namespace or invent an automatically exported resource module. Inspect the selected rules and compiler outputs when resource access needs integration.

## Module code

```cpp
export module app;

import std;
import huxerui;

using namespace huxerui;

[[huxerui::composable]]
View Counter() {
  auto count = UseState(0);
  return Row {
    Button("Count").OnClick([count] { count += 1; }),
    Text(count),
  };
}
```

Use module imports rather than `#include` in the generated module application. Import `std` in units that instantiate HuxerUI templates or use standard-library entities; importing HuxerUI does not make all standard-library declarations reachable to the caller.

The current templates select C++23 for standard-library module/toolchain compatibility. Keep that generated selection unless the resolved toolchain supports a verified alternative; it does not change the framework's C++20 API baseline. A newer language mode alone does not solve binary compatibility with a prebuilt SDK.

Define annotated composables in source or module units processed by `huxerui.rules`. The application root already owns a scope. Keep `src/main.cpp` focused on required entry setup and `RunApplication()`; the rules exclude that entry file from transformation. Platform registration required before startup can still live in explicit entry code.

Macros do not cross module boundaries. Prefer `[[huxerui::composable]]`; when explicitly writing a scope, use the exported `Scope` and `View` APIs rather than assuming `HUXERUI_SCOPE` is imported.

## Build and verification

```bash
mcpp build
mcpp run
mcpp test
```

Reuse the project's selected toolchain. Do not install a fixed compiler version or change the user's default toolchain merely because this reference includes mcpp. Inspect the installed command help and package configuration when selecting targets, workspace builds, tests, or packaging.

Use the resolved package's public interface and user documentation for HuxerUI APIs. A source checkout is not required beside an application, and relative paths into the framework repository are not portable skill references.

For manifest or build-program syntax, consult the documentation matching the installed mcpp version:

- [mcpp documentation](https://github.com/mcpp-community/mcpp/tree/main/docs)
- [Manifest](https://github.com/mcpp-community/mcpp/blob/main/docs/04-mcpp-toml.md)
- [Dependencies](https://github.com/mcpp-community/mcpp/blob/main/docs/05-dependencies.md)
- [Testing](https://github.com/mcpp-community/mcpp/blob/main/docs/08-testing.md)
- [Build programs](https://github.com/mcpp-community/mcpp/blob/main/docs/30-build-mcpp.md)

The default-branch links are discovery entry points, not proof that an older installed version accepts the same syntax.
