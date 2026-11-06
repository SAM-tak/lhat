// L^ (lhat) -- a program: what the host registered, and the registrations built once on its heap.

#include "message.h"
#include "program_internal.h"
#include "registry.h"

#include <stdlib.h>
#include <string.h>

#include "gc.h"  // LHAT_GC_BLACK -- host_heap の初期色 ([errors.grammar.types])
#include "grow.h"
#include "lhat/error.h"  // one shape for what a stage reported ([pipeline.stages.tree])
#include "lhat/port.h"
#include "rttype.h"
#include "type.h"
#include "lhat/vm.h"

// ---------------------------------------------------------------------------
// [modules.host.import]: what the host provides
// ---------------------------------------------------------------------------


// [modules.host.machine]: what the host registered is the machine's record of it. Nothing
// written in L^ adds to it or writes over it -- the host reaches it through
// this file's own API instead.
static LhatType *hosted_namespace(LhatProgram *program)
{
    LhatType *made = lhat_type_table(&program->types);
    if (made != NULL) {
        made->v.table.sealed = true;
        made->v.table.is_namespace = true;  // and named through, not held
        made->v.table.is_module = true;
    }
    return made;
}

static LhatType *hosted_root(LhatProgram *program)
{
    if (program->hosted == NULL) {
        program->hosted = hosted_namespace(program);
    }
    return program->hosted;
}

// [modules.host.import]: the table the unit at a dotted path published into, made where
// the path does not reach one. The tree is two tables a step: the step's own
// members are the units named under it, and what the unit there published is
// its `base` -- so an import^ can be handed the one without the other, while
// a walk of a whole path (lhat_check_type_named) reaches both. A step with no
// `base` is only on the way to a unit and is none itself.
//
// `name`, when given, is the name about to be published there, and is
// refused where a unit under this one already stands -- [modules.host.import]'s one name, one
// thing, across the two tables. The walk refuses the other order.
static LhatType *hosted_unit(LhatProgram *program, const char *module,
                             const char *name)
{
    LhatType *step = hosted_root(program);
    for (const char *segment = module; step != NULL;) {
        size_t length = strcspn(segment, ".");
        if (lhat_type_own_member(step->v.table.base, segment, length) != NULL) {
            return NULL;  // the unit above published this name already
        }
        const LhatTypeMember *found =
            lhat_type_own_member(step, segment, length);
        LhatType *next = found != NULL ? found->type : NULL;
        if (next == NULL) {
            next = hosted_namespace(program);
            if (next == NULL ||
                lhat_type_add_member(&program->types, step, segment, length,
                                     next) == NULL) {
                return NULL;
            }
        }
        step = next;
        if (segment[length] == '\0') {
            break;
        }
        segment += length + 1;
    }
    if (step == NULL ||
        (name != NULL && lhat_type_own_member(step, name, strlen(name)) != NULL)) {
        return NULL;
    }
    if (step->v.table.base == NULL) {
        step->v.table.base = hosted_namespace(program);
    }
    return step->v.table.base;
}

static const LhatTypeMember *hosted_member(const LhatType *table,
                                           const char *name)
{
    // [modules.hostvalue]: a host value type keeps its members in the same half of the
    // union a table does, and [modules.host.import] registers into one exactly as into the
    // other -- so both are searched here.
    //
    // The type's OWN members and no link: [modules.hostdata.base] puts a derived type under
    // its base, and registering a name the base already has is registering
    // it here, not finding it there.
    return lhat_type_own_member(table, name, strlen(name));
}

// [syntax.obj.override] with [modules.host.import]: a registration's signature as the descriptors
// the machine's overload search reads -- rttype.c's conversion, onto the
// program's own heap (host_heap), once, at registration. A parameter whose
// descriptor comes back NULL is not compared, so the search falls back on
// the counts. [syntax.type.param-names] keeps self^ out of the list either way, so the count is
// the same one the entry recorded.
static LhatRuntimeType **lower_host_params(LhatProgram *program,
                                           const LhatType *signature,
                                           uint8_t count)
{
    if (count == 0 || signature == NULL ||
        signature->kind != LHAT_TYPE_FUNC) {
        return NULL;
    }
    LhatRuntimeType **types =
        (LhatRuntimeType **)lhat_alloc((size_t)count * sizeof *types);
    if (types == NULL) {
        return NULL;
    }
    size_t at = 0;
    for (const LhatTypeList *p = signature->v.func.params;
         p != NULL && at < count; p = p->next) {
        types[at++] = lhat_rt_from_checked(&program->host_heap, p->type, NULL);
    }
    while (at < count) {
        types[at++] = NULL;
    }
    return types;
}

// What a machine's LhatHost takes over: its own copy of the pointers, since
// the host frees the array with itself, while the nodes stay the program's.
static LhatRuntimeType **borrowed_params(const LhatRuntimeType *const *types,
                                         uint8_t count)
{
    if (types == NULL || count == 0) {
        return NULL;
    }
    LhatRuntimeType **copy =
        (LhatRuntimeType **)lhat_alloc((size_t)count * sizeof *copy);
    if (copy != NULL) {
        memcpy(copy, types, (size_t)count * sizeof *copy);
    }
    return copy;
}

static bool keep_entry(LhatProgram *program, const char *module,
                       const char *type, const char *name, LhatHostFn call,
                       void *context, const LhatType *signature,
                       const char *signature_text)
{
    LHAT_GROW(program->host_entries, program->host_entry_count,
              program->host_entry_capacity, 8, return false);

    LhatHostEntry *entry = &program->host_entries[program->host_entry_count];
    memset(entry, 0, sizeof *entry);
    entry->module = prg_duplicate(module);
    entry->name = prg_duplicate(name);
    entry->type = type != NULL ? prg_duplicate(type) : NULL;
    entry->signature_text =
        signature_text != NULL ? prg_duplicate(signature_text) : NULL;
    entry->call = call;
    entry->context = context;
    if (signature != NULL && signature->kind == LHAT_TYPE_FUNC) {
        size_t count = 0;
        for (const LhatTypeList *p = signature->v.func.params; p != NULL;
             p = p->next) {
            count++;
        }
        entry->parameters = (uint8_t)count;
        // [syntax.type.variadic]: '...' is kept apart from the parameter list (check.c's
        // resolve_func_type), so what is counted above is the floor and this
        // is what says the count is one.
        entry->has_variadic = signature->v.func.variadic != NULL;
        entry->takes_self = signature->v.func.takes_self;
        entry->self_last = signature->v.func.self_last;
        entry->runtime_signature = lhat_rt_from_checked(&program->host_heap, signature, NULL);
        if (entry->runtime_signature == NULL) return false;
        entry->parameter_types =
            lower_host_params(program, signature, entry->parameters);
    }
    if (entry->module == NULL || entry->name == NULL ||
        (type != NULL && entry->type == NULL) ||
        (signature_text != NULL && entry->signature_text == NULL) ||
        (entry->parameters > 0 && entry->parameter_types == NULL)) {
        return false;
    }
    program->host_entry_count++;
    return true;
}

const LhatHostDataTag *lhat_register_hostdata_type(LhatProgram *program,
                                                   const char *module,
                                                   const char *name)
{
    LhatType *table = hosted_unit(program, module, name);
    if (table == NULL || hosted_member(table, name) != NULL) {
        return NULL;  // [modules.host.import]: one name, one thing
    }

    // [modules.label.errors]'s shape: what makes it its own type is the declaration, not
    // the members, so two host types that look alike stay apart. [syntax.stmt.member]'s
    // mark keeps a member from being added to it afterwards.
    LhatType *made = lhat_type_table(&program->types);
    if (made == NULL) {
        return NULL;
    }
    made->v.table.from_definition = true;
    made->v.table.nominal = true;
    if (lhat_type_add_member(&program->types, table, name, strlen(name),
                             made) == NULL ||
        !keep_entry(program, module, NULL, name, NULL, NULL, NULL, NULL)) {
        return NULL;
    }

    // [modules.hostdata]: the tag is identity and nothing else, so there is exactly one per
    // DECLARATION -- and the declaration is this C call, however many
    // programs make it. The registry (registry.h) is where the one lives; a
    // second program registering std.io.File comes away with the same tag,
    // which is what lets the two agree at run time about what a value is.
    LhatHostEntry *entry = &program->host_entries[program->host_entry_count - 1];
    entry->tag = (LhatHostDataTag *)lhat_registry_hostdata(module, name);
    if (entry->tag == NULL) {
        return NULL;
    }
    // And the checker's type says which declaration it is, so what writes a
    // type out can name it rather than spelling the shape -- [modules.hostdata] puts
    // identity in the declaration, and a shape written in its place is a
    // wider type that anything of the same members satisfies.
    made->v.table.hostdata_tag = entry->tag;

    // [modules.hostdata] の fits^ 版: vm.c がコンパイル時に "module.Name" から
    // 引けるよう、host_entries(非公開)とは別に vm.h の形へ薄く複製する。
    // 文字列は entry->module/name(host_entries が所有)をそのまま指す
    // だけ -- ここでは複製しない。program_dispose も host_type_entries
    // 側の文字列は解放しない。
    if (program->host_type_entry_count == program->host_type_entry_capacity) {
        size_t grown = program->host_type_entry_capacity
                           ? program->host_type_entry_capacity * 2
                           : 4;
        LhatHostTypeEntry *bigger = (LhatHostTypeEntry *)lhat_realloc(
            program->host_type_entries, grown * sizeof *bigger);
        if (bigger == NULL) {
            return NULL;
        }
        program->host_type_entries = bigger;
        program->host_type_entry_capacity = grown;
    }
    LhatHostTypeEntry *type_entry =
        &program->host_type_entries[program->host_type_entry_count++];
    type_entry->module = entry->module;
    type_entry->name = entry->name;
    type_entry->tag = entry->tag;
    // The array grows with lhat_realloc, which does not zero, so every field
    // is written here or it is whatever was in that memory.
    type_entry->flattened = false;

    return entry->tag;
}

