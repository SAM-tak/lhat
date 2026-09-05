// L^ (lhat) -- host registration and value construction.

#include "vm_internal.h"
#include <string.h>
#include <stdlib.h>
#include "hosted.h"
#include "lhat/port.h"
#include "type.h"

// ---------------------------------------------------------------------------
// 05 の 8.9: host values at run time
// ---------------------------------------------------------------------------

// The members table the machine built for a host value type at install, or
// NULL when the registration never reached this machine.
LhatTable *vm_hostvalue_members_of(Machine *m, const LhatHostValueTag *tag)
{
    return tag != NULL && tag->index < m->hostvalue_member_count
               ? m->hostvalue_members[tag->index]
               : NULL;
}

// Value equality is byte equality under the same tag -- a host value has no
// identity to fall back on, which is half of what makes it a value. Every
// making zeroes the data run's unused tail, so whole slots compare exactly.
bool vm_hostvalue_equal(LhatSlots slots, size_t left, size_t right)
{
    const LhatHostValueTag *tag = slots.values[left].hostvalue;
    if (tag == NULL || slots.tags[right] != LHAT_VALUE_HOSTVALUE ||
        slots.values[right].hostvalue != tag) {
        return false;
    }
    return memcmp(slots.values + left + 1, slots.values + right + 1,
                  (tag->width - 1) * sizeof(LhatValueUnion)) == 0;
}

// A host answered with a head-shaped run -- an argument passed through, or
// the scratch lhat_make_hostvalue filled -- written out whole into the slots
// at `at`. False when the run is not one.
bool vm_place_hostvalue_answer(Machine *m, size_t at, LhatValue answered)
{
    const LhatValueUnion *run = answered.as.hostvalue_run;
    const LhatHostValueTag *tag = run != NULL ? run[0].hostvalue : NULL;
    if (tag == NULL || at + tag->width > m->slot_capacity) {
        return false;
    }
    // Ascending, which survives the one overlapping case there is: a host
    // echoing an argument back, whose run sits above the answer slot.
    m->slots.values[at] = run[0];
    m->slots.tags[at] = (uint8_t)LHAT_VALUE_HOSTVALUE;
    for (size_t i = 1; i < tag->width; i++) {
        m->slots.values[at + i] = run[i];
        m->slots.tags[at + i] = (uint8_t)LHAT_VALUE_CONT;
    }
    return true;
}

// 05 の 8.9改2: the value a host receives from a yield^ or a return^ at the
// base of a run -- the bytes moved into the machine's scratch (the frame
// that held them is going) and the head re-aimed there. Alive until the
// next call that runs the machine, which is the tuple positions' contract;
// a host that keeps it longer copies the bytes out (lhat_hostvalue_data).
LhatValue vm_hand_hostvalue_out(Machine *m, LhatValue value)
{
    const LhatValueUnion *run = value.as.hostvalue_run;
    const LhatHostValueTag *tag = run != NULL ? run[0].hostvalue : NULL;
    if (tag == NULL) {
        return lhat_nil();
    }
    for (size_t i = 0; i < tag->width; i++) {
        m->hostvalue_scratch[i] = run[i];
    }
    value.as.hostvalue_run = m->hostvalue_scratch;
    return value;
}

// 02 の 13.8改: the several values a host answered with, laid down at `at`
// as the run every other producer makes -- head slot naming the width, the
// positions after it. `reserved` is what the call site left room for (CALL's
// C), and it has to agree: a host and its caller can disagree only by being
// compiled against different registrations, and quietly writing the wrong
// count would leave the caller reading slots nobody wrote.
//
// The room is released here, which is what keeps one enough: the next host
// call finds it free. Answers false when the widths disagree or the run
// would not fit.
bool vm_place_run_answer(Machine *m, size_t at, size_t reserved,
                             LhatValue answered)
{
    size_t positions = lhat_run_width(answered);
    size_t held = m->tuple_scratch_count;
    m->tuple_scratch_count = 0;
    if (positions != held || reserved != positions + 1 ||
        at + positions >= m->slot_capacity) {
        return false;
    }
    lhat_slots_set(m->slots, at, answered);
    for (size_t i = 0; i < positions; i++) {
        lhat_slots_set(m->slots, at + 1 + i, m->tuple_scratch[i]);
    }
    return true;
}

// A resume's sent host value, moved into the machine's scratch before the
// suspension's registers are restored over the very slots that hold it --
// the pointer form has to aim somewhere the restore cannot reach.
LhatValue vm_stash_sent_hostvalue(Machine *m, size_t slot)
{
    const LhatHostValueTag *tag = m->slots.values[slot].hostvalue;
    for (size_t i = 0; i < tag->width; i++) {
        m->hostvalue_scratch[i] = m->slots.values[slot + i];
    }
    LhatValue v;
    v.tag = LHAT_VALUE_HOSTVALUE;
    v.as.hostvalue_run = m->hostvalue_scratch;
    return v;
}

