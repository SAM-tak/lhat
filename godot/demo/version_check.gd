@tool
extends EditorScript

# Open this in the script editor and run it with File > Run (Ctrl+Shift+X).
# It answers in the Output panel, without a scene and without playing the
# project.
func _run() -> void:
	print("L^ ", LhatRuntime.version())

	# Empty means the whole graph -- hello.lh and what it requires -- read,
	# parsed and type checked without one diagnostic.
	print("check: ", LhatRuntime.check("res://hello.lh"))

	# The same, and then runs it. What it prints goes to this same panel.
	print("run: ", LhatRuntime.run("res://hello.lh"))