bool lhat_register_type(LhatProgram *program, const char *module,
                        const char *name)
{
    return lhat_register_hostdata_type(program, module, name) != NULL;
}

// [modules.hostdata.base]: the checker's type for a registered hostdata name, which is
// what the flatten pass moves members between. hosted_unit would make a
// module that is not there, but every type asked about here registered
// through it already, so it only ever finds one.
static LhatType *hosted_type_of(LhatProgram *program, const char *module,
                                const char *name)
{
    if (program->hosted == NULL) {
        return NULL;
    }
    LhatType *table = hosted_unit(program, module, NULL);
    const LhatTypeMember *member =
        table != NULL ? hosted_member(table, name) : NULL;
    return member != NULL ? member->type : NULL;
}

static LhatHostTypeEntry *host_type_entry_of(LhatProgram *program,
                                             const LhatHostDataTag *tag)
{
    for (size_t i = 0; i < program->host_type_entry_count; i++) {
        if (program->host_type_entries[i].tag == tag) {
            return &program->host_type_entries[i];
        }
    }
    return NULL;  // registered by another program; nothing here to read
}

// [modules.hostdata.base]: gives one type what its base registered, the base having been given
// its own first -- so a chain of any depth settles whatever order the
// entries happen to be in.
//
// A name the derived type registered for itself is left alone. That is what
// overriding is: [modules.hostdata]'s "what was registered cannot be written over" is about
// L^ writing into a host type, and this is one registration standing in
// front of another.
static void flatten_host_type(LhatProgram *program, LhatHostTypeEntry *entry)
{
    if (entry == NULL || entry->flattened) {
        return;
    }
    entry->flattened = true;

    const LhatHostDataTag *base_tag = entry->tag->base;
    if (base_tag == NULL) {
        return;
    }
    LhatHostTypeEntry *base_entry = host_type_entry_of(program, base_tag);
    flatten_host_type(program, base_entry);

    LhatType *derived = hosted_type_of(program, entry->module, entry->name);
    LhatType *base = hosted_type_of(program, base_tag->module, base_tag->name);
    if (derived == NULL || base == NULL) {
        return;
    }
    // Linked, not copied. A binding declares a class per engine class and
    // each one carries its whole ancestry, so copying makes every class pay
    // for every class above it -- a walk inside a walk, and the editor
    // waits for all of it on each save ([pipeline.stages.tree]). chk_find_member follows
    // this the way the machine follows the tag chain, and a name the derived
    // type declares itself still wins because its own list is asked first.
    derived->v.table.base = base;
}

// [modules.hostdata.base]: run once, when registration has closed. The moment is the first
// check: program.h already says registering after one "is too late and
// answers false", so there is nothing left to arrive.
//
// Doing this at each lhat_register_hostdata_subtype instead would be too
// early -- program.h's own advice for types that name each other is to
// register the bare types first and give them their members after, and a
// tree registered that way has nothing to inherit at the moment the relation
// is declared.
//
// The types written into are the program's own (&program->types), so nothing
// shared is touched. The tag's release is NOT copied down: the tag belongs to
// the process (registry.h), and lhat_hostdata_release walks the chain for it.
void prg_flatten_hostdata_bases(LhatProgram *program)
{
    if (program == NULL || program->hostdata_flattened) {
        return;
    }
    program->hostdata_flattened = true;
    for (size_t i = 0; i < program->host_type_entry_count; i++) {
        flatten_host_type(program, &program->host_type_entries[i]);
    }
}

// [modules.hostdata.base]: the same declaration, under a type declared earlier. The base
// is looked up rather than made, so naming one that is not there is refused
// -- a tree written out of order would otherwise settle silently on a base
// that arrived later meaning something else.
//
// Nothing is inherited here. What the derived type carries is settled when
// registration closes (prg_flatten_hostdata_bases), so members may be registered
// on either type before or after this call.
const LhatHostDataTag *lhat_register_hostdata_subtype(LhatProgram *program,
                                                      const char *module,
                                                      const char *name,
                                                      const char *base_module,
                                                      const char *base_name)
{
    if (program == NULL || base_module == NULL || base_name == NULL) {
        return NULL;
    }
    const LhatHostDataTag *base = NULL;
    for (size_t i = 0; i < program->host_type_entry_count; i++) {
        const LhatHostTypeEntry *at = &program->host_type_entries[i];
        if (strcmp(at->module, base_module) == 0 &&
            strcmp(at->name, base_name) == 0) {
            base = at->tag;
            break;
        }
    }
    if (base == NULL) {
        return NULL;
    }

    const LhatHostDataTag *tag =
        lhat_register_hostdata_type(program, module, name);
    if (tag == NULL) {
        return NULL;
    }
    // The tag is the process's, so this is where two programs are made to
    // agree about what a name is under (registry.h).
    return lhat_registry_set_hostdata_base(tag, base) ? tag : NULL;
}

bool lhat_register_hostdata_shared(LhatProgram *program, const char *module,
                                   const char *name, LhatHostHoldFn retain,
                                   LhatHostHoldFn let_go, void *context)
{
    if (program == NULL || module == NULL || name == NULL) {
        return false;
    }
    for (size_t i = 0; i < program->host_type_entry_count; i++) {
        const LhatHostTypeEntry *at = &program->host_type_entries[i];
        if (strcmp(at->module, module) == 0 && strcmp(at->name, name) == 0) {
            // [modules.hostdata.sharing]: the tag is the process's, so the contract settles
            // there (registry.h), as the release and the base do.
            return lhat_registry_set_hold(at->tag, retain, let_go, context);
        }
    }
    return false;
}

bool lhat_register_hostdata_type_arguments(LhatProgram *program, const char *module,
                                          const char *name, LhatHostTypeArgumentFn read,
                                          void *context)
{
    if (program == NULL || module == NULL || name == NULL) return false;
    for (size_t i = 0; i < program->host_type_entry_count; i++) {
        const LhatHostTypeEntry *at = &program->host_type_entries[i];
        if (strcmp(at->module, module) == 0 && strcmp(at->name, name) == 0) {
            return lhat_registry_set_type_arguments(at->tag, read, context);
        }
    }
    return false;
}

// lhat_register_error_kind の失敗経路が繰り返し要る後始末: variant_copies
// の先頭 filled_count 個(まだ何も書かれていない calloc 直後なら 0)と、
// 配列自体2つ。variants/variant_copies のどちらかが NULL でも(要素数0の
// 登録、あるいは calloc 失敗直後)、filled_count が 0 ならループが回らず
// 安全。
static void free_variant_arrays(char **variant_copies,
                                LhatErrorKind **variants, size_t filled_count)
{
    for (size_t i = 0; i < filled_count; i++) {
        lhat_free(variant_copies[i]);
    }
    lhat_free(variant_copies);
    lhat_free(variants);
}