// The registered field a string key names, or NULL.
const LhatHostValueField *vm_hostvalue_field_named(
    const LhatHostValueTag *tag, LhatValue key)
{
    if (!lhat_is_object_kind(key, LHAT_OBJECT_STRING)) {
        return NULL;
    }
    const LhatString *name = (const LhatString *)lhat_as_object(key);
    for (size_t i = 0; i < tag->field_count; i++) {
        if (strlen(tag->fields[i].name) == name->length &&
            memcmp(tag->fields[i].name, name->text, name->length) == 0) {
            return &tag->fields[i];
        }
    }
    return NULL;
}

// 'v.x' as a number^, off the data run that follows a head. Offsets were
// checked against the registered size when the field was. Public so the
// value writer spells a host value's content with it (value.c).
LhatValue lhat_hostvalue_field_value(const LhatValueUnion *data,
                                     const LhatHostValueField *field)
{
    const uint8_t *bytes = (const uint8_t *)data + field->offset;
    switch (field->kind) {
        case LHAT_HVFIELD_F32: {
            float f;
            memcpy(&f, bytes, sizeof f);
            return lhat_real((double)f);
        }
        case LHAT_HVFIELD_F64: {
            double d;
            memcpy(&d, bytes, sizeof d);
            return lhat_real(d);
        }
        case LHAT_HVFIELD_I8: {
            int8_t i;
            memcpy(&i, bytes, sizeof i);
            return lhat_integer(i);
        }
        case LHAT_HVFIELD_I16: {
            int16_t i;
            memcpy(&i, bytes, sizeof i);
            return lhat_integer(i);
        }
        case LHAT_HVFIELD_I32: {
            int32_t i;
            memcpy(&i, bytes, sizeof i);
            return lhat_integer(i);
        }
        case LHAT_HVFIELD_I64: {
            int64_t i;
            memcpy(&i, bytes, sizeof i);
            return lhat_integer(i);
        }
        case LHAT_HVFIELD_U8: {
            uint8_t u;
            memcpy(&u, bytes, sizeof u);
            return lhat_integer(u);
        }
        case LHAT_HVFIELD_U16: {
            uint16_t u;
            memcpy(&u, bytes, sizeof u);
            return lhat_integer(u);
        }
        case LHAT_HVFIELD_U32: {
            uint32_t u;
            memcpy(&u, bytes, sizeof u);
            return lhat_integer(u);
        }
    }
    return lhat_nil();
}

// And the write. False when the value is not a number^ -- 14.8's one type,
// either representation.
bool vm_hostvalue_field_set(LhatValueUnion *data,
                                const LhatHostValueField *field,
                                LhatValue value)
{
    if (!lhat_is_number(value)) {
        return false;
    }
    uint8_t *bytes = (uint8_t *)data + field->offset;
    double real = lhat_number_as_real(value);
    int64_t integer = lhat_is_integer(value) ? lhat_as_integer(value)
                                             : (int64_t)real;
    switch (field->kind) {
        case LHAT_HVFIELD_F32: {
            float f = (float)real;
            memcpy(bytes, &f, sizeof f);
            return true;
        }
        case LHAT_HVFIELD_F64:
            memcpy(bytes, &real, sizeof real);
            return true;
        case LHAT_HVFIELD_I8: {
            int8_t i = (int8_t)integer;
            memcpy(bytes, &i, sizeof i);
            return true;
        }
        case LHAT_HVFIELD_I16: {
            int16_t i = (int16_t)integer;
            memcpy(bytes, &i, sizeof i);
            return true;
        }
        case LHAT_HVFIELD_I32: {
            int32_t i = (int32_t)integer;
            memcpy(bytes, &i, sizeof i);
            return true;
        }
        case LHAT_HVFIELD_I64:
            memcpy(bytes, &integer, sizeof integer);
            return true;
        case LHAT_HVFIELD_U8: {
            uint8_t u = (uint8_t)integer;
            memcpy(bytes, &u, sizeof u);
            return true;
        }
        case LHAT_HVFIELD_U16: {
            uint16_t u = (uint16_t)integer;
            memcpy(bytes, &u, sizeof u);
            return true;
        }
        case LHAT_HVFIELD_U32: {
            uint32_t u = (uint32_t)integer;
            memcpy(bytes, &u, sizeof u);
            return true;
        }
    }
    return false;
}
bool lhat_machine_make_table(LhatMachine *machine, LhatValue *out)
{
    LhatTable *table = lhat_table_new(&machine->objects);
    if (table == NULL) {
        return false;
    }
    *out = lhat_object((LhatObject *)table);
    return true;
}

