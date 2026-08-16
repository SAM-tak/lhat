extends SceneTree

# What the extension registers, as lhat-host.json (05 の 8.7). The language
# server reads that file to know what the checker was told, and only this
# side knows what `godot` is -- the cli registers stdlib and links no engine.
#
#     godot --headless --path godot/demo --script dump_host_api.gd -- <out>
#
# scripts/dump-host-api.ps1 is what runs this and merges the two halves.
func _initialize() -> void:
	var out := "user://godot-host.json"
	var args := OS.get_cmdline_user_args()
	if args.size() > 0:
		out = args[0]

	var said := LhatRuntime.dump_host_api(out)
	if said != "":
		push_error(said)
		quit(1)
		return
	print(out)
	quit(0)