// [errors.grammar.types] の host 版: errordef^ を書かずに存在する誤り種別。型側は
// lhat_register_hostdata_type と同じ hosted_unit/hosted_member を通る;
// 実行時側は check.c の check_errordef / vm.c の declare_error が unit の
// ASTから作るのと同じ2つの呼び出し(lhat_type_error_set/error_kind、
// lhat_error_kind_new)を、program->host_heap に対して行う。
// [errors.kind.local]: `local` is which top, the C side of errordef^ / localerrordef^.
// The two public entries below are this with that decided.
static bool register_error_kind(LhatProgram *program, const char *module,
                                const char *name,
                                const char *const *variant_names,
                                size_t variant_count, bool local,
                                const LhatErrorKind **out_group,
                                const LhatErrorKind **out_variants)
{
    if (program == NULL || module == NULL || name == NULL) {
        return false;
    }
    LhatType *table = hosted_unit(program, module, name);
    if (table == NULL || hosted_member(table, name) != NULL) {
        return false;  // [modules.host.import]: one name, one thing
    }

    LhatType *set =
        lhat_type_error_set(&program->types, name, strlen(name), local);
    if (set == NULL ||
        lhat_type_add_member(&program->types, table, name, strlen(name),
                             set) == NULL) {
        return false;
    }
    for (size_t i = 0; i < variant_count; i++) {
        if (lhat_type_error_kind(&program->types, set, variant_names[i],
                                 strlen(variant_names[i])) == NULL) {
            return false;
        }
    }

    // [errors.kind.nominal]: a kind is the declaration it came from, and the declaration
    // is this C call -- so the kinds are the registry's (registry.h) and a
    // second program declaring the same ones comes away with the very same
    // objects. A declaration of the same name with a different list of
    // variants is refused there: two lists are two declarations.
    LhatErrorKind **variants = NULL;
    char **variant_copies = NULL;
    if (variant_count > 0) {
        variants =
            (LhatErrorKind **)lhat_calloc(variant_count, sizeof *variants);
        variant_copies =
            (char **)lhat_calloc(variant_count, sizeof *variant_copies);
        if (variants == NULL || variant_copies == NULL) {
            free_variant_arrays(variant_copies, variants, 0);
            return false;
        }
    }
    const LhatErrorKind *group = NULL;
    if (!lhat_registry_error_kind(module, name, variant_names, variant_count,
                                  local, &group,
                                  (const LhatErrorKind **)variants)) {
        free_variant_arrays(variant_copies, variants, 0);
        return false;
    }
    set->v.error.runtime_kind = group;
    LhatTypeList *checked_kind = set->v.error.kinds;
    for (size_t i = 0; i < variant_count; i++, checked_kind = checked_kind->next) {
        checked_kind->type->v.error.runtime_kind = variants[i];
        variant_copies[i] = prg_duplicate(variant_names[i]);
        if (variant_copies[i] == NULL) {
            free_variant_arrays(variant_copies, variants, i);
            return false;
        }
    }

    if (program->host_error_entry_count == program->host_error_entry_capacity) {
        size_t grown = program->host_error_entry_capacity
                           ? program->host_error_entry_capacity * 2
                           : 4;
        LhatHostErrorKind *bigger = (LhatHostErrorKind *)lhat_realloc(
            program->host_error_entries, grown * sizeof *bigger);
        if (bigger == NULL) {
            free_variant_arrays(variant_copies, variants, variant_count);
            return false;
        }
        program->host_error_entries = bigger;
        program->host_error_entry_capacity = grown;
    }

    LhatHostErrorKind *entry =
        &program->host_error_entries[program->host_error_entry_count];
    entry->module = prg_duplicate(module);
    entry->name = prg_duplicate(name);
    entry->group = group;
    entry->variant_names = (const char *const *)variant_copies;
    entry->variants = (const LhatErrorKind *const *)variants;
    entry->variant_count = variant_count;
    if (entry->module == NULL || entry->name == NULL) {
        lhat_free((void *)entry->module);
        lhat_free((void *)entry->name);
        free_variant_arrays(variant_copies, variants, variant_count);
        return false;
    }
    program->host_error_entry_count++;

    if (out_group != NULL) {
        *out_group = group;
    }
    if (out_variants != NULL) {
        for (size_t i = 0; i < variant_count; i++) {
            out_variants[i] = variants[i];
        }
    }
    return true;
}

bool lhat_register_error_kind(LhatProgram *program, const char *module,
                              const char *name,
                              const char *const *variant_names,
                              size_t variant_count,
                              const LhatErrorKind **out_group,
                              const LhatErrorKind **out_variants)
{
    return register_error_kind(program, module, name, variant_names,
                               variant_count, false, out_group, out_variants);
}

bool lhat_register_local_error_kind(LhatProgram *program, const char *module,
                                    const char *name,
                                    const char *const *variant_names,
                                    size_t variant_count,
                                    const LhatErrorKind **out_group,
                                    const LhatErrorKind **out_variants)
{
    return register_error_kind(program, module, name, variant_names,
                               variant_count, true, out_group, out_variants);
}

const LhatErrorKind *lhat_lookup_error_kind(const LhatProgram *program,
                                            const char *module,
                                            const char *name,
                                            const char *variant)
{
    if (program == NULL || module == NULL || name == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < program->host_error_entry_count; i++) {
        const LhatHostErrorKind *entry = &program->host_error_entries[i];
        if (strcmp(entry->module, module) != 0 ||
            strcmp(entry->name, name) != 0) {
            continue;
        }
        if (variant == NULL) {
            return entry->group;
        }
        for (size_t v = 0; v < entry->variant_count; v++) {
            if (strcmp(entry->variant_names[v], variant) == 0) {
                return entry->variants[v];
            }
        }
        return NULL;
    }
    return NULL;
}

void *lhat_lookup_host_context(const LhatProgram *program, const char *module,
                               const char *type, const char *name)
{
    if (program == NULL || module == NULL || name == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < program->host_entry_count; i++) {
        const LhatHostEntry *entry = &program->host_entries[i];
        if (strcmp(entry->module, module) != 0 ||
            strcmp(entry->name, name) != 0) {
            continue;
        }
        if ((type == NULL) != (entry->type == NULL)) {
            continue;
        }
        if (type != NULL && strcmp(entry->type, type) != 0) {
            continue;
        }
        return entry->context;
    }
    return NULL;
}

// [syntax.obj.override]: whether two registrations of one name could take the same
// call. Arms may stand together only where no call fits both -- which is what
// makes the search that resolves one able to stop at the first that fits.
//
// Told apart by anything that decides a call: how many parameters, whether a
// tail collects more, which operand the receiver is ([syntax.op.right]), and failing all
// of those, a parameter position where neither type admits the other's values.
#if LHAT_WITH_FRONTEND
static bool host_arms_overlap(const LhatType *a, const LhatType *b)
{
    if (a == NULL || b == NULL || a->kind != LHAT_TYPE_FUNC ||
        b->kind != LHAT_TYPE_FUNC) {
        return true;  // nothing to tell them apart with
    }
    if (a->v.func.takes_self != b->v.func.takes_self ||
        a->v.func.self_last != b->v.func.self_last) {
        return false;
    }
    // Two arms are told apart exactly where the machine's search (vm.c's
    // fits_call) can tell them apart: by a written parameter position the
    // two disagree on, or by one arm having no place for an argument the
    // other requires. [syntax.type.variadic]: a variadic tail makes the count a floor and is
    // not compared by type at the call, so past its written parameters an
    // arm takes anything -- 'f(string^)' and 'f(number^, ...)' part at the
    // first position, 'f(number^)' and 'f(number^, number^, ...)' at the
    // second, and 'f(string^, ...)' against 'f(string^, font, ...)' is one
    // call fitting both, so it is refused.
    const LhatTypeList *pa = a->v.func.params;
    const LhatTypeList *pb = b->v.func.params;
    for (;; pa = pa != NULL ? pa->next : NULL, pb = pb != NULL ? pb->next : NULL) {
        if (pa == NULL && pb == NULL) {
            return true;  // a call of exactly this count fits both
        }
        bool a_open = pa != NULL || a->v.func.variadic != NULL;
        bool b_open = pb != NULL || b->v.func.variadic != NULL;
        if (!a_open || !b_open) {
            return false;  // one takes no argument here; the other needs one
        }
        if (pa != NULL && pb != NULL &&
            lhat_type_disjoint(pa->type, pb->type)) {
            return false;
        }
    }
}
#endif  // LHAT_WITH_FRONTEND