bool lhat_machine_table_set(LhatMachine *machine, LhatTable *table,
                            LhatValue key, LhatValue value, bool *refused)
{
    bool ignored = false;
    if (machine == NULL || table == NULL) {
        return false;
    }
    return vm_set_key(machine, table, key, value,
                   refused != NULL ? refused : &ignored);
}

bool lhat_machine_make_host(LhatMachine *machine, LhatHostFn call,
                            void *context, uint8_t parameters,
                            bool has_variadic, bool takes_self, bool self_last,
                            LhatRuntimeType **parameter_types, LhatValue *out)
{
    LhatHost *host = lhat_host_new(&machine->objects, call, context, parameters,
                                   has_variadic, takes_self);
    if (host == NULL) {
        lhat_free(parameter_types);
        return false;
    }
    host->self_last = self_last;
    host->parameter_types = parameter_types;
    *out = lhat_object((LhatObject *)host);
    return true;
}

// 05 の 8.7: the same walk the unit prologue compiles to, done in C because
// nothing is being compiled here. 02 の 8.8's rule holds: a table is made
// where the path does not reach one, and what is there is left alone.
static LhatTable *reach_table(Machine *m, LhatTable *owner, const char *path)
{
    for (const char *segment = path;;) {
        size_t length = strcspn(segment, ".");
        // Asked by bytes first: a string key's equality is its bytes, so a
        // hit needs no key object -- and every registration after the first
        // of a path is a hit. But that read sees ONE table's hash half,
        // where the real one also climbs `definition`, steps past a
        // reserved seat and follows a delegate (object.c's table_get_in).
        //
        // A miss here is DESTRUCTIVE: what follows makes a table and writes
        // it over whatever the path named. So a miss is asked again with
        // the read that decides what a name means, and only a miss on THAT
        // makes anything.
        LhatValue found = lhat_table_get_bytes(owner, segment, length);
        LhatTable *next = vm_table_of(found);
        if (next == NULL && lhat_is_nil(found)) {
            LhatString *key = lhat_string_new(&m->objects, segment, length);
            if (key == NULL) {
                return NULL;
            }
            LhatValue held =
                lhat_table_get(owner, lhat_object((LhatObject *)key));
            next = vm_table_of(held);
            if (next == NULL) {
                if (!lhat_is_nil(held)) {
                    return NULL;  // something that is not a table is there
                }
                next = lhat_table_new(&m->objects);
                bool refused = false;
                if (next == NULL ||
                    !vm_set_key(m, owner, lhat_object((LhatObject *)key),
                             lhat_object((LhatObject *)next), &refused) ||
                    refused) {
                    return NULL;
                }
            }
        } else if (next == NULL) {
            return NULL;  // something that is not a table is there
        }
        owner = next;
        if (segment[length] == '\0') {
            return owner;
        }
        segment += length + 1;
    }
}

bool lhat_machine_make_hostdata(LhatMachine *machine, const LhatHostDataTag *tag,
                            void *pointer, LhatValue *out)
{
    if (machine == NULL || tag == NULL) {
        return false;
    }
    // The members live where the registration put them, which is under the
    // type's own name in L^.modules -- so what answers a call on this value
    // is the same table every other value of the type answers through.
    LhatTable *registry = machine->modules;
    if (registry == NULL) {
        return false;
    }
    LhatTable *module = reach_table(machine, registry, tag->module);
    LhatTable *members =
        module != NULL ? reach_table(machine, module, tag->name) : NULL;
    if (members == NULL) {
        return false;
    }

    LhatHostData *data = lhat_hostdata_new(&machine->objects, tag, pointer, members);
    if (data == NULL) {
        return false;
    }
    *out = lhat_object((LhatObject *)data);
    return true;
}

void *lhat_hostdata_pointer(LhatValue value, const LhatHostDataTag *tag)
{
    if (!lhat_is_object_kind(value, LHAT_OBJECT_HOSTDATA)) {
        return NULL;
    }
    const LhatHostData *data = (const LhatHostData *)lhat_as_object(value);
    // 05 の 8.8改: a tag declared under `tag` answers too. That is the
    // promise lhat_register_hostdata_subtype took -- a pointer of the
    // derived type may be read as one of the base's -- and refusing it here
    // would leave the host unable to use the relation it declared.
    for (const LhatHostDataTag *at = data->tag; at != NULL; at = at->base) {
        if (at == tag) {
            return data->pointer;
        }
    }
    return NULL;
}

