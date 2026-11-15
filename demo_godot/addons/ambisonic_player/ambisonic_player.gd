class_name AmbisonicPlayer
extends Node
## Plays 48 kHz ambiX WAVs (orders 1–5) in 16/24-bit PCM or 32-bit float.

signal finished
signal playback_error(message: String)

@export_file("*.wav") var audio_file: String
@export var listener: Camera3D
@export var autoplay := false
@export var loop := false:
	set(value):
		loop = value
		if is_instance_valid(_stream):
			_stream.set_loop(value)
@export_range(-60, 12, 0.1, "suffix:dB") var gain_db := 0.0:
	set(value):
		gain_db = value
		if is_instance_valid(_player):
			_player.volume_db = value

@export_group("SAF Decoding")
## Auto uses the WAV's detected order. A lower order drops the higher bands.
@export_enum("Auto", "1st", "2nd", "3rd", "4th", "5th") var decoding_order := 0
@export_enum("Least Squares (LS)", "LS with Diffuse EQ", "Spatial Resampling (SPR)",
	"Time Alignment (TA)", "Magnitude Least Squares (MagLS)") var decoding_method := 4
@export var max_re_weights := true
@export var diffuse_covariance_constraint := false
## SAF applies truncation EQ with LS and with phase simplification disabled.
@export var truncation_eq := true
@export_enum("Off", "Diffuse-field EQ", "Phase Simplification", "Diffuse-field EQ + Phase Simplification")
var hrir_preprocessing := 1
## Leave empty for the bundled KEMAR HRIRs. Assign an imported SOFA resource to use custom HRIRs.
@export var sofa_hrir: SofaHrir

var playback_position: float:
	get:
		return _player.get_playback_position() if is_instance_valid(_player) else 0.0

## Detected order, or 0 until the assigned file has been loaded.
var ambisonic_order: int:
	get:
		return _stream.get_ambisonic_order() if is_instance_valid(_stream) and _loaded_path == audio_file else 0

var last_error := ""
var _player: AudioStreamPlayer
var _stream: AmbisonicStream
var _loaded_path := ""
var _last_basis := Basis.IDENTITY


func _ready() -> void:
	_stream = AmbisonicStream.new()
	_stream.set_loop(loop)
	_player = AudioStreamPlayer.new()
	_player.stream = _stream
	_player.volume_db = gain_db
	_player.finished.connect(func(): finished.emit())
	add_child(_player)
	set_process(false)
	if autoplay:
		play()


func play() -> bool:
	if not is_node_ready():
		return _fail("Wait for AmbisonicPlayer.ready before calling play().")
	if audio_file.is_empty():
		return _fail("Assign an ambiX WAV (orders 1–5) to Audio File.")
	if _loaded_path != audio_file:
		stop()
		var error := _stream.load_file(audio_file)
		if not error.is_empty():
			return _fail(error)
		_loaded_path = audio_file
	if is_playing():
		return true

	var hrir_bytes := PackedByteArray()
	if sofa_hrir != null:
		hrir_bytes = sofa_hrir.data
		if hrir_bytes.is_empty():
			return _fail("The assigned SOFA HRIR resource is empty.")
	var decoder_error := _stream.configure_decoder(decoding_order, decoding_method, max_re_weights,
		diffuse_covariance_constraint, truncation_eq, hrir_preprocessing, hrir_bytes)
	if not decoder_error.is_empty():
		return _fail(decoder_error)
	_update_listener()
	_player.play()
	if not _player.is_playing():
		return _fail("Could not start the Ambisonic stream.")
	last_error = ""
	set_process(true)
	return true


func stop() -> void:
	if is_instance_valid(_player):
		_player.stop()
	set_process(false)


func restart() -> bool:
	stop()
	return play()


func is_playing() -> bool:
	return is_instance_valid(_player) and _player.is_playing()


func _process(_delta: float) -> void:
	if is_playing():
		_update_listener()
	else:
		set_process(false)


func _update_listener() -> void:
	var camera := listener if is_instance_valid(listener) else get_viewport().get_camera_3d()
	var basis := Basis.IDENTITY
	if camera != null:
		basis = camera.global_transform.basis
		if not basis.is_finite() or is_zero_approx(basis.determinant()):
			return
		if basis == _last_basis:
			return
		basis = basis.orthonormalized()
	if basis != _last_basis and _stream.set_listener_basis(basis):
		_last_basis = basis


func _fail(message: String) -> bool:
	last_error = message
	playback_error.emit(message)
	return false
