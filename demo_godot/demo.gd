extends Node3D

@onready var camera: Camera3D = $Camera3D
@onready var player: AmbisonicPlayer = $AmbisonicPlayer
@onready var yaw: HSlider = %Yaw
@onready var pitch: HSlider = %Pitch
@onready var roll: HSlider = %Roll
@onready var assigned_file := player.audio_file


func _ready() -> void:
	_marker("Front", Vector3.FORWARD, Color("#f5bd75"))
	_marker("Left", Vector3.LEFT, Color("#7cdece"))
	_marker("Above", Vector3.UP, Color("#bfacfc"))
	_marker("Oblique", Vector3(2, 3, -6).normalized(), Color("#81bdfa"))
	%Order.add_item("Assigned file", 0)
	for order in range(1, 6):
		%Order.add_item("Order %d (%d ch)" % [order, (order + 1) * (order + 1)], order)
	%Order.item_selected.connect(_select_order)
	%Play.pressed.connect(func(): _play(false))
	%Stop.pressed.connect(func():
		player.stop()
		%Status.text = "Stopped"
	)
	%Restart.pressed.connect(func(): _play(true))
	%Loop.toggled.connect(func(enabled: bool): player.loop = enabled)
	%Gain.value_changed.connect(func(value: float):
		player.gain_db = value
		%GainLabel.text = "Gain   %.1f dB" % value
	)
	%Reset.pressed.connect(func():
		for slider in [yaw, pitch, roll]:
			slider.set_value_no_signal(0)
		_update_camera()
	)
	for slider in [yaw, pitch, roll]:
		slider.value_changed.connect(func(_value: float): _update_camera())
	player.finished.connect(func(): %Status.text = "Finished")
	player.playback_error.connect(func(message: String): %Status.text = message)
	_update_camera()


func _play(from_start: bool) -> void:
	var started := player.restart() if from_start else player.play()
	if started:
		%Status.text = "Playing • Order %d" % player.ambisonic_order


func _select_order(index: int) -> void:
	var order: int = %Order.get_item_id(index)
	match order:
		0:
			player.audio_file = assigned_file
		1:
			player.audio_file = "res://fixtures/fixed-sources.wav"
		_:
			player.audio_file = "res://fixtures/fixed-sources-order-%d.wav" % order
	if player.is_playing():
		_play(true)
	else:
		%Status.text = "Ready"


func _update_camera() -> void:
	camera.rotation_degrees = Vector3(pitch.value, yaw.value, roll.value)
	%YawLabel.text = "Yaw   %+.0f°" % yaw.value
	%PitchLabel.text = "Pitch   %+.0f°" % pitch.value
	%RollLabel.text = "Roll   %+.0f°" % roll.value


func _marker(caption: String, direction: Vector3, color: Color) -> void:
	var marker := MeshInstance3D.new()
	var sphere := SphereMesh.new()
	sphere.radius = 0.12
	sphere.height = 0.24
	var material := StandardMaterial3D.new()
	material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	material.albedo_color = color
	sphere.material = material
	marker.mesh = sphere
	marker.position = direction * 4
	add_child(marker)

	var label := Label3D.new()
	label.text = caption
	label.pixel_size = 0.006
	label.font_size = 26
	label.billboard = BaseMaterial3D.BILLBOARD_ENABLED
	label.modulate = color
	label.position = marker.position + Vector3.DOWN * 0.24
	add_child(label)
