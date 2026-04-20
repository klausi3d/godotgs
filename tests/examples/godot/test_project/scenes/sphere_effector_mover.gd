extends SphereEffector3D

# Ping-pongs the effector along the X axis so it sweeps across the row of
# cabins beneath it, giving a real "dissolve travelling through the scene"
# animation rather than a steady dissolve. Combined with the #1 sphere-sway
# fix, position effects now actually read as coherent motion.

@export var sweep_distance: float = 18.0
@export var period_seconds: float = 8.0

var _t: float = 0.0
var _origin: Vector3

func _ready() -> void:
	_origin = position

func _process(delta: float) -> void:
	_t += delta
	var phase: float = sin(_t * TAU / period_seconds)
	position = _origin + Vector3(phase * sweep_distance, 0.0, 0.0)