void *lhat_hostvalue_data(LhatValue argument, const LhatHostValueTag *tag)
{
    if (!lhat_is_hostvalue(argument) || tag == NULL) {
        return NULL;
    }
    const LhatValueUnion *run = argument.as.hostvalue_run;
    if (run == NULL || run[0].hostvalue != tag) {
        return NULL;
    }
    // The head carries the tag; the bytes start one slot later. The cast
    // drops const on purpose: the run is a scratch copy of the caller's,
    // and 8.9 lets a host read or write it freely for the call's duration.
    return (void *)(run + 1);
}

bool lhat_make_hostvalue(LhatMachine *machine, const LhatHostValueTag *tag,
                         const void *bytes, LhatValue *out)
{
    if (machine == NULL || tag == NULL || bytes == NULL || out == NULL ||
        tag->size > LHAT_HOSTVALUE_MAX_BYTES) {
        return false;
    }
    LhatValueUnion *run = machine->hostvalue_scratch;
    // The data run's tail is zeroed so equality can compare whole slots.
    memset(run, 0, tag->width * sizeof *run);
    run[0].hostvalue = tag;
    memcpy(run + 1, bytes, tag->size);
    out->tag = LHAT_VALUE_HOSTVALUE;
    out->as.hostvalue_run = run;
    return true;
}

bool lhat_machine_make_enum(LhatMachine *machine, const char *name,
                            const struct LhatRuntimeType *decl,
                            const char *const *members,
                            const int64_t *values, size_t count,
                            LhatValue *out)
{
    Machine *m = (Machine *)machine;
    if (m == NULL || name == NULL || members == NULL || out == NULL) {
        return false;
    }
    LhatString *named = lhat_string_new(&m->objects, name, strlen(name));
    if (named == NULL) {
        return false;
    }
    LhatEnum *made = lhat_enum_new(&m->objects, named, decl);
    if (made == NULL) {
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        LhatString *member =
            lhat_string_new(&m->objects, members[i], strlen(members[i]));
        if (member == NULL) {
            return false;
        }
        LhatEnumerator *e = lhat_enumerator_new(
            &m->objects, made, member,
            lhat_integer(values != NULL ? values[i] : (int64_t)i + 1),
            i + 1);
        if (e == NULL) {
            return false;
        }
        bool refused = false;
        if (!vm_set_key(m, made->members, lhat_object((LhatObject *)member),
                     lhat_object((LhatObject *)e), &refused)) {
            return false;
        }
    }
    *out = lhat_object((LhatObject *)made);
    return true;
}

bool lhat_place_hostvalue(const LhatHostValueTag *tag, const void *bytes,
                          LhatHostValueRoom *room, LhatValue *out)
{
    if (tag == NULL || bytes == NULL || room == NULL || out == NULL ||
        tag->size > LHAT_HOSTVALUE_MAX_BYTES) {
        return false;
    }
    memset(room->run, 0, tag->width * sizeof room->run[0]);
    room->run[0].hostvalue = tag;
    memcpy(room->run + 1, bytes, tag->size);
    out->tag = LHAT_VALUE_HOSTVALUE;
    out->as.hostvalue_run = room->run;
    return true;
}
bool lhat_machine_bind_hostvalues(LhatMachine *machine,
                                  const LhatHostValueTypeEntry *entries,
                                  size_t count, size_t slots)
{
    if (machine == NULL || entries == NULL || count == 0 || slots == 0) {
        return false;
    }
    // 05 の 8.9: a tag's index is the process's and not this program's
    // (registry.h), so a program declaring only some of the host value types
    // leaves gaps. The array is taken to the width the indices reach rather
    // than to how many this program declared, and a gap stays NULL -- no
    // value of a type this program never declared can reach a machine of it.
    LhatTable **tables = (LhatTable **)lhat_calloc(slots, sizeof *tables);
    if (tables == NULL) {
        return false;
    }
    LhatTable *registry = machine->modules;
    if (registry == NULL) {
        lhat_free(tables);
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        // The same walk lhat_machine_make_hostdata does for a hostdata
        // type's members: the table registration put under the type's own
        // name is the type's members table.
        LhatTable *module = reach_table(machine, registry, entries[i].tag->module);
        LhatTable *members =
            module != NULL ? reach_table(machine, module, entries[i].tag->name)
                           : NULL;
        if (members == NULL || entries[i].tag->index >= slots) {
            lhat_free(tables);
            return false;
        }
        tables[entries[i].tag->index] = members;
    }
    lhat_free(machine->hostvalue_members);  // a second install replaces
    machine->hostvalue_members = tables;
    machine->hostvalue_member_count = slots;
    return true;
}

