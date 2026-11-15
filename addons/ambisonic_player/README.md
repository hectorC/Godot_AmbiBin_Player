# Godot AmbiBin Player

Copy `addons/ambisonic_player` into a Godot project. This preview build was tested on Godot 4.8 beta 3 on Windows x64 and macOS (Apple Silicon and Intel). Add an `AmbisonicPlayer` node, assign an Audio File and a Camera3D listener, then call `play()` or enable Autoplay. The player decodes to binaural stereo for headphones and sends the result through Godot's standard mixer.

Input files must be 48 kHz ambiX WAVs (ACN/SN3D), with 4, 9, 16, 25, or 36 channels. The player accepts 16-bit and 24-bit PCM and 32-bit float. In the Import dock, choose **Keep File (exported as is)** for the WAV and click **Reimport**.

To use custom HRIRs, enable the **Godot AmbiBin Player SOFA Importer** in Project Settings, import a SimpleFreeFieldHRIR `.sofa` file, and assign it to **SOFA HRIR** in the player's **SAF Decoding** group. Start or restart playback to apply decoder changes.

[Project source and support](https://github.com/hectorC/Godot_AmbiBin_Player)

See [LICENSE](LICENSE) and [third-party notices](LICENSES.md).
