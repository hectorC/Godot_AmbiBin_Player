# Build and Godot API Verification

## macOS build and packaging

The [macOS workflow](.github/workflows/macos.yml) builds on native Apple Silicon and Intel runners with Apple's Accelerate backend and runs the decoder, WAV, and Godot smoke tests against the pinned 4.8 beta 3 editor. After both jobs pass, the packaging job combines their dylibs with `lipo`, signs the result ad hoc, and uploads the `Godot-AmbiBin-Player-macOS` add-on artifact. SPARTA's macOS builds are the reference for covering both architectures. [SPARTA releases](https://github.com/leomccormack/SPARTA/releases)

The equivalent native build commands on a Mac with Xcode Command Line Tools, CMake, Git, and Python 3 are:

```sh
cmake -S . -B build/macos -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="$(uname -m)" \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 -DAMBI_BUILD_GODOT=ON \
  -DAMBI_GODOT_EXE="/Applications/Godot.app/Contents/MacOS/Godot"
cmake --build build/macos --parallel
ctest --test-dir build/macos -C Release --output-on-failure
cmake --install build/macos --config Release --prefix build/macos-addon
```

## Asset Library release

Stage the release from the CMake install output. For a combined release, put the tested Windows x64 DLL, its OpenBLAS dependency, and the universal macOS dylib in `addons/ambisonic_player/bin/Release`. Use a public distribution commit with that add-on at the archive root, plus the project license and ignore file. Keep the source and build recipes available at the corresponding source revision and identify that revision in the release documentation. Check the actual GitHub commit ZIP by installing it into a clean Godot project before submitting it. The Asset Library consumes that ZIP. The newer Asset Store accepts an uploaded add-on ZIP. [Asset Library submission guide](https://docs.godotengine.org/en/stable/community/asset_library/submitting_to_assetlib.html) · [Asset Store submission guide](https://docs.godotengine.org/en/stable/community/asset_store/submitting_to_asset_store.html)

The public demo uses generated directional fixtures. Personal recordings, SOFA files and `demo_godot/main.local.tscn` are ignored by Git.

## Build and test

Install Visual Studio 2026 with Desktop development with C++, the Windows SDK, CMake, Git, and Python 3 for CTest. The verified toolchain uses CMake 4.3.1, MSVC 19.51, and Windows SDK 10.0.26100. CMake fetches the pinned SAF and dr_libs sources and verifies the checksum of the OpenBLAS Windows x64 archive.

```powershell
cmake -S . -B build/windows-x64 -G 'Visual Studio 18 2026' -A x64
cmake --build build/windows-x64 --config Release --parallel
ctest --test-dir build/windows-x64 -C Release --output-on-failure
```

Use `Debug` in both commands for the Debug configuration. Set `-DBUILD_TESTING=OFF` at configure time to build the renderer without the Python test.

Windows builds use OpenBLAS LP64 for BLAS and LAPACKE. CMake corrects four LAPACKE call arguments in the pinned SAF source so diffuse covariance matching works with OpenBLAS. The build uses SAF's bundled FFT and default HRIRs and enables the SOFA reader with libmysofa and zlib. The Windows add-on ships `libopenblas.dll` beside the GDExtension and declares it as a Godot export dependency.

## Revisions

| Dependency | Revision |
| --- | --- |
| Godot 4.8 beta 3 (Windows x64, single precision) | `c51ff06e8836a70ea905701c081bf94124dcd6ce` |
| [godot-cpp](https://github.com/godotengine/godot-cpp/commit/2a4c066e1e8c121046e9d35fe91cc8e726861c48) | `2a4c066e1e8c121046e9d35fe91cc8e726861c48` |
| [SAF 1.3.5](https://github.com/leomccormack/Spatial_Audio_Framework/tree/18fd5aba46e20787b51f28f7197a68506c965c07) | `18fd5aba46e20787b51f28f7197a68506c965c07` |
| [dr_libs](https://github.com/mackron/dr_libs/tree/f12e197fd462e4c22873fbd70d11b1d2f2c1dc3c) | `f12e197fd462e4c22873fbd70d11b1d2f2c1dc3c` |
| [OpenBLAS](https://github.com/OpenMathLib/OpenBLAS/releases/tag/v0.3.34) | 0.3.34 (Windows x64 LP64) |

## Reproduce the Godot API check

The standalone renderer has no Godot build dependency. For the compatibility check, dump the API from the approved beta executable and generate bindings from the pinned `godot-cpp` revision:

```powershell
git clone https://github.com/godotengine/godot-cpp.git build/upstream/godot-cpp
git -C build/upstream/godot-cpp checkout --detach 2a4c066e1e8c121046e9d35fe91cc8e726861c48

$godot = 'D:\Program_Files\Godot_v4.8-beta3_win64.exe'
$dump = New-Item -ItemType Directory -Force build/godot-api
Push-Location $dump
try {
    $process = Start-Process -FilePath $godot -WindowStyle Hidden -Wait -PassThru -ArgumentList @(
        '--headless', '--dump-extension-api', '--dump-gdextension-interface-json'
    )
    if ($process.ExitCode -ne 0) { throw "Godot API dump failed: $($process.ExitCode)" }
} finally {
    Pop-Location
}

Get-FileHash build/godot-api/extension_api.json, build/upstream/godot-cpp/gdextension/extension_api-4-8.json
Get-FileHash build/godot-api/gdextension_interface.json, build/upstream/godot-cpp/gdextension/gdextension_interface.json

cmake -S build/upstream/godot-cpp -B build/godot-cpp-bindings -G 'Visual Studio 18 2026' -A x64 '-DGODOTCPP_API_VERSION=4.8'
cmake --build build/godot-cpp-bindings --config Release --target generate_bindings
```

Both pairs of JSON files matched byte for byte on this workstation. The generated `AudioStream` virtual is `Ref<AudioStreamPlayback> _instantiate_playback() const`. The generated `AudioStreamPlayback` virtuals include `int32_t _mix(AudioFrame*, float, int32_t)`, `void _start(double)`, `void _stop()`, `bool _is_playing() const`, and `double _get_playback_position() const`. The API defines `AudioFrame` as two floats in left, right order. Repeat the check when switching to Godot 4.8 stable.

## Build and run the Godot demo

```powershell
cmake -S . -B build/windows-x64 -G 'Visual Studio 18 2026' -A x64 -DAMBI_BUILD_GODOT=ON '-DAMBI_GODOT_EXE=D:/Program_Files/Godot_v4.8-beta3_win64.exe'
cmake --build build/windows-x64 --config Release --parallel 6
ctest --test-dir build/windows-x64 -C Release --output-on-failure
```

The build fetches the pinned `godot-cpp`, links the Windows x64 extension, and generates the demo's test recordings for orders 1 through 5. The demo references the Release DLL in `demo_godot/addons/ambisonic_player/bin/Release`. Open `demo_godot/project.godot` in Godot and run the main scene. Copy the built `addons/ambisonic_player` folder into another Godot project to use the node there.

`AMBI_GODOT_EXE` registers the optional Release test that imports the project and runs a smoke test with Godot's headless audio driver. This test checks the neutral mix against the standalone render with the Godot resampler's two-sample delay accounted for. It also checks live camera turning through `AudioServer`, separate playback state for a reused resource, play/stop/restart, loop and natural completion, gain, and a missing-file error. Logs are in `build/windows-x64/fixtures/Release/godot-import.log` and `godot-smoke.log`.

The first headless editor import can crash on shutdown in beta 3 when it exits during GDExtension documentation generation. The test runner uses `--frame-delay 800` for the import step. [Godot issue 111048](https://github.com/godotengine/godot/issues/111048)

`AmbisonicStream` reads the original WAV on the scene thread and initializes one decoder per playback before mixing. Playback stages SAF's 128-frame blocks for Godot's resampler. The scene thread validates the listener pose and publishes it with lock-free atomics. The audio thread uses the latest complete pose between blocks. Looping wraps the input while retaining the decoder's filter history. User-facing playback position is approximate because of buffering.

The SOFA editor importer embeds the original SOFA bytes in a `SofaHrir` resource. At playback startup, the extension validates the dataset and temporarily stages it as a file for SAF's path-based reader. The staging file is removed after decoder initialization. SOFA loading and decoder initialization happen outside the audio callback. The build uses the pinned SAF behavior for LS, LS with diffuse EQ, SPR, TA, and MagLS. The smoke test exercises these methods and the decoder controls.

Normal headphones, physical audio-device rate changes, release exports, and real-time performance under sustained camera movement remain to be verified.

## Standalone verification

- `src/ambisonic_decoder.*` wraps AmbiBIN with per-instance ownership. It configures the detected order, ACN, and SN3D, then calls `ambi_bin_init` and `ambi_bin_initCodec` before processing. The pinned SAF source converts SN3D to N3D internally. The decoder uses AmbiBIN's default MagLS and built-in Genelec Aural ID KEMAR HRIRs.
- `src/wav_io.*` validates the WAV and reads 16-bit PCM, 24-bit PCM, or 32-bit IEEE float into planar float buffers. WAVE_FORMAT_EXTENSIBLE can also carry left-aligned 24-bit PCM in a 32-bit container. `src/main.cpp` stages 128-frame blocks for the decoder and interleaves the left and right outputs.
- `tests/render_smoke.py` generates 96,037 frames of synthetic FOA with left and right bursts and an impulse in the final partial block. It checks stereo frame count, levels, ear dominance, the final impulse, and identical output from standard and extensible float WAVs. It compares both PCM depths and the padded 24-bit format with equivalent float samples. It also checks rejection of wrong sample rate, wrong channel count, unsupported PCM depth, truncated data, and non-finite samples.
- Release and Debug passed on this workstation. In Release, the 97,573-frame output peaked at 0.300 and the near ear was 13.42 dB stronger for the left burst and 13.53 dB stronger for the right burst. The fixture and render are in `build/windows-x64/fixtures/Release` as `foa-left-right.wav` and `stereo-left-right.wav`.

The smoke test can run with its generated fixtures alone. A representative ambiX recording would support production-file and headphone validation.

## Listener rotation verification

`listener_rotation` takes the three columns of a right-handed, orthonormal Godot camera rotation. The Godot node uses the camera's global orientation with scale removed. The conversion maps Godot XYZ into SAF forward, left, and up as `(-Z, -X, +Y)` and applies the inverse of the complete camera rotation in those coordinates. It extracts the angles in SAF's rotation order, including a defined solution at the pitch singularities. The playback applies the converted angles with `AmbisonicDecoder::set_listener_rotation` between blocks.

To render a fixed source with the camera turned 90° left, pass the three basis columns after the input and output files:

```powershell
.\build\windows-x64\Release\ambi_render.exe "recording.wav" "binaural-left-turn.wav" --listener-basis 0 0 -1  0 1 0  1 0 0
ctest --test-dir build/windows-x64 -C Release -R listener_rotation --verbose
```

`tests/rotation.py` generates four fixed sources in front, left, above, and at an oblique direction. For each pose, it projects the source directions onto the camera axes and writes a reference FOA file directly in ACN/SN3D. It then compares two stereo renders sample by sample. One uses AmbiBIN rotation on the fixed sources. The other uses the same neutral binaural decode on the projected reference. This isolates the rotation without introducing a different HRTF or decoder configuration.

The test covers neutral, ±90° yaw, pitch, and roll, and four combined poses including straight up, straight down, and near vertical. It also checks ear dominance for yaw and roll and rejects scaled, reflected, and non-finite bases. The Release run had a maximum absolute sample error of `5.96e-08` and a maximum relative RMS error of `1.32e-07`. Numerical tolerances in the test allow for floating-point differences across builds.

For headphone comparison, open `listener-neutral.wav`, `listener-yaw-left.wav`, and `listener-yaw-right.wav` under `build/windows-x64/fixtures/Release/rotation`. The first burst is the same world-front source. It should move to the right ear when the camera turns left and to the left ear when the camera turns right. A separate SPARTA render remains unverified.

## Higher Order Verification

The `higher_order_render` test synthesizes independent ACN/SN3D fixtures at orders 2–5 and compares AmbiBIN's rotated output with neutral decodes of directly re-encoded directions. It also isolates the highest band at each order to verify that the decoder uses those channels, and checks rejection of an incomplete 8-channel signal and an unsupported 49-channel signal.

The Godot smoke test loads and mixes each higher order, switches the demo to fifth order during playback, and captures the front burst on the expected side after a live camera turn. Its per-order mix timings are from a headless run and include test signal analysis. Sustained real-time performance and physical output-device testing remain part of Milestone 4.
