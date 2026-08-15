@tool
extends EditorScript

# Open this in the script editor and run it with File > Run (Ctrl+Shift+X).
# It answers in the Output panel, without a scene and without playing the
# project -- which is the whole of what this demo is for while the extension
# has nothing but these two calls in it.
func _run() -> void:
	print("L^ ", LhatRuntime.version())
	print(LhatRuntime.run_status_message(0))
