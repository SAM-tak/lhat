# Your first host application

Build an application that owns its loop and calls `update(dt)` each frame.
Unlike running a script with the CLI, the available APIs are those registered by the host.

## 1. Choose how to link L^

Installing L^ is optional. Choose one of these approaches:

| Approach | Required files | Host build |
| --- | --- | --- |
| Build from source alongside the host | L^ source tree | Include it with CMake's `add_subdirectory` and build both together |
| Use a prebuilt SDK | Public headers and link libraries | Load the SDK with `find_package` and link its libraries |

With a prebuilt SDK, the host project does not need the L^ source tree.
L^ is not header-only: you must link libraries as well as include headers.
A static Windows build using the standard port links `lhat.lib` and `lhatport.lib`.

Application sources include `"lhat.h"`, but the include directory must contain both `lhat.h`
and the public headers under `lhat/`, including the generated `lhat/version.h`.
Private headers from `src/` are not needed.

## 2. Build the host

The sample supports both approaches with this CMake configuration:

<<< @/../sample/embedding/CMakeLists.txt{cmake}

### Building alongside L^ source

Prepare the [build environment](../guide/installation.md).
On Windows, first run `. ./scripts/devshell.ps1` in PowerShell.
Run the following from the L^ repository root. Replace `/absolute/path/to/lhat` with your checkout's absolute path.
Forward slashes also work in Windows paths.

```sh
cmake -S sample/embedding -B build/embedding -G Ninja -DCMAKE_BUILD_TYPE=Release -DLHAT_SOURCE_DIR="/absolute/path/to/lhat"
cmake --build build/embedding
```

Building the sample also builds L^.
You do not need a separate L^ build or a `cmake --install` step first.

### Using a prebuilt SDK

Leave `LHAT_SOURCE_DIR` unset and pass the SDK root as `CMAKE_PREFIX_PATH`.
Use separate build directories for the two approaches.

```sh
cmake -S sample/embedding -B build/embedding-sdk -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="/absolute/path/to/lhat-sdk"
cmake --build build/embedding-sdk
```

The package configuration under the SDK's `lib/cmake/lhat` provides `lhat::lhat` and `lhat::lhatport`.
Use an SDK with matching headers, libraries, CPU architecture, and build configuration.
For this approach, replace `build/embedding` with `build/embedding-sdk` in the execution paths below.

<details>
<summary>Building your own SDK</summary>

If you do not have an SDK, build L^ alongside your host, or create an SDK once.
Run these commands from the L^ repository root to collect headers, libraries, and CMake configuration in `build/docs-sdk`.

```sh
cmake --preset release
cmake --build --preset release
cmake --install build/release --prefix build/docs-sdk
```

This repository's release workflow packages the CLI and language server, rather than an SDK.
Do not confuse CLI release archives with SDKs.

</details>

## 3. Define the script entry point

<<< @/../sample/embedding/main.lh{text}

`module^demo` declares a module, and `public^let^update` exposes a callback.
The host registers the `app` module; `app.report` is not a built-in function.

After checking the source, the host verifies this contract:

```c
lhat_unit_export_conforms(unit, "update", "p^number^;")
```

A file passing type checking does not necessarily provide the entry points your application needs.
Check names and parameter types before putting a script into the frame loop.

## 4. Run the host

Windows:

```powershell
./build/embedding/embedding_host.exe sample/embedding/main.lh
```

Linux / macOS:

```sh
./build/embedding/embedding_host sample/embedding/main.lh
```

```text
update 1: 0.016667
update 2: 0.033333
update 3: 0.050000
```

The host registers `app.report` before checking source, then initializes the compiled module with `lhat_run`.
The returned value is its table of public members. The host roots that value before calling `update` with `lhat_machine_call_member`.
The example deliberately collects garbage before every frame to verify that the callback and its captures remain reachable.

<details>
<summary>Complete host source</summary>

<<< @/../sample/embedding/host.c{c}

</details>

## 5. Move it into your application

Replace the example's `App` with your application state and move the three-iteration loop into your own frame loop.
Keep `LhatProgram` and `LhatMachine` for the session instead of recreating them every frame.
Replace `lhat_load_file` with a custom loader if your source comes from somewhere other than the filesystem.

Run the example's tests with `ctest --test-dir build/embedding --output-on-failure`.
Continue to [Host APIs and types](./host-api.md) to expose application functionality.