#if LHAT_WITH_FRONTEND
static bool register_into(LhatProgram *program, LhatType *owner,
                          const char *module, const char *type,
                          const char *name, const char *signature,
                          LhatHostFn call, void *context)
{
    if (owner == NULL || call == NULL) {
        return false;
    }
    // [modules.host.import]: a signature may name the builtins and whatever was registered
    // before it. Nothing a require^ brings in -- that would put the answer
    // back at the mercy of the order units are checked in. A member's may
    // write the type it is registered on as [syntax.type.self]'s Self^.
    LhatType *written = lhat_type_of_text(signature, strlen(signature),
                                          &program->types, program->hosted,
                                          type != NULL ? owner : NULL);
    if (written == NULL) {
        return false;
    }
    // [modules.hostdata.index]: [] is a hostdata-only, non-yielding getter.
    if (strcmp(name, "[]") == 0 &&
        (owner->kind != LHAT_TYPE_TABLE || owner->v.table.hostdata_tag == NULL ||
         written->kind != LHAT_TYPE_FUNC || !written->v.func.is_function ||
         !written->v.func.takes_self || written->v.func.self_last ||
         written->v.func.mutable_self ||
         written->v.func.yields || written->v.func.variadic != NULL ||
         written->v.func.params == NULL || written->v.func.params->next != NULL ||
         written->v.func.result == NULL ||
         lhat_type_tuple_arm_width(written->v.func.result) > 0)) {
        return false;
    }
    // [syntax.type.tuple]: the room a boundary answers into is LHAT_MAX_TUPLE
    // wide, and the reason it is that wide is that the machine's own is.
    // A signature promising more than the machine can carry is refused
    // HERE, where it is declared -- not found out later, when a host has
    // already written past the room it was handed. What the compiler
    // refuses at a written call site (compile.c), this refuses at a
    // registered one.
    if (written->kind == LHAT_TYPE_FUNC &&
        lhat_type_tuple_arm_width(written->v.func.result) >
            LHAT_MAX_TUPLE) {
        return false;
    }

    // [syntax.obj.override]: a name already registered gains an arm rather than being
    // refused. The type becomes the intersection of what was there and what
    // is written now -- which is exactly what an overload^ makes of a member
    // written twice, so a call site reads the two the same way.
    LhatTypeMember *existing = (LhatTypeMember *)hosted_member(owner, name);
    if (existing != NULL) {
        if (existing->type == NULL) {
            return false;
        }
        const LhatType *arms = existing->type;
        if (arms->kind == LHAT_TYPE_INTERSECT) {
            for (const LhatTypeList *arm = arms->v.composite.arms; arm != NULL;
                 arm = arm->next) {
                if (host_arms_overlap(arm->type, written)) {
                    return false;
                }
            }
        } else if (host_arms_overlap(arms, written)) {
            return false;
        }
        LhatType *joined =
            lhat_type_intersect(&program->types, existing->type, written);
        if (joined == NULL) {
            return false;
        }
        existing->type = joined;
        return keep_entry(program, module, type, name, call, context, written,
                          signature);
    }

    return lhat_type_add_member(&program->types, owner, name, strlen(name),
                                written) != NULL &&
           keep_entry(program, module, type, name, call, context, written,
                      signature);
}
#else
// [modules.binary.signatures]: what keep_entry takes from a parsed signature, taken from the
// descriptor the table holds instead -- the parts are the parameters, the
// marks are the receiver and the variadic tail.
static bool keep_entry_rt(LhatProgram *program, const char *module,
                          const char *type, const char *name, LhatHostFn call,
                          void *context, const LhatRuntimeType *rt,
                          const char *signature_text)
{
    LHAT_GROW(program->host_entries, program->host_entry_count,
              program->host_entry_capacity, 8, return false);
    LhatHostEntry *entry = &program->host_entries[program->host_entry_count];
    memset(entry, 0, sizeof *entry);
    entry->module = prg_duplicate(module);
    entry->name = prg_duplicate(name);
    entry->type = type != NULL ? prg_duplicate(type) : NULL;
    entry->signature_text = prg_duplicate(signature_text);
    entry->call = call;
    entry->context = context;
    if (rt->part_count > 0xFF) {
        return false;
    }
    entry->parameters = (uint8_t)rt->part_count;
    entry->has_variadic = rt->variadic != NULL;
    entry->takes_self = rt->takes_self;
    entry->self_last = rt->self_last;
    entry->runtime_signature = rt;
    if (entry->parameters > 0) {
        entry->parameter_types = borrowed_params(
            (const LhatRuntimeType *const *)rt->parts, entry->parameters);
    }
    if (entry->module == NULL || entry->name == NULL ||
        (type != NULL && entry->type == NULL) ||
        entry->signature_text == NULL ||
        (entry->parameters > 0 && entry->parameter_types == NULL)) {
        return false;
    }
    program->host_entry_count++;
    return true;
}

// [modules.binary.vm-only]: the signature table stands in for the text. The checker's side
// (hosted) is not built -- nothing here reads it.
static bool register_into(LhatProgram *program, LhatType *owner,
                          const char *module, const char *type,
                          const char *name, const char *signature,
                          LhatHostFn call, void *context)
{
    if (owner == NULL || call == NULL || signature == NULL) {
        return false;
    }
    if (prg_find_signature(program, signature) == NULL) {
        lhat_program_report(program, LHAT_PROGRAM_ERR_NO_SIGNATURE, signature);
        return false;
    }
    const LhatRuntimeType *rt = lhat_program_signature_type(program, signature);
    if (strcmp(name, "[]") == 0 &&
        (owner->kind != LHAT_TYPE_TABLE || owner->v.table.hostdata_tag == NULL ||
         rt == NULL || rt->kind != LHAT_TYPE_RT_SUBROUTINE || !rt->is_function ||
         !rt->takes_self || rt->self_last || rt->mutable_self || rt->variadic != NULL ||
         rt->part_count != 1 || rt->result == NULL ||
         rt->result->kind == LHAT_TYPE_RT_TUPLE)) {
        return false;
    }
    return rt != NULL && keep_entry_rt(program, module, type, name, call,
                                       context, rt, signature);
}
#endif  // LHAT_WITH_FRONTEND

bool lhat_register_member(LhatProgram *program, const char *module,
                          const char *type, const char *name,
                          const char *signature, LhatHostFn call,
                          void *context)
{
    LhatType *table = hosted_unit(program, module, NULL);
    const LhatTypeMember *found = hosted_member(table, type);
    if (found == NULL || found->type->kind != LHAT_TYPE_TABLE) {
        return false;  // no such type registered under that module
    }
    if (!register_into(program, found->type, module, type, name, signature,
                       call, context)) {
        return false;
    }

    // [modules.hostdata]: [syntax.with.checked] reads a lifetime off whether the type carries a
    // dispose^, so registering one is what makes the value the host's to
    // take back. The tag carries it, since that is what a value still has
    // when a collection reaches it.
    //
    // The tag is the process's (registry.h), so a second program declaring
    // the same dispose^ is that one declaration made twice and settles on
    // the same answer. A different one would be two ways of handing back
    // one type, which is refused.
    if (strcmp(name, "dispose") == 0) {
        for (size_t i = 0; i < program->host_entry_count; i++) {
            LhatHostEntry *entry = &program->host_entries[i];
            if (entry->tag != NULL && entry->type == NULL &&
                strcmp(entry->module, module) == 0 &&
                strcmp(entry->name, type) == 0) {
                return lhat_registry_set_release(entry->tag, call, context);
            }
        }
    }
    return true;
}

// [syntax.annot.host]: an annotation is checked and never installed, so this records
// only what the checker asks -- where it may be written, and what shape its
// arguments have. No LhatHostEntry: there is no value to build for a machine.
bool lhat_register_annotation(LhatProgram *program, const char *module,
                              const char *name, uint32_t targets)
{
    if (program == NULL || module == NULL || name == NULL || targets == 0) {
        return false;
    }
    // [modules.host.import]: one name, one thing. Unlike a member, a second registration is not
    // another arm -- [syntax.annot.spelling] keeps the namespace flat, and two hosts wanting the
    // same spelling is a collision to report rather than to merge.
    for (size_t i = 0; i < program->annotation_count; i++) {
        if (strcmp(program->annotations[i].name, name) == 0) {
            return false;
        }
    }

    char *kept_module = prg_duplicate(module);
    char *kept_name = prg_duplicate(name);
    if (kept_module == NULL || kept_name == NULL) {
        lhat_free(kept_module);
        lhat_free(kept_name);
        return false;
    }

    // The text grows alongside the declarations, so a refusal leaves the two
    // the same length.
    size_t at = program->annotation_count;
    if (at == program->annotation_capacity) {
        size_t grown = program->annotation_capacity
                           ? program->annotation_capacity * 2 : 4;
        LhatAnnotationDecl *decls = (LhatAnnotationDecl *)lhat_realloc(
            program->annotations, grown * sizeof *decls);
        char **texts = (char **)lhat_realloc(program->annotation_signatures,
                                             grown * sizeof *texts);
        if (decls != NULL) {
            program->annotations = decls;
        }
        if (texts != NULL) {
            program->annotation_signatures = texts;
        }
        if (decls == NULL || texts == NULL) {
            lhat_free(kept_module);
            lhat_free(kept_name);
            return false;
        }
        program->annotation_capacity = grown;
    }

    memset(&program->annotations[at], 0, sizeof program->annotations[at]);
    program->annotations[at].module = kept_module;
    program->annotations[at].name = kept_name;
    program->annotations[at].targets = targets;
    program->annotation_signatures[at] = NULL;
    program->annotation_count++;
    return true;
}

// The registration of `name`, or NULL. [syntax.annot.spelling]'s flat namespace is what lets the
// name alone find it.
static LhatAnnotationDecl *annotation_named(LhatProgram *program,
                                            const char *name, size_t *at)
{
    if (program == NULL || name == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < program->annotation_count; i++) {
        if (strcmp(program->annotations[i].name, name) == 0) {
            if (at != NULL) {
                *at = i;
            }
            return &program->annotations[i];
        }
    }
    return NULL;
}

#if LHAT_WITH_FRONTEND
bool lhat_register_annotation_signature(LhatProgram *program,
                                        const char *name,
                                        const char *signature)
{
    size_t at = 0;
    LhatAnnotationDecl *decl = annotation_named(program, name, &at);
    if (decl == NULL || signature == NULL || decl->signature != NULL) {
        return false;  // no such name, nothing said, or said once already
    }

    const LhatType *written = lhat_type_of_text(signature, strlen(signature),
                                                &program->types,
                                                program->hosted, NULL);
    char *kept = written != NULL ? prg_duplicate(signature) : NULL;
    if (written == NULL || kept == NULL) {
        lhat_free(kept);
        return false;
    }
    decl->signature = written;
    program->annotation_signatures[at] = kept;
    return true;
}
#else
bool lhat_register_annotation_signature(LhatProgram *program,
                                        const char *name,
                                        const char *signature)
{
    // [modules.binary.vm-only]: an annotation is the checker's to read, and there is no
    // checker; the text is kept for a dump, the type is not made.
    size_t at = 0;
    LhatAnnotationDecl *decl = annotation_named(program, name, &at);
    if (decl == NULL || signature == NULL ||
        program->annotation_signatures[at] != NULL) {
        return false;
    }
    program->annotation_signatures[at] = prg_duplicate(signature);
    return program->annotation_signatures[at] != NULL;
}
#endif  // LHAT_WITH_FRONTEND