// 05 の 5.7: the read-only half of reach_table -- walk to what is there and
// answer NULL where nothing is, making nothing on the way. A path that does
// not reach a table is a path with nothing to forget.
static LhatTable *table_at(Machine *m, LhatTable *owner, const char *segment,
                           size_t length)
{
    LhatString *key = lhat_string_new(&m->objects, segment, length);
    if (key == NULL) {
        return NULL;
    }
    return vm_table_of(lhat_table_get(owner, lhat_object((LhatObject *)key)));
}

bool lhat_machine_forget_unit(LhatMachine *machine, const char *module)
{
    if (machine == NULL || machine->environment == NULL || module == NULL ||
        *module == '\0') {
        return false;
    }
    LhatTable *owner = machine->modules;
    if (owner == NULL) {
        return false;
    }

    // Down to the table the last segment sits in. The ones above it stay:
    // other modules live under them, and 5.3's guard tests each step for
    // nil^ anyway, so an empty table left behind means the same as no table.
    const char *segment = module;
    size_t length = strcspn(segment, ".");
    while (segment[length] == '.') {
        owner = table_at(machine, owner, segment, length);
        if (owner == NULL) {
            return false;
        }
        segment += length + 1;
        length = strcspn(segment, ".");
    }

    LhatString *last = lhat_string_new(&machine->objects, segment, length);
    if (last == NULL) {
        return false;
    }
    LhatValue key = lhat_object((LhatObject *)last);
    if (lhat_is_nil(lhat_table_get(owner, key))) {
        return false;  // nothing stood there
    }
    // 04 の 11.3 spells "not there" nil^, which is exactly what 5.3's guard
    // tests -- so storing nil^ is the whole of forgetting.
    bool refused = false;
    return vm_set_key(machine, owner, key, lhat_nil(), &refused) && !refused;
}

// 05 の 8.7 の読み: the path walked without making anything. reach_table
// above creates the tables a registration needs; a read that created one
// would answer nil^ for a name and leave a table behind saying it had been
// asked for.
// The read-only twin of reach_table. It has a machine, so it asks the read
// that decides what a name means rather than the narrow one -- otherwise a
// member a type inherits (05 の 8.8改) reads as not registered while a call
// on the value finds it.
static const LhatTable *find_table(Machine *m, const LhatTable *owner,
                                   const char *path)
{
    for (const char *segment = path;;) {
        size_t length = strcspn(segment, ".");
        LhatString *key = lhat_string_new(&m->objects, segment, length);
        if (key == NULL) {
            return NULL;
        }
        const LhatTable *next =
            vm_table_of(lhat_table_get(owner, lhat_object((LhatObject *)key)));
        if (next == NULL) {
            return NULL;
        }
        owner = next;
        if (segment[length] == '\0') {
            return owner;
        }
        segment += length + 1;
    }
}

bool lhat_machine_registered(LhatMachine *machine, const char *module,
                             const char *type, const char *name,
                             LhatValue *out)
{
    if (machine == NULL || machine->environment == NULL || module == NULL ||
        name == NULL || out == NULL) {
        return false;
    }
    *out = lhat_nil();
    Machine *m = (Machine *)machine;
    const LhatTable *owner = m->modules;
    if (owner == NULL) {
        return false;
    }
    owner = find_table(m, owner, module);
    if (owner != NULL && type != NULL) {
        owner = find_table(m, owner, type);
    }
    if (owner == NULL) {
        return false;
    }
    LhatString *last = lhat_string_new(&m->objects, name, strlen(name));
    LhatValue held =
        last != NULL ? lhat_table_get(owner, lhat_object((LhatObject *)last))
                     : lhat_nil();
    if (lhat_is_nil(held)) {
        return false;
    }
    *out = held;
    return true;
}

bool lhat_machine_register(LhatMachine *machine, const char *module,
                           const char *type, const char *name, LhatValue value)
{
    if (machine == NULL || machine->environment == NULL) {
        return false;
    }
    LhatTable *owner = machine->modules;
    if (owner == NULL) {
        return false;
    }

    owner = reach_table(machine, owner, module);
    if (owner != NULL && type != NULL) {
        owner = reach_table(machine, owner, type);
    }
    if (owner == NULL) {
        return false;
    }

    LhatString *key = lhat_string_new(&machine->objects, name, strlen(name));
    if (key == NULL) {
        return false;
    }
    // 02 の 14.12: a second registration of one name is another arm, not a
    // replacement -- the same thing OVERLOADINDEX makes of a member written
    // twice, built here because a registration compiles to no instruction.
    // 05 の 8.7 has already refused any pair that could take the same call.
    LhatValue held = lhat_table_get(owner, lhat_object((LhatObject *)key));
    if (!lhat_is_nil(held)) {
        LhatOverload *group = NULL;
        if (lhat_is_object_kind(held, LHAT_OBJECT_OVERLOAD)) {
            group = (LhatOverload *)lhat_as_object(held);
        } else {
            group = lhat_overload_new(&machine->objects);
            if (group == NULL || !lhat_overload_add(group, held)) {
                return false;
            }
        }
        if (!lhat_overload_add(group, value)) {
            return false;
        }
        value = lhat_object((LhatObject *)group);
    }
    bool refused = false;
    return vm_set_key(machine, owner, lhat_object((LhatObject *)key), value,
                   &refused) && !refused;
}

