extends SphereEffector3D

# Animates #260 runtime-settable fields so the raster path is exercised each
# frame. Drives target_opacity, opacity_strength, and strength with different
# periods so their channels update independently and visibly.

@export var label_path: NodePath

var _t: float = 0.0
var _label: Label3D

func _ready() -> void:
	if label_path != NodePath():
		_label = get_node_or_null(label_path)

func _process(delta: float) -> void:
	_t += delta

	# Hold opacity channel steady at maximum dissolve so the dissolve column
	# reads as a clear "thinned to ~15% opacity inside the sphere", not a
	# breathing animation that can be mistaken for flicker.
	target_opacity = 0.0
	opacity_strength = 1.0

	# position strength: 0.6 ↔ 1.8 to show runtime position control.
	strength = 1.2 + 0.6 * sin(_t * TAU * 0.18)

	if _label != null:
		_label.text = "target_opacity   %0.2f\nopacity_strength %0.2f\nstrength         %0.2f" % [
			target_opacity, opacity_strength, strength,
		]
