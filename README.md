# Godot AmbiBin Player

A Godot 4.8 add-on for playing an ambiX soundfield over headphones. The `AmbisonicPlayer` node uses the Spatial Audio Framework (SAF) for binaural decoding and keeps the soundfield fixed in world space as a `Camera3D` turns. The project uses [GPL-3.0-or-later](LICENSE).

## Run the Godot demo

Follow the [GDExtension build instructions](BUILD.md#build-and-run-the-godot-demo), then open `demo_godot/project.godot` in Godot 4.8 beta 3 and press F5. Use headphones, click Play, and turn the camera with the Yaw, Pitch, and Roll sliders. The recording selector offers the file assigned in the scene and matched test recordings for orders 1 through 5. The order-specific recordings play bursts from front, left, above, and an oblique direction. Turning left makes the front burst move to the right ear.

To use the player in another scene, add an `AmbisonicPlayer` node and assign an **Audio File** and a **Listener** in the Inspector. With no listener assigned, it follows the viewport's active camera. Enable **Autoplay** or call `play()` from GDScript. Use **Loop** and **Gain dB** in the Inspector or set them during playback. The node also exposes `stop()`, `restart()`, `is_playing()`, `playback_position`, and the detected `ambisonic_order` after loading. The `playback_error` signal reports an unreadable or unsupported file.

Use a 48 kHz RIFF WAV in ACN channel order with SN3D normalization. Both the Godot player and standalone renderer accept **16-bit PCM, 24-bit PCM, and 32-bit IEEE float**, including WAVE_FORMAT_EXTENSIBLE. They infer the order from the channel count. The file must already be ambiX because its channel count and encoding alone cannot establish its Ambisonic convention. For WAVs inside the Godot project, choose the **Keep File** importer so the extension can read the original channels.

| Ambisonic order | Input channels |
| --- | ---: |
| 1 | 4 |
| 2 | 9 |
| 3 | 16 |
| 4 | 25 |
| 5 | 36 |

## SAF decoding and custom HRIRs

Select `AmbisonicPlayer` in the scene and open **SAF Decoding** in the Inspector. You can set the decoding order, method, MaxRE weighting, diffuse covariance constraint, truncation EQ, and HRIR preprocessing. **Auto** uses the detected WAV order. The method defaults to MagLS. SAF applies truncation EQ with plain LS when phase simplification is off. Decoder settings take effect the next time playback starts or restarts.

Leave **SOFA HRIR** empty to use SAF's bundled KEMAR HRIRs. To use a custom set:

1. Enable **Godot AmbiBin Player SOFA Importer** in **Project > Project Settings > Plugins**. The demo already enables it.
2. Copy a `.sofa` file into the project. Godot imports it as a **SOFA HRIR** resource.
3. Assign the imported resource to **SOFA HRIR** on the player and start or restart playback.

The scene references the imported resource, so it can travel with the project. Keep the original `.sofa` and its `.import` sidecar in source control. The loader accepts two-receiver `SimpleFreeFieldHRIR` datasets with spherical directions in degrees. Direction- or ear-dependent delays need to be baked into the impulse responses. Playback errors surface through the player's `playback_error` signal and in the demo's status.

## Standalone renderer

Follow [BUILD.md](BUILD.md) to build and run the smoke tests. To render a recording, run this from the repository root after building:

```powershell
.\build\windows-x64\Release\ambi_render.exe "recording.wav" "binaural.wav"
```

The output preserves AmbiBIN's 1,536-frame processing delay (32 ms at 48 kHz). The renderer zero-pads the final partial block and continues with silence for the reported delay. The output contains the original frame count plus the delay. Float samples retain SAF's output level.

The renderer also accepts a fixed listener orientation using `--listener-basis`. See [BUILD.md](BUILD.md#listener-rotation-verification) for the basis convention and test renders, and [LICENSES.md](LICENSES.md) for dependency notices and distribution items.
