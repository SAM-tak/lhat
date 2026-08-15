extends SceneTree

# The same two calls without an editor:
#
#     godot --headless --path godot/demo --script headless_check.gd
#
# Exits non-zero if the extension did not load, so it is the form that suits
# a build that wants checking rather than looking at.
func _initialize() -> void:
	if not ClassDB.class_exists("LhatRuntime"):
		push_error("the L^ extension did not load")
		quit(1)
		return
	print("L^ ", LhatRuntime.version())
	print(LhatRuntime.run_status_message(0))
	quit(0)