bool lhat_register_annotation_exclusive(LhatProgram *program,
                                        const char *name, const char *other)
{
    LhatAnnotationDecl *decl = annotation_named(program, name, NULL);
    if (decl == NULL || other == NULL || strcmp(name, other) == 0) {
        return false;  // nothing excludes itself
    }
    for (size_t i = 0; i < decl->exclusive_count; i++) {
        if (strcmp(decl->exclusives[i], other) == 0) {
            return true;  // said twice is said once
        }
    }

    char *kept = prg_duplicate(other);
    const char **grown = (const char **)lhat_realloc(
        (void *)decl->exclusives,
        (decl->exclusive_count + 1) * sizeof *decl->exclusives);
    if (kept == NULL || grown == NULL) {
        lhat_free(kept);
        if (grown != NULL) {
            decl->exclusives = grown;
        }
        return false;
    }
    decl->exclusives = grown;
    decl->exclusives[decl->exclusive_count++] = kept;
    return true;
}

bool lhat_register_annotation_requisite(LhatProgram *program,
                                        const char *name, const char *other)
{
    LhatAnnotationDecl *decl = annotation_named(program, name, NULL);
    if (decl == NULL || other == NULL || strcmp(name, other) == 0) {
        return false;  // nothing is its own other half
    }
    for (size_t i = 0; i < decl->requisite_count; i++) {
        if (strcmp(decl->requisites[i], other) == 0) {
            return true;  // said twice is said once
        }
    }

    char *kept = prg_duplicate(other);
    const char **grown = (const char **)lhat_realloc(
        (void *)decl->requisites,
        (decl->requisite_count + 1) * sizeof *decl->requisites);
    if (kept == NULL || grown == NULL) {
        lhat_free(kept);
        if (grown != NULL) {
            decl->requisites = grown;
        }
        return false;
    }
    decl->requisites = grown;
    decl->requisites[decl->requisite_count++] = kept;
    return true;
}

bool lhat_register_func(LhatProgram *program, const char *module,
                        const char *name, const char *signature,
                        LhatHostFn call, void *context)
{
    return register_into(program, hosted_unit(program, module, name),
                         module, NULL, name, signature, call, context);
}

bool lhat_register_instantiation_check_handler(
    LhatProgram *program, const char *module, const char *type, const char *name,
    size_t arm, LhatInstantiationCheckHandler handler, void *context)
{
    if (program == NULL || module == NULL || name == NULL || handler == NULL) return false;
#if LHAT_WITH_FRONTEND
    LhatType *owner = hosted_unit(program, module, NULL);
    if (type != NULL) {
        const LhatTypeMember *member = hosted_member(owner, type);
        owner = member != NULL ? member->type : NULL;
    }
    if (owner == NULL) return false;
    const LhatTypeMember *member = hosted_member(owner, name);
    LhatType *signature = member != NULL ? member->type : NULL;
    if (signature != NULL && signature->kind == LHAT_TYPE_INTERSECT) {
        const LhatTypeList *at = signature->v.composite.arms;
        while (at != NULL && arm-- > 0) at = at->next;
        signature = at != NULL ? at->type : NULL;
    } else if (arm != 0) {
        return false;
    }
    if (signature == NULL || signature->kind != LHAT_TYPE_FUNC ||
        signature->v.func.instantiation_handler != NULL) return false;
    signature->v.func.instantiation_handler = handler;
    signature->v.func.instantiation_context = context;
#else
    (void)type; (void)arm; (void)context;
#endif
    return true;
}

// [modules.host.nested-fault]: the shared road of the four constant registrations. No
// signature text and no lhat_type_of_text: an enum dump registers
// thousands of these, and lhat_type_simple answers the one shared node
// per kind. A constant takes its name whole -- an existing member,
// constant or subroutine, refuses it, since a value is no overload arm.
static bool register_const(LhatProgram *program, const char *module,
                           const char *type, const char *name,
                           LhatHostConstKind kind, LhatInteger whole,
                           LhatReal real, bool truth, const char *text)
{
    if (program == NULL || module == NULL || name == NULL ||
        (kind == LHAT_HOST_CONST_STRING && text == NULL)) {
        return false;
    }
    LhatType *owner = hosted_unit(program, module, type == NULL ? name : NULL);
    if (type != NULL) {
        const LhatTypeMember *found = hosted_member(owner, type);
        // [modules.hostdata]'s nominal table or [syntax.stmt.let]'s host value type -- both carry the
        // member list a static lands in.
        if (found == NULL || (found->type->kind != LHAT_TYPE_TABLE &&
                              found->type->kind != LHAT_TYPE_HOSTVALUE)) {
            return false;
        }
        owner = found->type;
    }
    if (owner == NULL || hosted_member(owner, name) != NULL) {
        return false;
    }
    LhatType *written = lhat_type_simple(
        &program->types, kind == LHAT_HOST_CONST_STRING ? LHAT_TYPE_STRING
                         : kind == LHAT_HOST_CONST_BOOL ? LHAT_TYPE_BOOL
                                                        : LHAT_TYPE_NUMBER);
    if (written == NULL ||
        lhat_type_add_member(&program->types, owner, name, strlen(name),
                             written) == NULL) {
        return false;
    }
    if (!keep_entry(program, module, type, name, NULL, NULL, NULL, NULL)) {
        return false;
    }
    LhatHostEntry *entry =
        &program->host_entries[program->host_entry_count - 1];
    entry->const_kind = kind;
    entry->const_integer = whole;
    entry->const_real = real;
    entry->const_bool = truth;
    if (text != NULL) {
        entry->const_text = prg_duplicate(text);
        if (entry->const_text == NULL) {
            return false;
        }
    }
    return true;
}

bool lhat_register_enum_valued(LhatProgram *program, const char *module,
                               const char *type, const char *name,
                               const char *const *members,
                               const LhatInteger *values, size_t count)
{
    if (program == NULL || module == NULL || name == NULL ||
        members == NULL || count == 0) {
        return false;
    }
    LhatType *owner = hosted_unit(program, module, type == NULL ? name : NULL);
    if (type != NULL) {
        const LhatTypeMember *found = hosted_member(owner, type);
        // [modules.hostdata]'s nominal table or [syntax.stmt.let]'s host value type -- the same member
        // list a static constant lands in.
        if (found == NULL || (found->type->kind != LHAT_TYPE_TABLE &&
                              found->type->kind != LHAT_TYPE_HOSTVALUE)) {
            return false;
        }
        owner = found->type;
    }
    if (owner == NULL || hosted_member(owner, name) != NULL) {
        return false;
    }
    LHAT_GROW(program->host_enums, program->host_enum_count,
              program->host_enum_capacity, 4, return false);
    LhatProgramEnum *e = &program->host_enums[program->host_enum_count];
    memset(e, 0, sizeof *e);
    e->module = prg_duplicate(module);
    e->type = type != NULL ? prg_duplicate(type) : NULL;
    e->name = prg_duplicate(name);
    e->members = (char **)lhat_calloc(count, sizeof *e->members);
    e->values = (LhatInteger *)lhat_alloc(count * sizeof *e->values);
    if (e->module == NULL || (type != NULL && e->type == NULL) ||
        e->name == NULL || e->members == NULL || e->values == NULL) {
        return false;
    }
    e->count = count;
    for (size_t i = 0; i < count; i++) {
        if (members[i] == NULL) {
            return false;
        }
        e->members[i] = prg_duplicate(members[i]);
        if (e->members[i] == NULL) {
            return false;
        }
        e->values[i] = values != NULL ? values[i] : (LhatInteger)i + 1;
    }

    // The checker's two tiers, [syntax.enum]'s -- a unit's when^ over the
    // members proves exhaustive the way a declared enum's does.
    LhatType *decl =
        lhat_type_enum_decl(&program->types, e->name, strlen(e->name));
    LhatType *number = lhat_type_simple(&program->types, LHAT_TYPE_NUMBER);
    if (decl == NULL || number == NULL) {
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        if (lhat_type_enum_member(&program->types, decl, e->members[i],
                                  strlen(e->members[i]), number) == NULL) {
            return false;
        }
    }
    if (lhat_type_add_member(&program->types, owner, e->name,
                             strlen(e->name), decl) == NULL) {
        return false;
    }

    // The identity fits^ compares, shared by every machine of this program.
    e->decl_rt = lhat_type_rt_new(&program->host_heap, LHAT_TYPE_RT_ENUM);
    if (e->decl_rt == NULL) {
        return false;
    }
    e->decl_rt->enum_decl = decl;
    e->decl_rt->enum_name =
        lhat_string_new(&program->host_heap, e->name, strlen(e->name));
    e->decl_rt->enum_module = lhat_string_new(&program->host_heap, module, strlen(module));
    if (type != NULL)
        e->decl_rt->enum_qualifier = lhat_string_new(&program->host_heap, type, strlen(type));
    if (e->decl_rt->enum_name == NULL || e->decl_rt->enum_module == NULL ||
        (type != NULL && e->decl_rt->enum_qualifier == NULL)) return false;
    program->host_enum_count++;
    return true;
}

