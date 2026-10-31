# Machines and values

Declarations: [vm.h](https://github.com/SAM-tak/lhat/blob/main/include/lhat/vm.h),
[value.h](https://github.com/SAM-tak/lhat/blob/main/include/lhat/value.h),
and [object.h](https://github.com/SAM-tak/lhat/blob/main/include/lhat/object.h).

## Creation and calls

```c
LhatMachine *lhat_machine_new(void);
void lhat_machine_dispose(LhatMachine *machine);
LhatRunResult lhat_run(LhatMachine *machine, const LhatProto *proto);
LhatRunResult lhat_machine_call(LhatMachine *machine, LhatValue callee,
                                const LhatValue *arguments, size_t count);
LhatRunResult lhat_machine_call_member(LhatMachine *machine,
    LhatValue receiver, const char *name, size_t length,
    const LhatValue *arguments, size_t count);
```

| API | Purpose |
| --- | --- |
| `lhat_machine_new_with_size` | Specify maximum frames and slots; 0 requests defaults |
| `lhat_run_arguments` | Run a unit with arguments |
| `lhat_machine_call` | Call a closure or host function; an overload group itself is not accepted |
| `lhat_machine_call_member` | Resolve a named member, handling its receiver and overloads |

Check `status == LHAT_RUN_OK` before reading `value`.
Multiple return values use `positions` and `position_count`.
Root GC-managed values if you need to retain them across subsequent execution.

## Coroutines and budgets

| API | Purpose |
| --- | --- |
| `lhat_machine_resume` | Start or resume a coroutine and retrieve its yielded / returned values |
| `lhat_machine_coroutine_done` | Check coroutine completion |
| `lhat_machine_make_coroutine` | Create a coroutine implemented by a C step function |
| `lhat_machine_set_budget` | Set an execution budget in backward branches; 0 means unlimited |
| `lhat_machine_is_suspended` | Check whether the machine is suspended by its budget |
| `lhat_machine_continue` | Continue the same budget-suspended execution with a fresh slice |

Do not start another call while the machine is `LHAT_RUN_SUSPENDED`.
See [Execution and lifetimes](../embedding/runtime.md) for the distinction between a coroutine yield and a machine-wide suspension.

## Host roots and weak references

| API | Purpose |
| --- | --- |
| `lhat_machine_host_root` | Retrieve the machine-rooted table reserved for the host |
| `lhat_machine_table_set` | Write with a GC barrier; handle both false and `refused == true` |
| `lhat_machine_weak_cache_get` | Look up a live wrapper by host pointer; returns nil if absent |
| `lhat_machine_weak_cache_put` | Remember a wrapper weakly; does not root it |
| `lhat_machine_weak_cache_forget` | Remove a cache entry, for example when a host object is destroyed |

To remove a host root, write nil under its key.
Registered modules under `L^.modules` are not writable and must not be used as host retention storage.

## Creating and reading values

| API | Purpose / caveats |
| --- | --- |
| `lhat_nil` / `lhat_bool` / `lhat_integer` / `lhat_real` | Construct scalar values |
| `lhat_is_*` / `lhat_as_*` | Check a kind and read the matching representation |
| `lhat_number_as_real` / `lhat_number_as_whole` | Convert between integer and real representations of `number^` |
| `lhat_machine_make_string` / `lhat_machine_make_table` | Allocate strings and tables on the machine heap |
| `lhat_table_get` / `lhat_table_get_bytes` | Read table values |
| `lhat_machine_make_hostdata` / `lhat_hostdata_pointer` | Wrap a host pointer / check its tag and unwrap |
| `lhat_make_hostvalue` / `lhat_hostvalue_data` | Return a value type / read one from callback arguments |
| `lhat_place_hostvalue` | Place a value-type argument in a host-owned `LhatHostValueRoom` |
| `lhat_machine_make_error` | Construct a value of a registered error kind |
| `lhat_value_text` | Format a value as text, with size measurement support |

A pointer returned by `lhat_hostvalue_data` is valid only during the callback.
`lhat_make_hostvalue` uses scratch space for a single returned value.
For multiple hostvalue arguments, allocate a separate `LhatHostValueRoom` per argument and retain it until the call returns.

## GC and diagnostics

| API | Purpose |
| --- | --- |
| `lhat_machine_gc_collect` | Request a full collection; returns live object count, not bytes |
| `lhat_machine_gc_settings` / `lhat_machine_gc_configure` | Read / configure GC pacing |
| `lhat_machine_gc_stats` / `lhat_machine_gc_step` / `lhat_machine_gc_pause` | GC statistics, manual steps, and pausing |
| `lhat_machine_pending_disposals` | Count coroutines with cleanup still pending |
| `lhat_machine_panic` / `lhat_machine_panic_text` | Raise a runtime error from a host callback |
| `lhat_run_error_write` / `lhat_machine_traceback` | Format a runtime failure report and traceback |
| `lhat_machine_fault_depth` / `lhat_machine_fault_frame` | Inspect frames from a failed execution |

GC may run L^ code to clean up a discarded coroutine.
Keep host contexts and registrations valid until GC and machine disposal have completed.
