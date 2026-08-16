extends Node

# The whole demo, in the one place that runs both ways: F5 in the editor and
#
#     godot --headless --path godot/demo
#
# An EditorScript would not do -- File > Run wants an EditorScript and
# --script wants a MainLoop, and one script cannot be both. A main scene is
# what both of them already know how to start.
#
# Exits non-zero if the extension did not load or the unit did not run, so it
# is the form that suits a build that wants checking rather than looking at.
func _ready() -> void:
	if not ClassDB.class_exists("LhatRuntime"):
		push_error("the L^ extension did not load")
		get_tree().quit(1)
		return

	print("L^ ", LhatRuntime.version())

	# Empty means the whole graph -- hello.lh and what it requires -- read,
	# parsed and type checked without one diagnostic.
	var said := LhatRuntime.check("res://hello.lh")
	if not said.is_empty():
		for line in said:
			push_error(line)
		get_tree().quit(1)
		return

	# The same, and then runs it. What it prints goes to the Output panel.
	said = LhatRuntime.run("res://hello.lh")
	for line in said:
		push_error(line)
	if not said.is_empty():
		get_tree().quit(1)
		return

	# Values crossing both ways: a unit answers the table of its public^
	# names, and one of them is called with Variants converted on the way in
	# and on the way back.
	var api := "res://lib/api.lh"
	print(LhatRuntime.call_member(api, "greet", ["Godot"]))
	print(LhatRuntime.call_member(api, "total", [[1, 2, 3, 4]]))
	print(LhatRuntime.call_member(api, "describe", ["L^"]))
	print(LhatRuntime.call_member(api, "numbers", [5]))

	# A node wearing an L^ script. The members are shared with every other
	# node wearing the file and the self^ fields are each node's own
	# (02 の 14.3), so two of them count separately.
	var script := load("res://counter.lh")
	var one := Node.new()
	var two := Node.new()
	one.set_script(script)
	two.set_script(script)
	add_child(one)
	add_child(two)

	# _ready is deferred to the frame the node entered on, and _process runs
	# from then on because has_method said the unit wrote one.
	await get_tree().process_frame
	await get_tree().process_frame
	one.call("tick")
	print("one=", one.call("count"), " two=", two.call("count"))
	print(one.call("greet", "Godot"))

	get_tree().quit(0)
