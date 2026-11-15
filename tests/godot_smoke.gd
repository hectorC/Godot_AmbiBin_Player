extends SceneTree

const DEMO = "res://main.tscn"
const FIXTURE = "res://fixtures/fixed-sources.wav"
const RATE = 48000
var failures := 0
var completions := 0


func _initialize() -> void:
	_run.call_deferred()


func _check(condition: bool, message: String) -> void:
	if not condition:
		push_error(message)
		failures += 1


func _energy(frames: PackedVector2Array) -> Vector2:
	var result := Vector2.ZERO
	for frame in frames:
		result += frame * frame
	return result


func _capture_first_burst(capture: AudioEffectCapture) -> PackedVector2Array:
	var frames := int(AudioServer.get_mix_rate() * 0.38)
	var deadline := Time.get_ticks_msec() + 3000
	while capture.get_frames_available() < frames and Time.get_ticks_msec() < deadline:
		await process_frame
	if capture.get_frames_available() < frames:
		_check(false, "AudioServer did not deliver enough frames for the first burst.")
		return PackedVector2Array()
	return capture.get_buffer(frames)


func _compare_reference(frames: PackedVector2Array, path: String) -> void:
	var wave := FileAccess.get_file_as_bytes(path)
	var offset := 12
	while offset + 8 <= wave.size() and wave.slice(offset, offset + 4).get_string_from_ascii() != "data":
		var size := wave.decode_u32(offset + 4)
		offset += 8 + size + size % 2
	offset += 8
	if offset + frames.size() * 8 > wave.size():
		_check(false, "Standalone reference is missing or truncated.")
		return
	# The built-in Godot cubic resampler returns its unity-rate signal two samples late.
	var error := 0.0
	var energy := 0.0
	for i in range(2, frames.size()):
		var position := offset + (i - 2) * 8
		var reference := Vector2(wave.decode_float(position), wave.decode_float(position + 4))
		error += (frames[i] - reference).length_squared()
		energy += reference.length_squared()
	var rms := sqrt(error / maxf(energy, 1e-12))
	_check(energy > 0.1 and rms < 1e-5, "Godot neutral render differs from the standalone render: " + str(rms))
	print("Unity-rate neutral relative RMS error: ", rms)

func _mix_pcm_fixture(path: String) -> PackedVector2Array:
	var stream := AmbisonicStream.new()
	var error := stream.load_file(path)
	if not error.is_empty():
		_check(false, "Could not load %s: %s" % [path, error])
		return PackedVector2Array()
	var playback := stream.instantiate_playback()
	if playback == null:
		_check(false, "Could not instantiate playback for " + path)
		return PackedVector2Array()
	playback.start()
	var frames := playback.mix_audio(1.0, 25000)
	playback.stop()
	return frames


func _mix_decoder_settings(stream: AmbisonicStream, method: int, max_re: bool,
		diffuse_covariance: bool, truncation_eq: bool, preprocessing: int) -> PackedVector2Array:
	var error := stream.configure_decoder(0, method, max_re, diffuse_covariance,
		truncation_eq, preprocessing, PackedByteArray())
	if not error.is_empty():
		_check(false, "Could not configure decoder: " + error)
		return PackedVector2Array()
	var playback := stream.instantiate_playback()
	if playback == null:
		_check(false, "Could not instantiate configured decoder.")
		return PackedVector2Array()
	playback.start()
	var frames := playback.mix_audio(1.0, 15000)
	playback.stop()
	_check(_energy(frames).is_finite() and _energy(frames).length() > 0.1,
		"Configured decoder produced invalid samples.")
	return frames