bool lhat_register_enum(LhatProgram *program, const char *module,
                        const char *type, const char *name,
                        const char *const *members, size_t count)
{
    return lhat_register_enum_valued(program, module, type, name, members,
                                     NULL, count);
}

bool lhat_register_const_integer(LhatProgram *program, const char *module,
                                 const char *type, const char *name,
                                 LhatInteger value)
{
    return register_const(program, module, type, name,
                          LHAT_HOST_CONST_INTEGER, value, 0.0, false, NULL);
}

bool lhat_register_const_real(LhatProgram *program, const char *module,
                              const char *type, const char *name,
                              LhatReal value)
{
    return register_const(program, module, type, name, LHAT_HOST_CONST_REAL,
                          0, value, false, NULL);
}

bool lhat_register_const_bool(LhatProgram *program, const char *module,
                              const char *type, const char *name, bool value)
{
    return register_const(program, module, type, name, LHAT_HOST_CONST_BOOL,
                          0, 0.0, value, NULL);
}

bool lhat_register_const_string(LhatProgram *program, const char *module,
                                const char *type, const char *name,
                                const char *text)
{
    return register_const(program, module, type, name,
                          LHAT_HOST_CONST_STRING, 0, 0.0, false, text);
}

// [modules.hostvalue]: how many payload bytes one field kind occupies, which is what
// registration checks offsets against.
static size_t hostvalue_field_bytes(LhatHostValueFieldKind kind)
{
    switch (kind) {
        case LHAT_HVFIELD_F32: return 4;
        case LHAT_HVFIELD_F64: return 8;
        case LHAT_HVFIELD_I8:  return 1;
        case LHAT_HVFIELD_I16: return 2;
        case LHAT_HVFIELD_I32: return 4;
        case LHAT_HVFIELD_I64: return 8;
        case LHAT_HVFIELD_U8:  return 1;
        case LHAT_HVFIELD_U16: return 2;
        case LHAT_HVFIELD_U32: return 4;
    }
    return 0;
}

const LhatHostValueTag *lhat_register_hostvalue_type(LhatProgram *program,
                                                     const char *module,
                                                     const char *name,
                                                     size_t size)
{
    if (program == NULL || module == NULL || name == NULL || size == 0 ||
        size > LHAT_HOSTVALUE_MAX_BYTES) {
        return NULL;
    }
    LhatType *table = hosted_unit(program, module, name);
    if (table == NULL || hosted_member(table, name) != NULL) {
        return NULL;  // [modules.host.import]: one name, one thing
    }

    // Grown before anything below can half-succeed, so a refusal here leaves
    // the program exactly as it was.
    if (program->hostvalue_type_entry_count ==
        program->hostvalue_type_entry_capacity) {
        size_t grown_count = program->hostvalue_type_entry_capacity
                                 ? program->hostvalue_type_entry_capacity * 2
                                 : 4;
        LhatHostValueTypeEntry *bigger = (LhatHostValueTypeEntry *)lhat_realloc(
            program->hostvalue_type_entries, grown_count * sizeof *bigger);
        if (bigger == NULL) {
            return NULL;
        }
        program->hostvalue_type_entries = bigger;
        program->hostvalue_type_entry_capacity = grown_count;
    }

    // [syntax.stmt.let]: the tag is identity, and there is one per DECLARATION rather than
    // one per program -- the registry (registry.h) keeps it, and a second
    // program declaring the same type comes away with the same one. A
    // declaration of the same name at a different size is refused there:
    // the width is what every frame holding one was laid out against.
    LhatHostValueTag *tag =
        (LhatHostValueTag *)lhat_registry_hostvalue(module, name, size);
    if (tag == NULL) {
        return NULL;
    }

    LhatType *made = lhat_type_hostvalue(&program->types, tag);
    if (made == NULL ||
        lhat_type_add_member(&program->types, table, name, strlen(name),
                             made) == NULL ||
        !keep_entry(program, module, NULL, name, NULL, NULL, NULL, NULL)) {
        return NULL;
    }
    // keep_entry has install make an empty table under the type's name in
    // L^.modules, exactly as for a hostdata type -- that table is where the
    // registered members land, and what the machine reads back as the
    // type's members table.
    LhatHostEntry *entry = &program->host_entries[program->host_entry_count - 1];

    LhatHostValueTypeEntry *type_entry =
        &program->hostvalue_type_entries[program->hostvalue_type_entry_count++];
    type_entry->module = entry->module;
    type_entry->name = entry->name;
    type_entry->tag = tag;

    return tag;
}

bool lhat_register_hostvalue_member(LhatProgram *program, const char *module,
                                    const char *type, const char *name,
                                    const char *signature, LhatHostFn call,
                                    void *context)
{
    LhatType *table = hosted_unit(program, module, NULL);
    const LhatTypeMember *found = hosted_member(table, type);
    if (found == NULL || found->type->kind != LHAT_TYPE_HOSTVALUE) {
        return false;  // no such value type registered under that module
    }
    // No dispose special case here: a host value has no lifetime to hand
    // over, which is half of what makes it one.
    return register_into(program, found->type, module, type, name, signature,
                         call, context);
}

bool lhat_register_hostvalue_field(LhatProgram *program, const char *module,
                                   const char *type, const char *field,
                                   size_t offset, LhatHostValueFieldKind kind)
{
    if (program == NULL || module == NULL || type == NULL || field == NULL) {
        return false;
    }
    // The tag comes from the registry rather than through the checker type,
    // so the const on the public pointer stays honest -- the program owns
    // what it hands out and may still write to it here.
    LhatHostValueTag *tag = NULL;
    for (size_t i = 0; i < program->hostvalue_type_entry_count; i++) {
        const LhatHostValueTypeEntry *entry =
            &program->hostvalue_type_entries[i];
        if (strcmp(entry->module, module) == 0 &&
            strcmp(entry->name, type) == 0) {
            tag = (LhatHostValueTag *)entry->tag;
            break;
        }
    }
    size_t bytes = hostvalue_field_bytes(kind);
    if (tag == NULL || bytes == 0 || offset > tag->size ||
        tag->size - offset < bytes) {
        return false;
    }

    // The checker side has to exist and not already answer the name --
    // neither as a field nor as a registered member.
    LhatType *table = hosted_unit(program, module, NULL);
    const LhatTypeMember *found = hosted_member(table, type);
    if (found == NULL || found->type->kind != LHAT_TYPE_HOSTVALUE ||
        hosted_member(found->type, field) != NULL) {
        return false;
    }

    // The field belongs to the tag, and the tag to the process (registry.h)
    // -- so a second program declaring the same field settles on the one
    // that is there, and one that declares it at a different offset or of a
    // different kind is refused.
    if (!lhat_registry_hostvalue_field(tag, field, offset, kind)) {
        return false;
    }
    // The checker's side is this program's, and is added whether the field
    // was already declared or not.
    LhatType *number = lhat_type_simple(&program->types, LHAT_TYPE_NUMBER);
    if (number == NULL ||
        lhat_type_add_member(&program->types, found->type, field,
                             strlen(field), number) == NULL) {
        return false;
    }
    return true;
}

// [modules.host.machine]: L^ itself. Kept apart from the entries above because those land
// under L^.modules and this one does not -- and because the checker reads it
// from a different place, its own L^ rather than the import^ registry.
bool lhat_register_global(LhatProgram *program, const char *name,
                          const char *signature, LhatHostFn call,
                          void *context)
{
    if (program == NULL || name == NULL || call == NULL) {
        return false;
    }
    if (program->globals == NULL) {
        program->globals = lhat_type_table(&program->types);
        if (program->globals == NULL) {
            return false;
        }
    }
    if (hosted_member(program->globals, name) != NULL) {
        return false;  // [modules.host.import]: one name, one thing
    }
#if LHAT_WITH_FRONTEND
    LhatType *written = lhat_type_of_text(signature, strlen(signature),
                                          &program->types, program->hosted,
                                          NULL);
    if (written == NULL ||
        lhat_type_add_member(&program->types, program->globals, name,
                             strlen(name), written) == NULL) {
        return false;
    }
#else
    // [modules.binary.vm-only]: the table stands in for the text, as in register_into.
    if (signature == NULL || prg_find_signature(program, signature) == NULL) {
        lhat_program_report(program, LHAT_PROGRAM_ERR_NO_SIGNATURE,
               signature != NULL ? signature : name);
        return false;
    }
    const LhatRuntimeType *held = lhat_program_signature_type(program, signature);
    if (held == NULL || held->part_count > 0xFF) {
        return false;
    }
#endif

    LHAT_GROW(program->global_entries, program->global_count,
              program->global_capacity, 4, return false);
    LhatGlobalEntry *entry = &program->global_entries[program->global_count];
    memset(entry, 0, sizeof *entry);
    entry->name = prg_duplicate(name);
    entry->signature_text = prg_duplicate(signature);
    entry->call = call;
    entry->context = context;
#if LHAT_WITH_FRONTEND
    if (written->kind == LHAT_TYPE_FUNC) {
        size_t count = 0;
        for (const LhatTypeList *p = written->v.func.params; p != NULL;
             p = p->next) {
            count++;
        }
        entry->parameters = (uint8_t)count;
        entry->has_variadic = written->v.func.variadic != NULL;
        entry->takes_self = written->v.func.takes_self;
        entry->self_last = written->v.func.self_last;
        entry->runtime_signature = lhat_rt_from_checked(&program->host_heap, written, NULL);
        if (entry->runtime_signature == NULL) return false;
        entry->parameter_types =
            lower_host_params(program, written, entry->parameters);
    }
#else
    entry->parameters = (uint8_t)held->part_count;
    entry->has_variadic = held->variadic != NULL;
    entry->takes_self = held->takes_self;
    entry->self_last = held->self_last;
    entry->runtime_signature = held;
    if (entry->parameters > 0) {
        entry->parameter_types = borrowed_params(
            (const LhatRuntimeType *const *)held->parts, entry->parameters);
    }
#endif
    if (entry->name == NULL || entry->signature_text == NULL ||
        (entry->parameters > 0 && entry->parameter_types == NULL)) {
        return false;
    }
    program->global_count++;
    return true;
}

