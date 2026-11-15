@tool
extends EditorImportPlugin

func _get_importer_name() -> String:
	return "ambisonic_player.sofa"


func _get_visible_name() -> String:
	return "SOFA HRIR"


func _get_recognized_extensions() -> PackedStringArray:
	return PackedStringArray(["sofa"])


func _get_save_extension() -> String:
	return "res"


func _get_resource_type() -> String:
	return "SofaHrir"


func _get_preset_count() -> int:
	return 0


func _get_import_options(_path: String, _preset_index: int) -> Array[Dictionary]:
	return []


func _import(source_file: String, save_path: String, _options: Dictionary,
		_platform_variants: Array[String], _gen_files: Array[String]) -> Error:
	var bytes := FileAccess.get_file_as_bytes(source_file)
	if bytes.is_empty():
		return ERR_FILE_CANT_READ
	var resource := SofaHrir.new()
	resource.data = bytes
	return ResourceSaver.save(resource, save_path + ".res")