func _run() -> void:
	if not ClassDB.class_exists("AmbisonicStream"):
		_check(false, "AmbisonicStream GDExtension class was not registered.")
		quit(1)
		return

	var stream := AmbisonicStream.new()
	var error := stream.load_file(FIXTURE)
	if not error.is_empty():
		_check(false, error)
		quit(1)
		return
	var playback := stream.instantiate_playback()
	var parallel_playback := stream.instantiate_playback()
	if playback == null or parallel_playback == null:
		_check(false, "Failed to instantiate the audio stream.")
		quit(1)
		return
	playback.start()
	var direct := PackedVector2Array()
	for i in range(23):
		var block := playback.mix_audio(1.0, 613)
		_check(block.size() == 613, "Unexpected short mix for an active playback.")
		direct.append_array(block)
	_check(_energy(direct).length() > 0.1, "The standalone Godot mix produced silence.")
	parallel_playback.start()
	_check(direct.slice(0, 613) == parallel_playback.mix_audio(1.0, 613),
		"Two playbacks of one resource share the audio cursor.")
	playback.stop()
	parallel_playback.stop()
	var args := OS.get_cmdline_user_args()
	if AudioServer.get_mix_rate() == RATE and not args.is_empty():
		_compare_reference(direct, args[0])
	var ls := _mix_decoder_settings(stream, 0, true, false, true, 1)
	for method in range(1, 5):
		_check(ls != _mix_decoder_settings(stream, method, true, false, true, 1),
			"Decoding method %d produced the LS output." % method)
	_check(ls != _mix_decoder_settings(stream, 0, false, false, true, 1), "MaxRE had no effect.")
	_check(ls != _mix_decoder_settings(stream, 0, true, true, true, 1), "Diffuse covariance had no effect.")
	_check(ls != _mix_decoder_settings(stream, 0, true, false, false, 1), "Truncation EQ had no effect.")
	_check(ls != _mix_decoder_settings(stream, 0, true, false, true, 0), "HRIR preprocessing had no effect.")
	_check(not stream.configure_decoder(0, 4, true, false, true, 1,
		"invalid SOFA".to_utf8_buffer()).is_empty(), "An invalid SOFA file was accepted.")
	if args.size() > 1:
		for bits in [16, 24]:
			var stem := args[1].path_join("pcm%d" % bits)
			var expected := _mix_pcm_fixture(stem + "-reference.wav")
			var actual := _mix_pcm_fixture(stem + "-extensible.wav")
			_check(_energy(expected).length() > 0.1 and actual == expected,
				"%d-bit PCM playback differs from its float reference." % bits)

	for order in range(2, 6):
		var higher := AmbisonicStream.new()
		var path := "res://fixtures/fixed-sources-order-%d.wav" % order
		var load_error := higher.load_file(path)
		_check(load_error.is_empty() and higher.get_ambisonic_order() == order,
			"Failed to load or detect %s: %s" % [path, load_error])
		var higher_mix := higher.instantiate_playback()
		if higher_mix == null:
			_check(false, "Could not instantiate order %d." % order)
			continue
		higher_mix.start()
		var energy := Vector2.ZERO
		var started_at := Time.get_ticks_usec()
		for i in range(23):
			var block := higher_mix.mix_audio(1.0, 613)
			_check(block.size() == 613, "Short mix for order %d." % order)
			energy += _energy(block)
		print("Order %d Godot mixed 14,099 frames in %.2f ms (includes test energy calculation)." %
			[order, (Time.get_ticks_usec() - started_at) / 1000.0])
		_check(energy.length() > 0.1 and energy.is_finite(), "Invalid stereo output for order %d." % order)
		higher_mix.stop()

	var scene := (load(DEMO) as PackedScene).instantiate()
	root.add_child(scene)
	await process_frame
	var player := scene.get_node("AmbisonicPlayer") as AmbisonicPlayer
	player.audio_file = FIXTURE
	player.finished.connect(func(): completions += 1)
	var capture := AudioEffectCapture.new()
	capture.buffer_length = 1.0
	AudioServer.add_bus_effect(0, capture)
	var yaw_slider := scene.get_node("%Yaw") as HSlider
	var gain_slider := scene.get_node("%Gain") as HSlider

	scene.get_node("%Play").pressed.emit()
	capture.clear_buffer()
	await process_frame
	yaw_slider.value = 90
	var right := _energy(await _capture_first_burst(capture))
	_check(player.is_playing() and player.playback_position > 0.1, "Scene playback did not advance.")
	_check(right.y > 2.0 * right.x and right.x > 0.01, "The first source did not move right during a live left turn.")

	scene.get_node("%Stop").pressed.emit()
	_check(not player.is_playing(), "Stop left playback active.")
	await create_timer(0.08).timeout
	gain_slider.value = -15.0
	yaw_slider.value = -90
	scene.get_node("%Restart").pressed.emit()
	capture.clear_buffer()
	var left := _energy(await _capture_first_burst(capture))
	_check(left.x > 2.0 * left.y and left.y > 0.001, "The first source did not move left on restart.")
	var gain_difference := 10.0 * log((left.x + left.y) / maxf(right.x + right.y, 1e-12)) / log(10.0)
	_check(absf(gain_difference + 12.0) < 1.5, "Live gain differs from the expected 12 dB change.")
	print("AudioServer mix rate: ", AudioServer.get_mix_rate())
	print("Live-turn ear energies: ", right, " and ", left, "; gain change: ", gain_difference, " dB")

	await create_timer(1.30).timeout
	_check(player.is_playing() and completions == 0, "Looping ended after one pass.")
	scene.get_node("%Loop").button_pressed = false
	_check(not player.loop, "Loop control did not update the player.")
	await create_timer(1.8).timeout
	_check(not player.is_playing() and completions == 1,
		"One-shot completion after disabling Loop (playing=%s, finished=%s, position=%s)." %
		[player.is_playing(), completions, player.playback_position])

	player.audio_file = "res://missing.wav"
	_check(not player.play() and not player.last_error.is_empty(), "Missing WAV did not produce an error.")
	scene.queue_free()
	await process_frame

	var reopened := (load(DEMO) as PackedScene).instantiate()
	var autoplay_player := reopened.get_node("AmbisonicPlayer") as AmbisonicPlayer
	autoplay_player.audio_file = FIXTURE
	autoplay_player.autoplay = true
	root.add_child(reopened)
	await process_frame
	_check(autoplay_player.is_playing(), "Autoplay did not start after creating a new scene.")
	var order_picker := reopened.get_node("%Order") as OptionButton
	order_picker.select(5)
	order_picker.item_selected.emit(5)
	_check(autoplay_player.is_playing() and autoplay_player.ambisonic_order == 5,
		"Switching to the fifth-order fixture during playback failed.")
	var higher_yaw := reopened.get_node("%Yaw") as HSlider
	capture.clear_buffer()
	higher_yaw.value = 90
	var higher_frames := await _capture_first_burst(capture)
	var higher_right := _energy(higher_frames)
	print("Fifth-order live turn: ", higher_frames.size(), " captured frames, energy ", higher_right)
	_check(higher_right.y > 2.0 * higher_right.x and higher_right.x > 0.001,
		"Fifth-order playback did not follow a live left turn.")
	autoplay_player.stop()
	reopened.queue_free()
	await create_timer(0.08).timeout

	print("Godot demo smoke test failures: ", failures)
	quit(0 if failures == 0 else 1)