// [modules.host.initial]. `member` is written "L^.<name>", which is the only form for now:
// [modules.host.machine]'s table is where a host puts what it wants seen without a require^.
bool lhat_bind_initial(LhatProgram *program, const char *name,
                       const char *member)
{
    static const char prefix[] = "L^.";
    if (program == NULL || name == NULL || member == NULL ||
        strncmp(member, prefix, sizeof prefix - 1) != 0) {
        return false;
    }
    const char *reached = member + sizeof prefix - 1;
    if (*reached == '\0') {
        return false;
    }

    if (program->initial_count == program->initial_capacity) {
        size_t grown =
            program->initial_capacity ? program->initial_capacity * 2 : 4;
        char **names = (char **)lhat_realloc(program->initial_names,
                                             grown * sizeof *names);
        if (names == NULL) {
            return false;
        }
        program->initial_names = names;
        char **members = (char **)lhat_realloc(program->initial_members,
                                               grown * sizeof *members);
        if (members == NULL) {
            return false;
        }
        program->initial_members = members;
        program->initial_capacity = grown;
    }
    program->initial_names[program->initial_count] = prg_duplicate(name);
    program->initial_members[program->initial_count] = prg_duplicate(reached);
    if (program->initial_names[program->initial_count] == NULL ||
        program->initial_members[program->initial_count] == NULL) {
        return false;
    }
    program->initial_count++;
    return true;
}

// ---------------------------------------------------------------------------
// [modules.host.shared-registry]: the registrations, built once on the program's own heap
// ---------------------------------------------------------------------------
//
// install used to build every registration again for every machine: a value
// object and a key string apiece, and a table per path segment, all on that
// machine's heap. Nothing about any of it is the machine's -- a LhatHost
// carries the host's own C pointers, a key string is bytes, a type's members
// table holds only more of the same. So it is built here instead, on
// host_heap, where things are born black (LHAT_GC_BLACK, set below) and
// therefore belong to no machine's collection: lhat_gc_reach returns without
// writing when what it is handed is not white, which is the whole of what
// makes a shared object safe to walk from several threads at once.
//
// The condition that comes with it is IMMUTABILITY, not blackness (gc.h).
// A write into a black table would trip lhat_gc_barrier_back -- whose guard
// is "is the parent black", which these meet for ever -- and thread the
// table onto one machine's gray list. So everything built here is sealed and
// shared (share_tree), which refuses the host's writes as well as a
// program's, and what a machine still writes into (a unit's own module^, a
// late registration) lands on the machine's side of the join.

// The heap-only twin of vm.c's reach_table. No machine, so no barrier and
// no reserved-seat dance: nothing here is written twice.
static LhatTable *shared_reach(LhatHeap *heap, LhatTable *owner,
                               const char *path)
{
    for (const char *segment = path;;) {
        size_t length = strcspn(segment, ".");
        LhatString *key = lhat_string_new(heap, segment, length);
        if (key == NULL) {
            return NULL;
        }
        LhatValue held = lhat_table_get(owner, lhat_object((LhatObject *)key));
        LhatTable *next =
            lhat_is_object_kind(held, LHAT_OBJECT_TABLE)
                ? (LhatTable *)lhat_as_object(held)
                : NULL;
        if (next == NULL) {
            if (!lhat_is_nil(held)) {
                return NULL;  // something that is not a table is there
            }
            next = lhat_table_new(heap);
            bool refused = false;
            if (next == NULL ||
                !lhat_table_set(owner, lhat_object((LhatObject *)key),
                                lhat_object((LhatObject *)next), &refused) ||
                refused) {
                return NULL;
            }
        }
        owner = next;
        if (segment[length] == '\0') {
            return owner;
        }
        segment += length + 1;
    }
}

// The heap-only twin of lhat_machine_register, overloads included:
// [syntax.obj.override] makes a second registration of one name another arm, and that is a
// fact about the registrations rather than about any machine.
static bool shared_register(LhatHeap *heap, LhatTable *root,
                            const char *module, const char *type,
                            const char *name, LhatValue value)
{
    LhatTable *owner = shared_reach(heap, root, module);
    if (owner != NULL && type != NULL) {
        owner = shared_reach(heap, owner, type);
    }
    if (owner == NULL) {
        return false;
    }
    LhatString *key = lhat_string_new(heap, name, strlen(name));
    if (key == NULL) {
        return false;
    }
    LhatValue held = lhat_table_get(owner, lhat_object((LhatObject *)key));
    if (!lhat_is_nil(held)) {
        LhatOverload *group =
            lhat_is_object_kind(held, LHAT_OBJECT_OVERLOAD)
                ? (LhatOverload *)lhat_as_object(held)
                : lhat_overload_new(heap);
        if (group == NULL) {
            return false;
        }
        if (!lhat_is_object_kind(held, LHAT_OBJECT_OVERLOAD) &&
            !lhat_overload_add(group, held)) {
            return false;
        }
        if (!lhat_overload_add(group, value)) {
            return false;
        }
        value = lhat_object((LhatObject *)group);
    }
    bool refused = false;
    return lhat_table_set(owner, lhat_object((LhatObject *)key), value,
                          &refused) &&
           !refused;
}

// One registration made into a value on the program's heap and put where it
// belongs: a constant, a string, a type's (empty) members table, or a host.
static bool shared_entry(LhatProgram *program, LhatTable *root,
                         const LhatHostEntry *e)
{
    LhatHeap *heap = &program->host_heap;
    LhatValue value = lhat_nil();
    if (e->const_kind == LHAT_HOST_CONST_INTEGER) {
        value = lhat_integer(e->const_integer);
    } else if (e->const_kind == LHAT_HOST_CONST_REAL) {
        value = lhat_real(e->const_real);
    } else if (e->const_kind == LHAT_HOST_CONST_BOOL) {
        value = lhat_bool(e->const_bool);
    } else if (e->const_kind == LHAT_HOST_CONST_STRING) {
        LhatString *text =
            lhat_string_new(heap, e->const_text, strlen(e->const_text));
        if (text == NULL) {
            return false;
        }
        value = lhat_object((LhatObject *)text);
    } else if (e->call == NULL) {
        // A type registers as an empty table under its module; its members
        // are entries of their own and land in it as they come. The order
        // matters: a member arriving first would make the table, and the
        // type's own entry would then read a non-nil and wrap the two into
        // an overload. The registration API keeps the order.
        LhatTable *made = lhat_table_new(heap);
        if (made == NULL) {
            return false;
        }
        value = lhat_object((LhatObject *)made);
    } else {
        LhatHost *host = lhat_host_new(heap, e->call, e->context,
                                       e->parameters, e->has_variadic,
                                       e->takes_self);
        if (host == NULL) {
            return false;
        }
        host->self_last = e->self_last;
        host->signature = e->runtime_signature;
        host->parameter_types = borrowed_params(
            (const LhatRuntimeType *const *)e->parameter_types,
            e->parameters);
        if (e->parameters > 0 && host->parameter_types == NULL) {
            return false;
        }
        value = lhat_object((LhatObject *)host);
    }
    return shared_register(heap, root, e->module, e->type, e->name, value);
}

// A registered enum, on the program's heap. The declaration type is
// already there (lhat_type_rt_new against host_heap at registration).
static bool shared_enum(LhatProgram *program, LhatTable *root,
                        const LhatProgramEnum *e)
{
    LhatHeap *heap = &program->host_heap;
    LhatString *named = lhat_string_new(heap, e->name, strlen(e->name));
    if (named == NULL) {
        return false;
    }
    LhatEnum *made = lhat_enum_new(heap, named, e->decl_rt);
    if (made == NULL) {
        return false;
    }
    for (size_t i = 0; i < e->count; i++) {
        LhatString *member =
            lhat_string_new(heap, e->members[i], strlen(e->members[i]));
        if (member == NULL) {
            return false;
        }
        LhatEnumerator *made_one = lhat_enumerator_new(
            heap, made, member,
            lhat_integer(e->values != NULL ? e->values[i] : (LhatInteger)i),
            i + 1);
        bool refused = false;
        if (made_one == NULL ||
            !lhat_table_set(made->members, lhat_object((LhatObject *)member),
                            lhat_object((LhatObject *)made_one), &refused) ||
            refused) {
            return false;
        }
    }
    return shared_register(heap, root, e->module, e->type, e->name,
                           lhat_object((LhatObject *)made));
}