// 05 の 8.7改5: the machine's half of the join. The path above the module
// is the machine's to make (a unit's own module^ lands in the same spine),
// and the module itself is a pointer to what the program built.
bool lhat_machine_attach_module(LhatMachine *machine, const char *path,
                               LhatTable *table)
{
    Machine *m = (Machine *)machine;
    if (m == NULL || m->modules == NULL || path == NULL || table == NULL) {
        return false;
    }
    const char *last = strrchr(path, '.');
    LhatTable *owner = m->modules;
    if (last != NULL) {
        size_t above = (size_t)(last - path);
        char spine[LHAT_QUALIFIED_NAME_BUFFER];
        if (above >= sizeof spine) {
            return false;
        }
        memcpy(spine, path, above);
        spine[above] = '\0';
        owner = reach_table(m, owner, spine);
        if (owner == NULL) {
            return false;
        }
        last++;
    } else {
        last = path;
    }
    LhatString *key = lhat_string_new(&m->objects, last, strlen(last));
    if (key == NULL) {
        return false;
    }
    // A black table written into a white one: the backward barrier asks
    // whether the VALUE is white, and this one never is, so nothing is
    // threaded onto anybody's gray list. The other direction -- a machine's
    // object written into the shared table -- is what the seal refuses.
    bool refused = false;
    return vm_set_key(m, owner, lhat_object((LhatObject *)key),
                   lhat_object((LhatObject *)table), &refused) &&
           !refused;
}

// 05 の 8.8改: puts a registered type's members table under its base's, so
// that a member the base declared is found by WALKING rather than by having
// been copied down when the program was installed. Copying meant a pass over
// every registration for every type and every one of its ancestors -- a
// binding with a class per engine class waited seconds for it -- and it was
// a second copy of what the checker had already stopped making (the base
// link on the type, type.h).
//
// `definition` is the link table_get_in already climbs, and `is_definition`
// is what tells it this climb is 14.5's walk up to a base and passes
// everything, rather than 14.7's walk from an instance which lets only what
// takes a receiver through. Nearest wins by the order of the walk, which is
// what the copy had to work out by hand.
bool lhat_machine_link_hostdata_base(LhatMachine *machine,
                                     const char *module, const char *name,
                                     const char *base_module,
                                     const char *base_name)
{
    Machine *m = (Machine *)machine;
    if (m == NULL || m->environment == NULL) {
        return false;
    }
    LhatTable *root = m->modules;
    if (root == NULL) {
        return false;
    }
    LhatTable *owner = reach_table(m, root, module);
    LhatTable *derived = owner != NULL ? reach_table(m, owner, name) : NULL;
    LhatTable *base_owner = reach_table(m, root, base_module);
    LhatTable *base =
        base_owner != NULL ? reach_table(m, base_owner, base_name) : NULL;
    if (derived == NULL || base == NULL || derived == base) {
        return false;
    }
    derived->definition = base;
    derived->is_definition = true;
    base->is_definition = true;
    return true;
}

bool lhat_machine_set_global(LhatMachine *machine, const char *name,
                             LhatValue value)
{
    Machine *m = (Machine *)machine;
    if (m == NULL || m->environment == NULL || name == NULL) {
        return false;
    }
    // 05 の 8.6: vm_set_key rather than an instruction, so the seal on L^ is
    // not in the way -- what it refuses is what a program writes, and this is
    // the host writing what the program will read.
    return vm_set_member(m, m->environment, name, value);
}
bool lhat_machine_make_string(LhatMachine *machine, const char *text,
                              size_t length, LhatValue *out)
{
    Machine *m = (Machine *)machine;
    LhatString *string = lhat_string_new(&m->objects, text, length);
    if (string == NULL) {
        return false;
    }
    *out = lhat_object((LhatObject *)string);
    return true;
}

