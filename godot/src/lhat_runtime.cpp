#include "lhat_runtime.h"

#include <godot_cpp/core/class_db.hpp>

#include "lhat.h"

namespace godot {

void LhatRuntime::_bind_methods()
{
    ClassDB::bind_static_method("LhatRuntime", D_METHOD("version"),
                                &LhatRuntime::version);
    ClassDB::bind_static_method("LhatRuntime",
                                D_METHOD("run_status_message", "status"),
                                &LhatRuntime::run_status_message);
}

String LhatRuntime::version()
{
    return String(LHAT_VERSION);
}

// A status the machine does not know answers "unknown" rather than reading
// past its own table, so the int arrives from GDScript unchecked on purpose.
String LhatRuntime::run_status_message(int status)
{
    return String(lhat_run_status_message((LhatRunStatus)status));
}

}  // namespace godot