// [modules.hostdata.base], done once: a derived type's members table stands on its
// base's, so a member the base declares is found by walking. `definition` is
// the link table_get_in already climbs, and `is_definition` makes the climb
// [syntax.obj.compose]'s walk up to a base, which passes everything. It is a fact about the
// declarations, so it belongs to the build.
static bool shared_link_bases(LhatProgram *program, LhatTable *root)
{
    LhatHeap *heap = &program->host_heap;
    for (size_t i = 0; i < program->host_type_entry_count; i++) {
        const LhatHostTypeEntry *te = &program->host_type_entries[i];
        const LhatHostDataTag *base = te->tag->base;
        if (base == NULL) {
            continue;
        }
        LhatTable *derived = shared_reach(heap, root, te->module);
        LhatTable *under = shared_reach(heap, root, base->module);
        if (derived == NULL || under == NULL) {
            return false;
        }
        derived = shared_reach(heap, derived, te->name);
        under = shared_reach(heap, under, base->name);
        if (derived == NULL || under == NULL) {
            return false;
        }
        derived->definition = under;
        derived->is_definition = true;
        under->is_definition = true;
    }
    return true;
}

// Every table under `table`, sealed and marked shared. What a program writes
// at L^.modules is refused by SETINDEX from here on, and what a host writes by
// vm_set_key, which is what keeps the promise the black birth is made under.
// An enum's members are a table too, and reach_table walks into them.
static void share_tree(LhatTable *table)
{
    if (table == NULL || table->shared) {
        return;
    }
    table->sealed = true;
    table->shared = true;
    for (size_t i = 0; i < table->entry_capacity; i++) {
        LhatValue held = table->entries[i].value;
        if (lhat_is_object_kind(held, LHAT_OBJECT_TABLE)) {
            share_tree((LhatTable *)lhat_as_object(held));
        } else if (lhat_is_object_kind(held, LHAT_OBJECT_ENUM)) {
            share_tree(((LhatEnum *)lhat_as_object(held))->members);
        }
    }
}

// The module a registration named, by number over both lists -- an enum may
// be the only thing registered under its module (a lone `k.Mode`), so the
// entries alone do not name every module there is.
static const char *registered_module(const LhatProgram *program, size_t at)
{
    if (at < program->host_entry_count) {
        return program->host_entries[at].module;
    }
    at -= program->host_entry_count;
    return at < program->host_enum_count ? program->host_enums[at].module
                                         : NULL;
}

static size_t registered_module_count(const LhatProgram *program)
{
    return program->host_entry_count + program->host_enum_count;
}

// Whether `path` has another registered module path above it. Where one
// does, only the shallower is shared -- the deeper is inside it already,
// and hanging both off a machine would mean writing into a sealed table.
static bool has_shared_ancestor(const LhatProgram *program, const char *path)
{
    for (size_t i = 0; i < registered_module_count(program); i++) {
        const char *other = registered_module(program, i);
        size_t length = strlen(other);
        if (length < strlen(path) && strncmp(other, path, length) == 0 &&
            path[length] == '.') {
            return true;
        }
    }
    return false;
}

static bool remember_shared(LhatProgram *program, const char *path,
                            LhatTable *table)
{
    for (size_t i = 0; i < program->shared_count; i++) {
        if (strcmp(program->shared[i].path, path) == 0) {
            return true;  // one entry per module, however many registrations
        }
    }
    LHAT_GROW(program->shared, program->shared_count,
              program->shared_capacity, 8, return false);
    program->shared[program->shared_count].path = path;
    program->shared[program->shared_count].table = table;
    program->shared_count++;
    return true;
}

// The whole of it. Answers false only when memory ran out, and install with
// it.
static bool build_shared_modules(LhatProgram *program)
{
    if (program->shared_ready &&
        program->shared_from_entries == program->host_entry_count &&
        program->shared_from_types == program->host_type_entry_count &&
        program->shared_from_enums == program->host_enum_count) {
        return true;
    }
    // A registration has arrived since the last build. The old tables stay
    // where they are -- host_heap is freed with the program, and machines
    // installed against them go on reading them -- and this makes new ones.
    // A machine installed before a registration does not have it, which is
    // what install answered before this existed too.
    program->shared_count = 0;
    program->shared_ready = false;

    LhatHeap *heap = &program->host_heap;
    // Scaffolding: the shape L^.modules has, so that a path is reached the
    // same way it is at install. Only the nodes named by a registration are
    // kept; what stands above them is a machine's to make.
    LhatTable *root = lhat_table_new(heap);
    if (root == NULL) {
        return false;
    }
    for (size_t i = 0; i < program->host_entry_count; i++) {
        if (!shared_entry(program, root, &program->host_entries[i])) {
            return false;
        }
    }
    if (!shared_link_bases(program, root)) {
        return false;
    }
    for (size_t i = 0; i < program->host_enum_count; i++) {
        if (!shared_enum(program, root, &program->host_enums[i])) {
            return false;
        }
    }
    for (size_t i = 0; i < registered_module_count(program); i++) {
        const char *path = registered_module(program, i);
        if (has_shared_ancestor(program, path)) {
            continue;
        }
        LhatTable *table = shared_reach(heap, root, path);
        if (table == NULL || !remember_shared(program, path, table)) {
            return false;
        }
    }
    for (size_t i = 0; i < program->shared_count; i++) {
        share_tree(program->shared[i].table);
    }
    program->shared_ready = true;
    program->shared_from_entries = program->host_entry_count;
    program->shared_from_types = program->host_type_entry_count;
    program->shared_from_enums = program->host_enum_count;
    return true;
}

bool prg_install_all(const LhatProgram *program, LhatMachine *machine)
{
    // [modules.host.shared-registry]: the registrations, the types they stand under and the
    // enums, all built once on the program's heap and hung here by name.
    // What was a value object and a key string per registration is now a
    // pointer per module.
    LhatProgram *writable = (LhatProgram *)program;
    if (!build_shared_modules(writable)) {
        return false;
    }
    for (size_t i = 0; i < program->shared_count; i++) {
        if (!lhat_machine_attach_module(machine, program->shared[i].path,
                                        program->shared[i].table)) {
            return false;
        }
    }

    // [modules.hostdata.base]'s base links and [modules.host.panic]'s enums used to be built here, once per
    // machine. They are facts about the declarations rather than about any
    // machine, so build_shared_modules has them (shared_link_bases,
    // shared_enum).
    // [modules.host.machine]: what goes in L^ itself rather than under its registry.
    for (size_t i = 0; i < program->global_count; i++) {
        const LhatGlobalEntry *e = &program->global_entries[i];
        LhatValue value = lhat_nil();
        if (!lhat_machine_make_host(
                machine, e->call, e->context, e->parameters, e->has_variadic,
                e->takes_self, e->self_last,
                borrowed_params((const LhatRuntimeType *const *)
                                    e->parameter_types,
                                e->parameters),
                &value) ||
            !lhat_machine_set_global(machine, e->name, value)) {
            return false;
        }
        ((LhatHost *)lhat_as_object(value))->signature = e->runtime_signature;
    }
    // [modules.hostvalue]: a host value has no heap half to carry its members table,
    // so the machine keeps one per registered type, found by the tag's
    // index. The tables themselves are the ones the entry loop above put
    // under L^.modules -- reachable from the environment, so the collector
    // needs nothing new.
    //
    // [modules.require.reload] with registry.h: the index is the process's, so the array is taken
    // to the width every declared type reaches rather than to how many this
    // program declared -- otherwise a program declaring the second of two
    // types would index past its own array.
    if (program->hostvalue_type_entry_count > 0 &&
        !lhat_machine_bind_hostvalues(machine,
                                      program->hostvalue_type_entries,
                                      program->hostvalue_type_entry_count,
                                      lhat_registry_hostvalue_count())) {
        return false;
    }
    return true;
}

bool lhat_program_install(const LhatProgram *program, LhatMachine *machine)
{
    // [modules.host.lock]: a read of the registrations and the unit table, and still
    // under the lock -- those are what a check or a load writes, and
    // std.thread's worker is the caller this exists for.
    LhatProgram *writable = (LhatProgram *)program;
    lhat_program_hold(writable);
    bool ok = prg_install_all(program, machine);
    // [l10n.choose.machine]: and this is whose language the machine says things in.
    lhat_machine_hold_program(machine, program);
    lhat_program_release(writable);
    return ok;
}

void lhat_program_init(LhatProgram *program, bool strict,
                       LhatProgramLoader load, void *context)
{
    memset(program, 0, sizeof *program);
    lhat_type_arena_init(&program->types);
    program->strict = strict;
    program->load = load;
    program->loader_context = context;
    // See lhat_proto_new's comment: born black so a machine reading a
    // host-registered error kind never writes into program->host_heap
    // -- required once more than one machine (std.thread) can read it.
    program->host_heap.white = LHAT_GC_BLACK;
    // [modules.require.reload]: what discarded bodies leave behind. Black for the same reason --
    // they came off chunk heaps that were born black, and no machine's
    // collection may write into them.
    program->retired_objects.white = LHAT_GC_BLACK;
}
