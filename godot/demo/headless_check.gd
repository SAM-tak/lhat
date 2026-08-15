extends SceneTree

# The demo without an editor:
#
#     godot --headless --path godot/demo --script headless_check.gd
#
# Exits non-zero if the extension did not load or the unit did not run, so it
# is the form that suits a build that wants checking rather than looking at.
func _initialize() -> void:
	if not ClassDB.class_exists("LhatRuntime"):
		push_error("the L^ extension did not load")
		quit(1)
		return

	print("L^ ", LhatRuntime.version())

	var said := LhatRuntime.run("res://hello.lh")
	for line in said:
		push_error(line)
	quit(0 if said.is_empty() else 1)