bool lhat_machine_make_closure(LhatMachine *machine, const LhatProto *proto,
                               LhatValue *out)
{
    Machine *m = (Machine *)machine;
    if (m == NULL || proto == NULL || proto->upvalue_count != 0) {
        return false;
    }
    LhatClosure *closure = (LhatClosure *)lhat_object_alloc(
        &m->objects, sizeof *closure, LHAT_OBJECT_SUBROUTINE);
    if (closure == NULL) {
        return false;
    }
    closure->proto = proto;
    // calloc 起源(lhat_object_alloc)なので upvalues/upvalue_count は
    // 既に NULL/0 -- proto->upvalue_count == 0 の前提と一致する。
    *out = lhat_object((LhatObject *)closure);
    return true;
}

static void own_tree(LhatProto *proto, LhatObject *owner)
{
    proto->owner = owner;
    for (size_t i = 0; i < proto->proto_count; i++) {
        own_tree(proto->protos[i], owner);
    }
}

bool lhat_machine_adopt_script(LhatMachine *machine, LhatProto *proto,
                               LhatValue *out)
{
    Machine *m = (Machine *)machine;
    if (m == NULL || proto == NULL || proto->upvalue_count != 0) {
        return false;
    }
    LhatLoadedScript *script = (LhatLoadedScript *)lhat_object_alloc(
        &m->objects, sizeof *script, LHAT_OBJECT_SCRIPT);
    if (script == NULL) {
        return false;
    }
    script->root = proto;
    own_tree(proto, (LhatObject *)script);
    // The collector runs between instructions and no further, so the script
    // is still there when the closure that reaches it is made.
    return lhat_machine_make_closure(machine, proto, out);
}

void lhat_machine_panic(LhatMachine *machine, LhatValue value)
{
    Machine *m = (Machine *)machine;
    if (m == NULL) {
        return;
    }
    // Rooted the way a fault's value is (gc.c), and read back by
    // vm_host_faulted when the host function returns.
    m->fault_value = value;
    m->host_panicked = true;
}

bool lhat_machine_panic_text(LhatMachine *machine, const char *text)
{
    LhatValue message = lhat_nil();
    if (!lhat_machine_make_string(machine, text, strlen(text), &message)) {
        return false;
    }
    lhat_machine_panic(machine, message);
    return true;
}

bool lhat_machine_make_cell(LhatMachine *machine, LhatValue held,
                            LhatUpvalue **out)
{
    Machine *m = (Machine *)machine;
    if (m == NULL || out == NULL) {
        return false;
    }
    LhatUpvalue *cell = (LhatUpvalue *)lhat_object_alloc(
        &m->objects, sizeof *cell, LHAT_OBJECT_UPVALUE);
    if (cell == NULL) {
        return false;
    }
    lhat_ref_set(lhat_upvalue_closed_ref(cell), held);
    cell->location = lhat_upvalue_closed_ref(cell);
    cell->next_open = NULL;
    *out = cell;
    return true;
}

bool lhat_machine_make_closure_with(LhatMachine *machine,
                                    const LhatProto *proto,
                                    LhatUpvalue *const *cells, size_t count,
                                    LhatValue *out)
{
    Machine *m = (Machine *)machine;
    if (m == NULL || proto == NULL || out == NULL ||
        proto->upvalue_count != count || (count > 0 && cells == NULL)) {
        return false;
    }
    LhatClosure *closure = (LhatClosure *)lhat_object_alloc(
        &m->objects, sizeof *closure, LHAT_OBJECT_SUBROUTINE);
    if (closure == NULL) {
        return false;
    }
    closure->proto = proto;
    closure->upvalue_count = count;
    if (count > 0) {
        closure->upvalues =
            (LhatUpvalue **)lhat_calloc(count, sizeof *closure->upvalues);
        if (closure->upvalues == NULL) {
            return false;
        }
        for (size_t i = 0; i < count; i++) {
            if (cells[i] == NULL) {
                return false;
            }
            closure->upvalues[i] = cells[i];
        }
    }
    *out = lhat_object((LhatObject *)closure);
    return true;
}

const LhatProto *lhat_closure_proto(LhatValue closure)
{
    if (!lhat_is_object_kind(closure, LHAT_OBJECT_SUBROUTINE)) {
        return NULL;
    }
    return ((const LhatClosure *)lhat_as_object(closure))->proto;
}

size_t lhat_closure_capture_count(LhatValue closure)
{
    if (!lhat_is_object_kind(closure, LHAT_OBJECT_SUBROUTINE)) {
        return 0;
    }
    return ((const LhatClosure *)lhat_as_object(closure))->upvalue_count;
}

