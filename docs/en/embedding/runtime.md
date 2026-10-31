# Frame loops and value lifetimes

## Separate initialization from frame updates

Normally, run `lhat_program_check` and `lhat_program_compile` at startup or when a script changes.
Initialize modules with `lhat_run` and retain their public callbacks or newly created instances.
Each frame, call the required code with `lhat_machine_call` or `lhat_machine_call_member`.

`lhat_machine_call_member` resolves a member on its receiver and supplies that receiver to members taking `self^`.
Simply reading a table member and using `lhat_machine_call` may not handle an instance method's receiver correctly.

## C variables are not GC roots

Storing `LhatValue callback` in a C structure does not make the collector scan that structure.
Put callbacks, instances, and coroutines retained across frames in the machine's host root table.

```c
LhatTable *roots = lhat_machine_host_root(machine);
bool refused = false;
bool stored = roots && lhat_machine_table_set(
    machine, roots, lhat_integer(0), callback, &refused);
/* Use callback later only if stored && !refused. */
```

Remove a root by writing `lhat_nil()` under the same key.
Use `lhat_machine_table_set` for writes to tables held by the host. Its write barrier handles an in-progress GC cycle.
Do not substitute a direct call to `lhat_table_set`.

Disposing a machine invalidates values on its heap.
Program-owned code and constants must outlive the machines using them.

## Coroutine yields and execution budgets

| State | Host action |
| --- | --- |
| A coroutine executes `yield^` | Advance it using `lhat_machine_resume` |
| A budget produces `LHAT_RUN_SUSPENDED` | Continue the same execution using `lhat_machine_continue` |

Calling a yieldable procedure returns a coroutine. Root it and execute it with `lhat_machine_resume`.
A resume result contains a yielded or final returned value; distinguish completion with `lhat_machine_coroutine_done`.

`lhat_machine_set_budget(machine, turns)` divides execution according to backward loop branches.
0 means unlimited. This is not a time limit, instruction limit, or limit on C callback execution time.
Nested execution that reenters L^ from the host is not suspended by this budget either.

::: warning A machine suspended by its budget
Do not run another event or callback on that machine.
On the next frame, use `lhat_machine_continue`; if it suspends again, carry it forward.
To abandon the suspended execution, dispose of the machine.
:::

For independent tasks, choose coroutine scheduling or separate machines as appropriate.
Do not execute the same machine concurrently on multiple threads.
For program updates from multiple threads, supply a host mutex using `lhat_program_set_lock` before execution starts.
Create programs and register APIs serially.

## Handling execution failures

Check every `LhatRunResult.status`, treating budget suspension separately from errors.
For other failures, record `lhat_run_error_write` or `lhat_machine_traceback` before starting another execution.
An API returning typed errors as ordinary values may return an error value even when the run status is `LHAT_RUN_OK`.

## Shutdown order

1. Stop accepting new events, then stop and join workers.
2. Run required script cleanup while engine state is still valid.
3. Clear host-held values and caches.
4. Call `lhat_machine_dispose`.
5. Call `lhat_program_free`.
6. Destroy engine state used by the machine's release hooks.

Call `lhat_registry_dispose` only after every program in the process has been destroyed.
A plugin must not call it merely because it has destroyed its own program.
See [Machines and values](../api/machine.md) for API details.