LhatValue lhat_closure_capture(LhatValue closure, size_t index)
{
    if (!lhat_is_object_kind(closure, LHAT_OBJECT_SUBROUTINE)) {
        return lhat_nil();
    }
    const LhatClosure *held = (const LhatClosure *)lhat_as_object(closure);
    if (index >= held->upvalue_count || held->upvalues[index] == NULL) {
        return lhat_nil();
    }
    return lhat_ref_get(held->upvalues[index]->location);
}

const void *lhat_closure_capture_id(LhatValue closure, size_t index)
{
    if (!lhat_is_object_kind(closure, LHAT_OBJECT_SUBROUTINE)) {
        return NULL;
    }
    const LhatClosure *held = (const LhatClosure *)lhat_as_object(closure);
    return index < held->upvalue_count ? held->upvalues[index] : NULL;
}

bool lhat_machine_make_error(LhatMachine *machine, const LhatErrorKind *kind,
                             const char *message, LhatValue cause,
                             LhatValue *out)
{
    Machine *m = (Machine *)machine;
    if (m == NULL || kind == NULL) {
        return false;
    }
    // Same shape LHAT_BC_NEWERROR builds, and the same field defaults
    // compile_error_new writes for what a construction left out (04 の
    // 2.3): every kind has message and cause without either being declared.
    LhatError *error = lhat_error_new(&m->objects, kind);
    if (error == NULL) {
        return false;
    }
    LhatString *message_key = lhat_string_new(&m->objects, "message", 7);
    LhatString *message_text = lhat_string_new(
        &m->objects, message != NULL ? message : "",
        message != NULL ? strlen(message) : 0);
    if (message_key == NULL || message_text == NULL) {
        return false;
    }
    bool refused = false;
    if (!vm_set_key(m, error->fields, lhat_object((LhatObject *)message_key),
                 lhat_object((LhatObject *)message_text), &refused) ||
        refused) {
        return false;
    }
    if (!lhat_is_nil(cause)) {
        LhatString *cause_key = lhat_string_new(&m->objects, "cause", 5);
        if (cause_key == NULL) {
            return false;
        }
        if (!vm_set_key(m, error->fields, lhat_object((LhatObject *)cause_key),
                     cause, &refused) ||
            refused) {
            return false;
        }
    }
    *out = lhat_object((LhatObject *)error);
    return true;
}

const char *lhat_run_status_message(LhatRunStatus status)
{
    switch (status) {
        case LHAT_RUN_OK:              return "ran";
        case LHAT_RUN_TYPE_ERROR:      return "an instruction was given the wrong type";
        case LHAT_RUN_NOT_CALLABLE:    return "this is not a subroutine";
        case LHAT_RUN_ARITY:           return "the wrong number of arguments";
        case LHAT_RUN_STACK_OVERFLOW:  return "the calls went too deep";
        case LHAT_RUN_OUT_OF_MEMORY:   return "out of memory";
        case LHAT_RUN_BAD_KEY:         return "this cannot be a key";
        case LHAT_RUN_SEALED:
            return "this table belongs to the machine; what it holds is "
                   "written by the host, not from here";
        // 02 の 14.11
        case LHAT_RUN_MUTABLE_DEFAULT:
            return "a field's default lives on the prototype: an immutable "
                   "value, a table of its own (each instance is given a "
                   "copy), or a definition; what something else made is "
                   "given inside new";
        case LHAT_RUN_BAD_FORMAT:
            return "a number^ is written through one numeric conversion; "
                   "write '%d' or '%g' and no length of your own";
        case LHAT_RUN_DEAD_COROUTINE:  return "this coroutine has finished";
        case LHAT_RUN_NO_SUCH_UNIT:
            return "this machine was not given the unit this require^ asks for";
        case LHAT_RUN_YIELD_OUTSIDE:   return "nothing is waiting for this yield^";
        case LHAT_RUN_NO_CANDIDATE:    return "no way of calling this member takes these arguments";
        case LHAT_RUN_COROUTINE_NOT_STARTED:     return "resume needs start() first";
        case LHAT_RUN_COROUTINE_ALREADY_STARTED: return "start() only works before the first resume";
        // 04 の 11.6: a placeholder for a caller that only wants this
        // status's own name -- the actual message is what the program
        // panicked with, in LhatRunResult.value, which this cannot see.
        case LHAT_RUN_PANIC:                     return "panic^";
        case LHAT_RUN_SUSPENDED:                 return "the slice ran out";
        // 02 の 13.8改
        case LHAT_RUN_TUPLE_ARITY:
            return "this call and what it called disagree on how many values "
                   "come back";
        case LHAT_RUN_TUPLE_UNEXPECTED:
            return "this answered several values where one was expected; "
                   "pack^ makes a table of them";
    }
    return "unknown";
}
