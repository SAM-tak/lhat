// L^ (lhat) -- the type checking stage.


#include "check_internal.h"
#include "instantiation_internal.h"
#include "message.h"
#include "registry.h"

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

size_t lhat_check_error_count(const LhatCheckResult *result)
{
    size_t count = 0;
    for (size_t i = 0; i < result->diagnostic_count; i++) {
        if (result->strict || !result->diagnostics[i].relaxed_ok) count++;
    }
    return count;
}

void chk_report_fix(Checker *c, const LhatNode *at, LhatCheckErrorCode code,
                    const char *name, size_t name_length,
                    const LhatFixSlot *fixes, size_t count)
{
    // [syntax.type.fits]'s narrowing reads a path the caller has already inferred, so
    // whatever is wrong with it has been said once. One mistake is one
    // diagnostic ([pipeline.infer.rounds] keeps a ring to one as well).
    if (at == NULL || c->rereading > 0) {
        return;
    }

    LhatCheckResult *r = c->result;
    LHAT_GROW(r->diagnostics, r->diagnostic_count, r->diagnostic_capacity, 8,
              return);

    LhatCheckDiagnostic *d = &r->diagnostics[r->diagnostic_count++];
    d->cause = NULL;
    d->cause_path = NULL;
    d->left_type = NULL;
    d->right_type = NULL;
    d->code = code;
    d->relaxed_ok = code == LHAT_CHECK_ERR_ACCESS_ON_MAYBE_NIL ||
                   code == LHAT_CHECK_ERR_OPERATOR_ON_MAYBE_NIL ||
                   code == LHAT_CHECK_ERR_RESULT_UNDECIDED ||
                   code == LHAT_CHECK_ERR_PARAM_UNDECIDED ||
                   code == LHAT_CHECK_ERR_TYPE_UNDECIDED;
    d->offset = at->offset;
    d->line = at->line;
    d->column = at->column;
    d->name = name;
    d->name_length = name != NULL ? (uint32_t)name_length : 0;
    d->highlight_length = 0;
    for (size_t i = 0; i < LHAT_FIX_SLOTS; i++) {
        d->fixes[i].title = NULL;
    }
    for (size_t i = 0; i < count && i < LHAT_FIX_SLOTS; i++) {
        d->fixes[i] = fixes[i];
    }
}

void chk_report(Checker *c, const LhatNode *at, LhatCheckErrorCode code)
{
    chk_report_fix(c, at, code, NULL, 0, NULL, 0);
}

// [lsp.next]: one text the result keeps for a fix.
struct LhatFixText {
    struct LhatFixText *next;
    char text[];
};

static const char *keep_text(LhatCheckResult *result, const char *text,
                             size_t length)
{
    struct LhatFixText *kept =
        (struct LhatFixText *)lhat_alloc(sizeof *kept + length + 1);
    if (kept == NULL) {
        return NULL;
    }
    memcpy(kept->text, text, length);
    kept->text[length] = '\0';
    kept->next = result->fix_texts;
    result->fix_texts = kept;
    return kept->text;
}

static unsigned char fold(unsigned char byte)
{
    return byte >= 'A' && byte <= 'Z' ? (unsigned char)(byte - 'A' + 'a')
                                      : byte;
}

// Optimal string alignment: insertions, deletions and substitutions, and a
// swap of two neighbours as one edit -- 'lenght' is one away from 'length',
// which is the mistake a hand makes. Both lengths are under CHK_NEAR_NAME.
static size_t distance_between(const char *a, size_t a_length, const char *b,
                               size_t b_length)
{
    size_t rows[3][CHK_NEAR_NAME + 1];
    size_t *two = rows[0];
    size_t *one = rows[1];
    size_t *now = rows[2];
    for (size_t j = 0; j <= b_length; j++) {
        one[j] = j;
    }
    for (size_t i = 1; i <= a_length; i++) {
        now[0] = i;
        unsigned char x = fold((unsigned char)a[i - 1]);
        for (size_t j = 1; j <= b_length; j++) {
            unsigned char y = fold((unsigned char)b[j - 1]);
            size_t best = one[j] + 1;
            if (now[j - 1] + 1 < best) {
                best = now[j - 1] + 1;
            }
            if (one[j - 1] + (x == y ? 0 : 1) < best) {
                best = one[j - 1] + (x == y ? 0 : 1);
            }
            if (i > 1 && j > 1 && x == fold((unsigned char)b[j - 2]) &&
                fold((unsigned char)a[i - 2]) == y && two[j - 2] + 1 < best) {
                best = two[j - 2] + 1;
            }
            now[j] = best;
        }
        size_t *spare = two;
        two = one;
        one = now;
        now = spare;
    }
    return one[b_length];
}

bool chk_nearest_start(Checker *c, ChkNearest *near, const char *written,
                       size_t length)
{
    near->written = written;
    near->written_length = length;
    near->best_length = 0;
    near->distance = 0;
    return c->rereading == 0 && written != NULL && length > 0 &&
           length < CHK_NEAR_NAME;
}

void chk_nearest_offer(ChkNearest *near, const char *name, size_t length)
{
    if (name == NULL || length == 0 || length >= CHK_NEAR_NAME ||
        near->written == NULL) {
        return;
    }
    // The name itself is not a suggestion: where it exists and was still
    // refused, the reason is something other than its spelling.
    if (length == near->written_length &&
        memcmp(name, near->written, length) == 0) {
        return;
    }
    size_t allowed = near->written_length / 3 > 0 ? near->written_length / 3
                                                   : 1;
    size_t apart = length > near->written_length ? length - near->written_length
                                                 : near->written_length - length;
    if (apart > allowed) {
        return;
    }
    size_t distance =
        distance_between(near->written, near->written_length, name, length);
    if (distance > allowed ||
        (near->best_length > 0 && distance >= near->distance)) {
        return;
    }
    memcpy(near->best, name, length);
    near->best_length = length;
    near->distance = distance;
}

void chk_nearest_member(void *context, const LhatTypeMember *member)
{
    chk_nearest_offer((ChkNearest *)context, member->name, member->name_length);
}

void chk_nearest_builtin(void *context, const char *name, size_t length,
                         LhatType *type)
{
    (void)type;
    chk_nearest_offer((ChkNearest *)context, name, length);
}

void chk_report_near(Checker *c, const LhatNode *at, LhatCheckErrorCode code,
                     const char *name, size_t length, const LhatNode *spelt,
                     const ChkNearest *near)
{
    LhatFixSlot fix;
    size_t count = 0;
    if (at != NULL && spelt != NULL && spelt->end > spelt->offset &&
        near->best_length > 0 && c->rereading == 0) {
        const char *kept = keep_text(c->result, near->best, near->best_length);
        if (kept != NULL) {
            // Suggested: a near name is a guess at what was meant, and the
            // name that was written may be one still to be made.
            fix = lhat_fix_slot(LHAT_FIX_NEAR_NAME, LHAT_FIX_SUGGESTED,
                                spelt->offset, spelt->end - spelt->offset,
                                kept);
            count = 1;
        }
    }
    chk_report_fix(c, at, code, name, length, &fix, count);
}

void chk_member_declared_at(Checker *c, LhatTypeMember *member,
                           const LhatNode *at)
{
#if LHAT_WITH_RESOLUTIONS
    if (member == NULL || at == NULL) {
        return;
    }
    member->declared_at = at->offset;
    // program.c names a source by the path it loaded, and the unit owns both
    // for as long as the program does -- so the member may borrow it and a
    // reader of the type still knows which file to open.
    member->declared_in = c->lexer != NULL && c->lexer->source != NULL
                              ? c->lexer->source->name
                              : NULL;
#else
    (void)c;
    (void)member;
    (void)at;
#endif
}

void chk_kind_declared_at(Checker *c, LhatType *kind, const LhatNode *at)
{
#if LHAT_WITH_RESOLUTIONS
    if (kind == NULL || at == NULL) {
        return;
    }
    kind->v.error.declared_at = at->offset;
    kind->v.error.declared_in = c->lexer != NULL && c->lexer->source != NULL
                                    ? c->lexer->source->name
                                    : NULL;
#else
    (void)c;
    (void)kind;
    (void)at;
#endif
}

#if LHAT_WITH_RESOLUTIONS
// [lsp.hover]: what a written name turned out to mean. Kept so that a tool
// reads the walk's own answer rather than resolving [lton.host]'s scoping a second
// time. Dropped rather than failing the check when there is no room -- what
// it feeds is a convenience, and the program is no less checked without it.
//
// Nothing below this point is read by the language: [pipeline.stages.tree]'s later stages
// compile and run the same tree either way. The call sites carry the same
// guard, so a build without it does not so much as make the call.
static void record_resolution(Checker *c, const LhatNode *at, const Binding *b,
                              LhatType *type)
{
    LhatCheckResult *r = c->result;
    if (at->end <= at->offset) {
        return;  // no span: nothing to hover over
    }
    LHAT_GROW(r->resolutions, r->resolution_count, r->resolution_capacity, 64,
              return);

    LhatResolution *entry = &r->resolutions[r->resolution_count++];
    entry->use = at->offset;
    entry->use_end = at->end;
    entry->definition = b->offset;
    entry->has_definition = true;
    entry->is_parameter = b->is_parameter;
    entry->immutable = b->immutable;
    entry->builtin = false;  // a name a scope holds is the program's own
    entry->definition_path = NULL;  // bound in this unit, by this scope
    entry->type = type;
    entry->named_kind = NULL;
    entry->call_signature = NULL;
}

void chk_record_resolution(Checker *c, const LhatNode *at, const Binding *b)
{
    record_resolution(c, at, b, b->type);
}

// [syntax.type.fits]: inside a branch that established something about a name, what the
// name holds is narrower than what its binding says -- and it is the same
// name, bound by the same word in the same place. So everything else about
// it is the binding's answer and only the type is the branch's.
void chk_record_narrowed_resolution(Checker *c, const LhatNode *at,
                                    const Binding *b, LhatType *type)
{
    record_resolution(c, at, b, type);
}

// The same, for a name whose meaning has no place in this source to point
// at. [syntax.obj.structure]'s member is found in a type rather than in a scope, and the type
// may have come from another unit or a host registration; [modules.host.initial]'s
// host-bound name was registered in C; [modules.host.machine]'s L^ is the machine itself;
// [syntax.sub.this]'s this^ and [syntax.obj.super]'s super^ are given by the body around them.
// What every one of them still has is a type, which is what a reader asking
// "what is this" wanted.
void chk_record_typed_resolution(Checker *c, const LhatNode *at,
                                 LhatType *type)
{
    chk_record_member_resolution(c, at, type, NULL, false);
}


// A name whose meaning was found somewhere other than a scope -- in a type
// ([syntax.obj.structure]), in the set that declared it ([errors.kind.errordef], [syntax.enum]), in the unit a
// require^ reached ([modules.require]). `path` is the unit `definition` is an offset
// into, and doubles as whether there is a place at all: NULL is the "no
// place to point at" check.h describes, which a host registration and a
// built-in keep.
static void record_placed(Checker *c, const LhatNode *at, LhatType *type,
                          uint32_t definition, const char *path, bool builtin,
                          const LhatType *named_kind)
{
    LhatCheckResult *r = c->result;
    if (at == NULL || at->end <= at->offset) {
        return;  // no span: nothing to hover over
    }
    LHAT_GROW(r->resolutions, r->resolution_count, r->resolution_capacity, 64,
              return);

    LhatResolution *entry = &r->resolutions[r->resolution_count++];
    entry->use = at->offset;
    entry->use_end = at->end;
    entry->definition = path != NULL ? definition : 0;
    entry->has_definition = path != NULL;
    entry->definition_path = path;
    // Neither question applies to any of these. A member is not what a
    // signature declared, and [syntax.stmt.let]'s let^/var^ is about a name a scope holds
    // -- what a table's member may be written through is [syntax.sub.table-write] and
    // [modules.host.machine]'s question, asked of the table rather than of the name. Saying
    // "immutable" here would be answering a different question with this
    // one's word.
    entry->is_parameter = false;
    entry->immutable = false;
    entry->builtin = builtin;
    entry->type = type;
    entry->named_kind = named_kind;
    entry->call_signature = NULL;
}

void chk_record_member_resolution(Checker *c, const LhatNode *at,
                                  LhatType *type,
                                  const LhatTypeMember *member, bool builtin)
{
    // [syntax.obj.structure] looks a member up in a type, and the type says where it was
    // written -- in this unit or in the one that published it ([modules.deps.static]).
    record_placed(c, at, type, member != NULL ? member->declared_at : 0,
                  member != NULL ? member->declared_in : NULL, builtin, NULL);
}

void chk_record_kind_resolution(Checker *c, const LhatNode *at, LhatType *type,
                                const LhatType *kind)
{
    record_placed(c, at, type, kind != NULL ? kind->v.error.declared_at : 0,
                  kind != NULL ? kind->v.error.declared_in : NULL, false, kind);
}

void chk_record_unit_resolution(Checker *c, const LhatNode *at,
                                LhatType *exports, const char *unit_path)
{
    // The whole unit is what was named, so its start is the place.
    record_placed(c, at, exports, 0, unit_path, false, NULL);
}

static void record_try_route(Checker *c, const LhatNode *site,
                             LhatTryRouteKind kind, const LhatNode *target,
                             LhatType *errors)
{
    LhatCheckResult *r = c->result;
    // A body walked again, or once for each shape it is called with, finds its way
    // to the same places.
    for (size_t i = r->try_route_count; i-- > 0;) {
        LhatTryRoute *route = &r->try_routes[i];
        if (route->at == site->offset && route->kind == kind &&
            route->target == target) {
            if (route->errors != NULL && errors != NULL) {
                route->errors = lhat_type_union(
                    r->types, (LhatType *)route->errors, errors);
            }
            return;
        }
    }
    LHAT_GROW(r->try_routes, r->try_route_count, r->try_route_capacity, 16,
              return);
    r->try_routes[r->try_route_count++] =
        (LhatTryRoute){site->offset, kind, target, errors};
}

void chk_try_route_leaves(Checker *c, const LhatNode *site, LhatType *errors,
                          bool undecided)
{
    if (errors == NULL && !undecided) {
        return;
    }
    if (c->catch_frame == NULL) {
        if (c->this_link != NULL) {
            record_try_route(c, site, LHAT_TRY_TO_BODY, c->this_link->body, errors);
        } else {
            record_try_route(c, site, LHAT_TRY_TO_UNIT, NULL, errors);
        }
        return;
    }
    struct TryPending *pending =
        lhat_type_semantic_alloc(c->result->types, sizeof *pending);
    if (pending == NULL) {
        return;
    }
    *pending = (struct TryPending){site, errors, undecided, c->catch_frame->pending};
    c->catch_frame->pending = pending;
}

void chk_try_routes_reach_arm(Checker *c, struct CatchFrame *frame,
                              const LhatNode *arm, LhatType *want)
{
    for (struct TryPending *p = frame->pending; p != NULL; p = p->next) {
        if (p->undecided) {
            record_try_route(c, p->site, LHAT_TRY_TO_ARM, arm, NULL);
            if (want == NULL) {
                p->undecided = false;  // a bare arm takes whatever it turns out to be
            }
            continue;
        }
        LhatType *here = want != NULL ? chk_only(c, p->left, want) : p->left;
        if (here != NULL) {
            record_try_route(c, p->site, LHAT_TRY_TO_ARM, arm, here);
        }
        p->left = want != NULL ? chk_without(c, p->left, want) : NULL;
    }
}

void chk_try_routes_pass_on(Checker *c, struct CatchFrame *frame)
{
    for (struct TryPending *p = frame->pending; p != NULL; p = p->next) {
        chk_try_route_leaves(c, p->site, p->left, p->undecided);
    }
}

void chk_record_member_site(Checker *c, const LhatNode *node,
                            LhatType *receiver, bool number_word)
{
    LhatCheckResult *r = c->result;
    if (node == NULL) {
        return;
    }
    LHAT_GROW(r->member_sites, r->member_site_count, r->member_site_capacity,
              32, return);

    LhatMemberSite *site = &r->member_sites[r->member_site_count++];
    // access_node (parser.c) makes the node from the dot's own token, so the
    // offset is the operator and the end is one past whatever followed it.
    site->dot = node->offset;
    site->end = node->end;
    site->nil_safe = node->v.access.nil_safe;
    site->number_word = number_word;
    site->receiver = receiver;
}

static int compare_resolutions(const void *a, const void *b)
{
    const LhatResolution *ra = (const LhatResolution *)a;
    const LhatResolution *rb = (const LhatResolution *)b;
    if (ra->use != rb->use) {
        return ra->use < rb->use ? -1 : 1;
    }
    return 0;
}

// lhat_check_resolution_at halves its way in, so the array has to be ordered
// by `use`. The walk very nearly gives that for free -- names are met in
// source order -- but not quite: a member is recorded after the target it
// was read from, and [syntax.obj.members-order]'s two passes over a def^ meet the same member
// twice. So the order is made rather than assumed.
//
// A repeat is the same name recorded again, and the later record is the one
// to keep: the second pass over a definition writes the inferred type over
// the provisional one the first pass left ([syntax.obj.members-order]).
void chk_settle_resolutions(LhatCheckResult *result)
{
    if (result->resolution_count < 2) {
        return;
    }
    qsort(result->resolutions, result->resolution_count,
          sizeof *result->resolutions, compare_resolutions);

    size_t kept = 0;
    for (size_t i = 0; i < result->resolution_count; i++) {
        // qsort is not stable, so "the later record" cannot be read off the
        // order after sorting -- but a repeat is a name recorded twice with
        // the same span, and what differs is the type. Keeping the last of
        // an equal run is what the two-pass case wants; where nothing
        // differs, either is the same answer.
        bool same_as_kept = kept > 0 &&
                            result->resolutions[kept - 1].use ==
                                result->resolutions[i].use;
        if (same_as_kept) {
            result->resolutions[kept - 1] = result->resolutions[i];
        } else {
            result->resolutions[kept++] = result->resolutions[i];
        }
    }
    result->resolution_count = kept;
}

static int compare_member_sites(const void *a, const void *b)
{
    const LhatMemberSite *sa = (const LhatMemberSite *)a;
    const LhatMemberSite *sb = (const LhatMemberSite *)b;
    if (sa->dot != sb->dot) {
        return sa->dot < sb->dot ? -1 : 1;
    }
    return 0;
}

// The same ordering and the same reason as chk_settle_resolutions, on the
// same two-pass repeat: [syntax.obj.members-order] walks a def^ twice and the second walk knows
// the receiver the first could only guess at.
void chk_settle_member_sites(LhatCheckResult *result)
{
    if (result->member_site_count < 2) {
        return;
    }
    qsort(result->member_sites, result->member_site_count,
          sizeof *result->member_sites, compare_member_sites);

    size_t kept = 0;
    for (size_t i = 0; i < result->member_site_count; i++) {
        bool same_as_kept = kept > 0 && result->member_sites[kept - 1].dot ==
                                            result->member_sites[i].dot;
        if (same_as_kept) {
            result->member_sites[kept - 1] = result->member_sites[i];
        } else {
            result->member_sites[kept++] = result->member_sites[i];
        }
    }
    result->member_site_count = kept;
}
#endif  // LHAT_WITH_RESOLUTIONS

// The same, about a name. The text is borrowed from the source, which [debug.godot]
// keeps alive as long as the result -- a copy per diagnostic would be paid
// for by every program, and almost none of them read one.
// [lsp.next]: an annotation that was already written, or that another answers, is
// one to take off. The whole of it goes, and the line's end with it --
// [syntax.annot.where] puts one on its own line, so leaving the line blank would be a second
// thing for the writer to tidy. Answers how many fixes were worked out, which
// is none for an annotation the source does not account for.
static size_t annotation_removal(const Checker *c, const LhatNode *at,
                                 LhatFixSlot *out)
{
    const LhatSource *source = c->lexer != NULL ? c->lexer->source : NULL;
    if (source == NULL || at->end <= at->offset) {
        return 0;
    }
    size_t through = at->end;
    while (through < source->length &&
           (source->text[through] == ' ' || source->text[through] == '\t')) {
        through++;
    }
    if (through < source->length && source->text[through] == '\n') {
        through++;
    }
    // [syntax.annot.spelling] makes the '@' a mark on the name rather than a character of it, so
    // the name's own span begins one byte in. What takes the annotation off
    // takes its mark with it.
    uint32_t from = at->offset;
    if (from > 0 && source->text[from - 1] == '@') {
        from--;
    }
    *out = lhat_fix_slot(LHAT_FIX_REMOVE_ANNOTATION, LHAT_FIX_SUGGESTED, from,
                         (uint32_t)(through - from), "");
    return 1;
}

void chk_report_named(Checker *c, const LhatNode *at,
                      LhatCheckErrorCode code, const char *name,
                      size_t length)
{
    chk_report_fix(c, at, code, name, length, NULL, 0);
}

void chk_report_named_span(Checker *c, const LhatNode *at,
                           LhatCheckErrorCode code, const char *name,
                           size_t length)
{
    size_t before = c->result->diagnostic_count;
    chk_report_named(c, at, code, name, length);
    if (c->result->diagnostic_count > before && at->end > at->offset) {
        c->result->diagnostics[before].highlight_length = at->end - at->offset;
    }
}

// ---------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------

// The text of an IDENT, HAT_IDENT or TYPE_NAME node. Names live as spans into
// the source, so nothing is copied.
//
// [lex.hat.repeat]: the hat is part of the name -- 'self^' is a different
// name from 'self'. A spelling with more hats than one is the same name
// reached further out (this^^^ is the this^ two levels up), so the
// ast.c's canonical-name reading ([lex.hat.repeat]), against this unit's source.
// One definition is what keeps the checker and the machine agreeing on the
// key a member lives under.
bool chk_node_name(const Checker *c, const LhatNode *node,
                   const char **text, size_t *length)
{
    return lhat_node_name(node, c->lexer->source->text, c->lexer->strings,
                          text, length);
}

bool chk_name_is(const char *text, size_t length, const char *literal)
{
    return lhat_name_is(text, length, literal);
}

// [syntax.type.discard]: whether this is '_^', the receptacle that binds nothing. Only the
// hatted spelling means it -- [syntax.type.discard] refuses `_x` on purpose, so an ordinary
// name beginning with an underscore is untouched.
bool chk_is_discard(const Checker *c, const LhatNode *node)
{
    const char *name = NULL;
    size_t length = 0;
    return node != NULL && node->kind == LHAT_NODE_HAT_IDENT &&
           chk_node_name(c, node, &name, &length) &&
           chk_name_is(name, length, "_^");
}

// [syntax.obj.super]: whether this is super^ written out. Only the hatted spelling means
// it, so an ordinary name `super` is untouched.
bool chk_is_super_name(Checker *c, const LhatNode *node)
{
    const char *name = NULL;
    size_t length = 0;
    return node != NULL && node->kind == LHAT_NODE_HAT_IDENT &&
           chk_node_name(c, node, &name, &length) &&
           chk_name_is(name, length, "super^");
}

// [lex.hat.repeat]: a hat name is the language's, so a written one declares nothing
// of the writer's. A member key may still be one of the two a value answers
// by whatever else it holds -- [syntax.obj.tostring-hat]'s tostring^ and [syntax.for.forms]'s iterate^, the
// pair lhat_member_held_as folds -- and a parameter may not be any.
void chk_refuse_hat_name(Checker *c, const LhatNode *node, bool member)
{
    const char *name = NULL;
    size_t length = 0;
    if (node == NULL || node->kind != LHAT_NODE_HAT_IDENT ||
        !chk_node_name(c, node, &name, &length)) {
        return;
    }
    if (member && lhat_member_held_as(name, length) != length) {
        return;
    }
    chk_report(c, node, LHAT_CHECK_ERR_HAT_NAME_RESERVED);
}

// ---------------------------------------------------------------------------
// Scopes ([syntax.stmt.visible])
// ---------------------------------------------------------------------------

Binding *chk_scope_find_local(Scope *scope, const char *name, size_t length)
{
    for (Binding *b = scope->bindings; b != NULL; b = b->next) {
        if (b->name_length == length && memcmp(b->name, name, length) == 0) {
            return b;
        }
    }
    return NULL;
}

// [syntax.sub.kinds] asks which scope answered, not only what it holds: an f^ may write to
// a name its own body made and to no other. `found_in` may be NULL for the
// callers that only want the binding.
Binding *chk_scope_find(Scope *scope, const char *name, size_t length, Scope **found)
{
    for (Scope *s = scope; s != NULL; s = s->parent) {
        Binding *b = chk_scope_find_local(s, name, length);
        if (b != NULL) {
            if (found != NULL) {
                *found = s;
            }
            return b;
        }
    }
    return NULL;
}

// [lex.hat.repeat]: the stacked reach, passing over the innermost `skip`
// bindings of the name -- it^^ is the it^ one binding out, self^^/def^^
// the enclosing def^'s. The same search order as scope_find, so the two
// agree on which binding is "innermost".
Binding *chk_scope_find_skipping(Scope *scope, const char *name,
                                 size_t length, size_t skip)
{
    for (Scope *s = scope; s != NULL; s = s->parent) {
        Binding *b = chk_scope_find_local(s, name, length);
        if (b != NULL) {
            if (skip == 0) {
                return b;
            }
            skip--;
        }
    }
    return NULL;
}

// [lex.scope]: where a scope specifier starts looking, counted either way.
//
// `$^` skips the scope the name was written in and searches from its
// parent, `$^^` from the one above that -- outwards from here. `$` names
// the outermost, which [modules.unit] makes the unit and [pipeline.machine] makes the
// session's top level; `$$` the one scope inside it and `$$$` the one
// inside that -- inwards from there. The lexer eats the first sigil before
// counting, so `depth` is the absolute index outright: 0 is the unit.
//
// Answers NULL when there are not that many scopes, which is the writer
// counting wrong rather than a name that is missing.
//
// The search from there is the ordinary one -- a specifier says where to
// begin, not where to stop, so an ancestor further out still answers.
Scope *chk_scope_from(Scope *scope, const LhatNode *node)
{
    // [syntax.loop.scope]: a transparent layer holds names without being a '{' anyone
    // wrote, so it is stepped over rather than counted. One step out of a
    // loop body reaches what encloses the loop, whether or not the body has
    // clauses -- which is the one scope compile.c counts for it too.
    if (node->v.scope.kind == LHAT_SCOPE_FILE) {
        // How many scopes stand between here and the outermost, so that an
        // absolute depth can be turned into that many steps outwards.
        uint32_t open = 0;
        for (Scope *at = scope; at != NULL && at->parent != NULL;
             at = at->parent) {
            if (!at->transparent) {
                open++;
            }
        }
        if (node->v.scope.depth > open) {
            return NULL;  // naming a scope further in than the one here
        }
        uint32_t out = open - node->v.scope.depth;
        for (uint32_t i = 0; i < out; i++) {
            if (scope == NULL) {
                return NULL;
            }
            do {
                scope = scope->parent;
            } while (scope != NULL && scope->transparent);
        }
        return scope;
    }
    for (uint32_t out = 0; out < node->v.scope.depth; out++) {
        if (scope == NULL) {
            return NULL;
        }
        do {
            scope = scope->parent;
        } while (scope != NULL && scope->transparent);
    }
    return scope;
}

Binding *chk_scope_add(Scope *scope, const char *name, size_t length,
                       LhatType *type, uint32_t offset)
{
    Binding *b = (Binding *)lhat_calloc(1, sizeof *b);
    if (b == NULL) {
        return NULL;
    }
    b->name = name;
    b->name_length = length;
    b->type = type;
    b->offset = offset;

    if (scope->bindings == NULL) {
        scope->bindings = b;
    } else {
        scope->tail->next = b;
    }
    scope->tail = b;
    return b;
}

void chk_scope_open(Scope *scope, Scope *parent, const LhatNode *node,
                    bool transparent)
{
    scope->bindings = NULL;
    scope->tail = NULL;
    scope->parent = parent;
    scope->transparent = transparent;
    scope->from = node != NULL ? node->offset : 0;
    scope->to = node != NULL ? node->end : 0;
}

#if LHAT_WITH_RESOLUTIONS
// [lsp.hover]: what the scope held, taken as it closes -- the one moment every
// name it ever had is in hand at once. A scope covering no source is a
// lookup table rather than a piece of a program and is passed over.
static void record_bindings(Checker *c, const Scope *scope)
{
    if (scope->to == 0) {
        return;
    }
    LhatCheckResult *r = c->result;
    for (const Binding *b = scope->bindings; b != NULL; b = b->next) {
        LHAT_GROW(r->binding_sites, r->binding_site_count,
                  r->binding_site_capacity, 32, return);

        LhatBindingSite *site = &r->binding_sites[r->binding_site_count++];
        site->from = scope->from;
        site->to = scope->to;
        site->name = b->name;
        site->name_length = (uint32_t)b->name_length;
        site->type = b->type;
        site->is_parameter = b->is_parameter;
        site->immutable = b->immutable;
        site->names_type = b->names_type;
        site->named_type = b->named_type;
    }
}
#endif

void chk_scope_close(Checker *c, Scope *scope)
{
    chk_freeze_templates(c, scope);
#if LHAT_WITH_RESOLUTIONS
    record_bindings(c, scope);
#else
    (void)c;
#endif
    Binding *b = scope->bindings;
    while (b != NULL) {
        Binding *next = b->next;
        lhat_free(b);
        b = next;
    }
    scope->bindings = NULL;
    scope->tail = NULL;
}

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

LhatType *chk_simple(Checker *c, LhatTypeKind kind)
{
    return lhat_type_simple(c->result->types, kind);
}

// [errors.kind.local] with [syntax.op.as-error]: localerror^.CastFailure as a type. Cached for the
// reason chk_typeinfo_type is: [errors.kind.nominal] makes identity the object, so a second
// mention of the name in the same unit has to come to the same one or
// conformance between them would answer false.
//
// The runtime object it stands for is the process's (registry.h); this is
// the checker's own type for the same declaration, made per check the way
// every other type in the arena is.
LhatType *chk_cast_failure_type(Checker *c)
{
    if (c->cast_failure_type != NULL) {
        return c->cast_failure_type;
    }
    // A kind with no set above it: [errors.kind.local] puts it under the top directly rather
    // than inside a declaration, since there is no declaration.
    LhatType *kind = lhat_type_error_kind(c->result->types, NULL,
                                          "localerror^.CastFailure", 23);
    if (kind != NULL) {
        kind->v.error.local = true;
        kind->v.error.runtime_kind = lhat_registry_cast_failure();
    }
    c->cast_failure_type = kind;
    return kind;
}

// [errors.kind.local]: what the operators mean by "an error". The two tops are
// disjoint, so neither alone stands for both -- catch^ removing error^ from
// 'T|CastFailure' would remove nothing and then say the left cannot fail.
LhatType *chk_any_error(Checker *c)
{
    return lhat_type_union(c->result->types,
                           lhat_type_error_top(c->result->types, false),
                           lhat_type_error_top(c->result->types, true));
}

// [errors.kind.local-no-propagate]. The rule is that a localerror^ may not be in what a caller
// receives, so this is asked of a result type and of a yield type.
//
// A NOMINAL TYPE IS AN ATOM. Walking into a def^'s or an error kind's
// declared fields would find `cause`, which [errors.kind.made] widened to take either
// family -- and then every error kind on earth would answer true here and
// nothing could be returned at all. What closes that hole is the written
// side instead: [errors.kind.local-no-propagate] refuses localerror^ as a declared field's type, so
// there is nothing inside a nominal type for this walk to have missed.
// A structure may hold itself, so the walk is bounded the way
// chk_mentions_function_coroutine's is and for the same reason.
bool chk_type_touches_local(const LhatType *type, unsigned depth)
{
    if (type == NULL || depth > 8) {
        return false;
    }
    if (lhat_type_is_local_error(type)) {
        return true;
    }
    switch (type->kind) {
        case LHAT_TYPE_UNION:
        case LHAT_TYPE_INTERSECT:
        case LHAT_TYPE_TUPLE:
            for (const LhatTypeList *arm = type->v.composite.arms; arm != NULL;
                 arm = arm->next) {
                if (chk_type_touches_local(arm->type, depth + 1)) {
                    return true;
                }
            }
            return false;

        case LHAT_TYPE_TABLE:
            // A structural table only. [syntax.obj.def]'s def^ and [modules.hostdata]'s host type
            // share this payload and are the atoms above; `nominal` and
            // `is_definition` are what tell them apart.
            if (type->v.table.nominal || type->v.table.is_definition) {
                return false;
            }
            for (const LhatTypeMember *m = type->v.table.members; m != NULL;
                 m = m->next) {
                if (chk_type_touches_local(m->type, depth + 1)) {
                    return true;
                }
            }
            return chk_type_touches_local(type->v.table.variadic, depth + 1) ||
                   chk_type_touches_local(type->v.table.index_key, depth + 1) ||
                   chk_type_touches_local(type->v.table.index_value, depth + 1);

        // [syntax.sub.no-contagion]: what a call to one of these is worth is what a caller
        // receives, so a signature carrying one out is caught here too.
        case LHAT_TYPE_FUNC:
            return chk_type_touches_local(type->v.func.result, depth + 1);

        case LHAT_TYPE_CORO:
            return chk_type_touches_local(type->v.coroutine.produce, depth + 1) ||
                   chk_type_touches_local(type->v.coroutine.result, depth + 1);

        default:
            return false;
    }
}

LhatType *chk_resolve_type(Checker *c, const LhatNode *node);
LhatType *chk_infer(Checker *c, const LhatNode *node);
LhatType *chk_environment_type(Checker *c);  // [modules.host.machine]
// [modules.hostvalue]: the escape rules, written where the escapes would happen.
Binding *chk_scope_find(Scope *scope, const char *name, size_t length, Scope **found);
bool chk_scope_within_body(Checker *c, const Scope *found);
bool chk_is_hostvalue(const LhatType *type);
LhatType *chk_without_nil_arm(Checker *c, LhatType *target);
LhatType *chk_typeinfo_type(Checker *c);     // [syntax.obj.typeof]
void chk_register_module_type(Checker *c, const char *module_name,
                              LhatType *exports);  // [modules.require.once]
LhatType *chk_hosted_module(Checker *c, const LhatNode *path);  // [modules.host.import]
void chk_check_statement(Checker *c, const LhatNode *node);
void chk_check_block_in_scope(Checker *c, const LhatNode *node);
LhatType *chk_collect_exports(Checker *c, const LhatNode *statements);
void chk_check_statements(Checker *c, const LhatNode *statements);
LhatType *chk_infer_def(Checker *c, const LhatNode *node, LhatType *base);
LhatType *chk_compose_definitions(Checker *c, const LhatNode *node,
                                  LhatType *left, LhatType *right);
LhatType *chk_instance_of(const LhatType *definition);
const LhatTypeMember *chk_unimplemented_member(const LhatType *definition);
const LhatTypeMember *chk_hole_of(const LhatType *definition, bool *field);
const LhatTypeMember *chk_find_member(const LhatType *table, const char *name,
                                      size_t length);
LhatType *chk_only(Checker *c, LhatType *type, LhatType *wanted);
LhatType *chk_without(Checker *c, LhatType *type, LhatType *unwanted);
ParamVar *chk_param_var_for(Checker *c, const LhatType *type);
bool chk_signature_accepts(const LhatType *func, LhatType *const *args,
                           size_t count, bool through_member);
void chk_check_write_target(Checker *c, const LhatNode *target);
void chk_check_opaque_write(Checker *c, const LhatNode *target);
bool chk_receiver_is_own_coroutine(Checker *c, const LhatNode *receiver);

// [syntax.obj.number]: one number type over integers and reals. The rest are the plain
// builtin spellings.
static LhatType *builtin_type(Checker *c, const char *name, size_t length)
{
    if (chk_name_is(name, length, "number^")) {
        return chk_simple(c, LHAT_TYPE_NUMBER);
    }
    if (chk_name_is(name, length, "string^")) {
        return chk_simple(c, LHAT_TYPE_STRING);
    }
    if (chk_name_is(name, length, "costate^")) {
        return chk_simple(c, LHAT_TYPE_COSTATE);  // [syntax.sub.save-load]
    }
    if (chk_name_is(name, length, "bool^")) {
        return chk_simple(c, LHAT_TYPE_BOOL);
    }
    if (chk_name_is(name, length, "nil^")) {
        return chk_simple(c, LHAT_TYPE_NIL);
    }
    if (chk_name_is(name, length, "any^")) {
        return chk_simple(c, LHAT_TYPE_ANY);
    }
    // [errors.kind.local]: the two tops. Disjoint families rather than one above the
    // other, which is what keeps error^ writable in a result type now that a
    // family which cannot be returned exists.
    if (chk_name_is(name, length, "error^")) {
        return lhat_type_error_top(c->result->types, false);
    }
    if (chk_name_is(name, length, "localerror^")) {
        return lhat_type_error_top(c->result->types, true);
    }
    return NULL;
}

// The written members of one t^{ ... } or of its self^{ ... } section, read
// into 'table'. The section itself is not a member and is passed over here --
// resolve_table_type reads it before any of this.
static void resolve_members_into(Checker *c, LhatType *table,
                                 const LhatNode *items, const LhatNode *node)
{
    // [syntax.obj.structure]: an entry with no name is the type of the next position. They
    // are counted the way a literal counts its own -- from zero, in written
    // order, with the named ones taking no place in the sequence.
    size_t position = 0;
    for (const LhatNode *m = items; m != NULL; m = m->next) {
        const char *name = NULL;
        size_t length = 0;
        if (m->v.entry.value != NULL &&
            m->v.entry.value->kind == LHAT_NODE_SELF_TABLE) {
            continue;
        }
        LhatType *member = chk_resolve_type(c, m->v.entry.value);
        // [modules.hostvalue]: a table member is never a host value, written no more
        // than inferred.
        if (chk_is_hostvalue(member)) {
            chk_report(c, m->v.entry.value != NULL ? m->v.entry.value : node,
                       LHAT_CHECK_ERR_HOSTVALUE_ESCAPES);
        }
        if (m->v.entry.computed) {
            table->v.table.index_key = chk_resolve_type(c, m->v.entry.key);
            table->v.table.index_value = member;
            chk_check_tuple_position(c, m->v.entry.value, member);
            LhatType *key = table->v.table.index_key;
            if (key->kind == LHAT_TYPE_NIL || key->kind == LHAT_TYPE_NONE ||
                lhat_type_tuple_width(key) > 0) {
                chk_report(c, m->v.entry.key, LHAT_CHECK_ERR_BAD_KEY);
            }
            if (chk_is_hostvalue(key)) {
                chk_report(c, m->v.entry.key, LHAT_CHECK_ERR_HOSTVALUE_ESCAPES);
            }
            continue;
        }
        // [syntax.type.variadic], [syntax.obj.structure]: the unbounded tail. 'value' is NULL for an untyped
        // '...', which [syntax.type.variadic] makes any^.
        if (m->v.entry.variadic) {
            table->v.table.variadic =
                m->v.entry.value != NULL ? member : chk_simple(c, LHAT_TYPE_ANY);
            continue;
        }
        if (m->v.entry.key == NULL) {
            // [syntax.obj.structure-repeat]: 'type[n]' is n positions of that type, which is what a
            // run of them written out is. Made one by one here, so nothing
            // downstream can tell the two spellings apart.
            uint32_t repeat = m->v.entry.repeat > 0 ? m->v.entry.repeat : 1;
            for (uint32_t i = 0; i < repeat; i++) {
                lhat_type_add_index_member(c->result->types, table, position++,
                                           member);
            }
            continue;
        }
        chk_refuse_hat_name(c, m->v.entry.key, true);
        if (!chk_node_name(c, m->v.entry.key, &name, &length)) {
            continue;
        }
        chk_check_tostring_shape(c, m, member, name, length, false);
        chk_member_declared_at(
            c, lhat_type_add_member(c->result->types, table, name, length,
                                    member),
            m->v.entry.key);
    }
    if (table->v.table.index_key != NULL) {
        LhatType *entries = lhat_type_table(c->result->types);
        entries->v.table.entries_known = true;
        entries->v.table.members = table->v.table.members;
        entries->v.table.variadic = table->v.table.variadic;
        chk_expect(c, node, entries, table, LHAT_CHECK_ERR_MISMATCH);
    }
}

static LhatType *resolve_table_type(Checker *c, const LhatNode *node)
{
    LhatType *table = lhat_type_table(c->result->types);
    // [syntax.type.self]: the structure is what Self^ inside it names, so it is bound
    // before the members are read -- a member's type is written after the
    // literal it belongs to has been opened, which is the whole point.
    struct SelfLink here = { table, c->self_link };
    c->self_link = &here;

    // [syntax.obj.members-order]: a self^{ ... } section makes this a definition, and says what
    // its instances carry. It is read first, and from then on [syntax.type.self]'s Self^
    // names the instance -- inside a def^ that is what the word means, and
    // this is the same structure written down ([syntax.obj.typeof]). The definition itself
    // is one structure further out, Self^^.
    const LhatNode *section = NULL;
    for (const LhatNode *m = node->v.list.items; m != NULL; m = m->next) {
        if (m->v.entry.value != NULL &&
            m->v.entry.value->kind == LHAT_NODE_SELF_TABLE) {
            section = m->v.entry.value;
            break;
        }
    }
    struct SelfLink within = { NULL, c->self_link };
    if (section != NULL) {
        LhatType *instance = lhat_type_table(c->result->types);
        // What the section makes this, said the same way chk_infer_def says
        // it of a def^ -- [syntax.obj.compose]'s '..' tells a definition from an instance of
        // one by this, and [syntax.obj.name]'s plain_table_type asks it of a value before
        // reading a bare name as a built-in member. Both questions have the
        // same answer whether the definition was written out or inferred.
        //
        // [modules.hostdata]'s from_definition is not set beside it. That one closes a
        // *value* to a member being added afterwards, and what is written
        // here is a demand a value meets -- one a def^ made carries it
        // already, and a plain table that happens to satisfy the shape is not
        // closed by having been asked for. Nor does either take part in
        // conformance ([syntax.op.semantic]), so what a name annotated with this accepts is
        // unchanged.
        table->v.table.is_definition = true;
        table->v.table.instance = instance;
        within.type = instance;
        c->self_link = &within;
        resolve_members_into(c, instance, section->v.list.items, node);
        // [syntax.obj.self]: a member is reached through the definition as well
        // ('let^ f = A.m'), so it stands on both. A field does not: the
        // template says what an instance carries, and nothing of it is on the
        // definition. Which is which is the same question [syntax.obj.members-order] asks of a
        // def^'s entries, so the written form says what a def^ builds.
        for (const LhatTypeMember *m = instance->v.table.members; m != NULL;
             m = m->next) {
            if (chk_takes_receiver(m->type)) {
                lhat_type_add_member(c->result->types, table, m->name,
                                     m->name_length, m->type);
            }
        }
    }

    resolve_members_into(c, table, node->v.list.items, node);
    c->self_link = here.outer;
    return table;
}

// [syntax.obj.self]: in a type the receiver is written as the first parameter, 'p^self^;'.
// [syntax.type.param-names] keeps names out of a type, so it arrives as a parameter whose type is
// the word self^ rather than as a named one.
static bool is_self_marker(const Checker *c, const LhatNode *param)
{
    const LhatNode *written = param->v.param.name != NULL ? param->v.param.name
                                                          : param->v.param.type;
    const char *name = NULL;
    size_t length = 0;
    return written != NULL && !param->v.param.variadic &&
           (written->kind == LHAT_NODE_HAT_IDENT ||
            written->kind == LHAT_NODE_TYPE_NAME) &&
           chk_node_name(c, written, &name, &length) &&
           chk_name_is(name, length, "self^");
}

// Where the receiver stands in a written parameter list: 0 when this parameter
// is not the marker at all, 1 when it leads, 2 when it trails.
//
// [syntax.op.right]: a trailing self^ says the receiver is the RIGHT operand, which
// only an op^ may mean -- 'op^+ = f^lhs:number^, self^' is what answers
// '1 + v', where the left operand can carry no answer of its own. infer_def
// refuses it on any other member ([syntax.obj.self]: everywhere else the receiver is what
// stands before the dot).
//
// Written at both ends it counts as both, leaving a signature with no ordinary
// parameter at all -- which [syntax.op.define]'s shape rule then reports for an op^, and the
// rule above for anything else. Neither needs a case here.
int chk_self_marker_at(const Checker *c, const LhatNode *params,
                       const LhatNode *param)
{
    if (!is_self_marker(c, param)) {
        return 0;
    }
    if (param == params) {
        return 1;
    }
    // A self^ written in the middle is not a marker: it stays a parameter
    // whose type is the word self^, which [syntax.type.signature] has no such type for.
    return param->next == NULL ? 2 : 0;
}

LhatType *chk_resolve_func_type(Checker *c, const LhatNode *node)
{
    LhatType *outer_signature = c->argument_signature;
    bool outer_receiver = c->receiver_type_expression;
    for (const LhatNode *p = node->v.func.params; p; p = p->next) {
        if (chk_self_marker_at(c, node->v.func.params, p)) c->receiver_type_expression = true;
    }
    c->argument_signature = NULL;
    LhatType *func = lhat_type_func(c->result->types, node->v.func.is_function);
    func->v.func.answers_fresh = node->v.func.answers_fresh;  // [syntax.sub.fresh]
    for (const LhatNode *param = node->v.func.params; param != NULL;
         param = param->next) {
        int marker = chk_self_marker_at(c, node->v.func.params, param);
        if (marker != 0) {
            func->v.func.takes_self = true;
            func->v.func.self_last = marker == 2;
            // [syntax.sub.mutable-self]: the marker travels with the seat. On a p^ it says
            // nothing a p^ could not already do, so it is not recorded.
            if (param->v.param.mutable_receiver && func->v.func.is_function) {
                func->v.func.mutable_self = true;
            }
            continue;
        }
        if (param->v.param.variadic) {
            func->v.func.variadic = param->v.param.type != NULL
                                        ? chk_resolve_type(c, param->v.param.type)
                                        : chk_simple(c, LHAT_TYPE_ANY);
            continue;
        }
        lhat_type_add_param(c->result->types, func,
                            param->v.param.type != NULL
                                ? chk_resolve_type(c, param->v.param.type)
                                : chk_simple(c, LHAT_TYPE_PENDING));
    }
    if (node->v.func.return_type != NULL) {
        // [syntax.type.tuple]: a result is one of the few positions a tuple may be written
        // in. The parameters above were resolved without the permission, so
        // '(A, B)' as an argument is already refused -- which is what leaves
        // [syntax.type.variadic]'s expansion rule with nothing to expand.
        c->tuple_allowed = true;
        c->argument_signature = func;
        func->v.func.result = chk_resolve_type(c, node->v.func.return_type);
    }
    c->argument_signature = outer_signature;
    c->receiver_type_expression = outer_receiver;
    return func;
}

// [errors.catch.clause]: a kind is reached through the declaration that introduced it,
// so a type may be a qualified name.
// [modules.env.one] with [syntax.obj.instance]: a name that holds a definition means, as a type, an
// instance of it -- writing the name asks for the whole structure, and that
// is what an instance carries.
static LhatType *as_written_type(LhatType *bound)
{
    LhatType *instance = chk_instance_of(bound);
    return instance != NULL ? instance : bound;
}

// [modules.env.one]: one environment, and a name in it stands for a value, a type, or
// both. Only three things carry a type -- a def^ (its instances, [syntax.obj.instance]), an
// errordef^ and its kinds ([errors.kind.nominal]), and a host type ([modules.hostdata]) -- so a
// name bound to anything else is not one that may be written where a type is.
//
// Without this the lookup answered with whatever the name was worth, which
// made every binding a type meaning "what this value is". 'let^ x = 1' then
// admitted 'y : x' for number^, which [modules.env.one] does not say and nothing relies on.
static bool contains_typeinfo(const LhatType *type)
{
    if (type == NULL) {
        return false;
    }
    if (type->kind == LHAT_TYPE_TABLE) {
        return type->v.table.is_typeinfo;
    }
    if (type->kind == LHAT_TYPE_UNION || type->kind == LHAT_TYPE_INTERSECT) {
        for (const LhatTypeList *arm = type->v.composite.arms; arm != NULL;
             arm = arm->next) {
            if (contains_typeinfo(arm->type)) {
                return true;
            }
        }
    }
    return false;
}

static bool names_a_type(const LhatType *bound)
{
    // A descriptor, including one imported or made optional by '?.', only
    // names a type when its binding explicitly carries the alias target.
    if (contains_typeinfo(bound)) {
        return false;
    }
    if (bound == NULL) {
        return true;  // [syntax.repl.same]'s gap; nothing here says it is wrong
    }
    switch (bound->kind) {
        // A value and nothing else. [syntax.op.semantic] makes identity structural, so a
        // table -- a def^, a host type ([modules.hostdata]), what require^ answers ([lsp.next.fixes]) --
        // has a structure that can be asked for, and is left alone here. What
        // these have is a value's worth and no structure at all.
        case LHAT_TYPE_NUMBER:
        case LHAT_TYPE_STRING:
        case LHAT_TYPE_BOOL:
        case LHAT_TYPE_NIL:
        case LHAT_TYPE_FUNC:
        case LHAT_TYPE_CORO:
        case LHAT_TYPE_COSTATE:
            return false;
        default:
            return true;
    }
}

LhatType *chk_kind_of_set(const LhatType *set, const char *name, size_t length)
{
    if (set == NULL || (set->kind != LHAT_TYPE_ERROR_SET &&
                        set->kind != LHAT_TYPE_ENUM)) {
        return NULL;
    }
    for (const LhatTypeList *k = set->v.error.kinds; k != NULL; k = k->next) {
        if (k->type->v.error.name_length == length &&
            memcmp(k->type->v.error.name, name, length) == 0) {
            return k->type;
        }
    }
    return NULL;
}

// [lsp.hover]: what a written type name turned out to name. [syntax.type] resolves one
// here rather than through [lton.host]'s scoping, so nothing else records it -- and a
// tool that colours a name by the place it stands in cannot tell 'number^'
// from the 'number' somebody meant to write that way. What resolves is a
// type; what does not is a misspelling, and being able to say which is the
// whole use of this.
static void record_type_name(Checker *c, const LhatNode *at, LhatType *type)
{
#if LHAT_WITH_RESOLUTIONS
    if (at != NULL && type != NULL) {
        chk_record_typed_resolution(c, at, type);
    }
#else
    (void)c;
    (void)at;
    (void)type;
#endif
}

// The same two, where the name was found in something that knows where it
// was written -- so a reader can be sent there, which a bare type cannot say.
static void record_type_member(Checker *c, const LhatNode *at, LhatType *type,
                               const LhatTypeMember *member)
{
#if LHAT_WITH_RESOLUTIONS
    if (at != NULL && type != NULL) {
        chk_record_member_resolution(c, at, type, member, false);
    }
#else
    (void)c;
    (void)at;
    (void)type;
    (void)member;
#endif
}

static void record_type_kind(Checker *c, const LhatNode *at, LhatType *kind)
{
#if LHAT_WITH_RESOLUTIONS
    if (at != NULL && kind != NULL) {
        chk_record_kind_resolution(c, at, kind, kind);
    }
#else
    (void)c;
    (void)at;
    (void)kind;
#endif
}

// [syntax.type.return-type]: what X names, for X.ReturnType -- a function: a let^- or
// var^-bound value, a signature alias, a unit's export, or another
// ReturnType (a function answering a function). The name is read where a
// value's is ([modules.env.one]), and the declaration flag or the value's own type
// says what it is: a function value never passes names_a_type, so this is
// where it is looked at directly. NULL where nothing is found.
static LhatType *function_named_by(Checker *c, const LhatNode *node)
{
    const char *name = NULL;
    size_t length = 0;
    if (node->kind == LHAT_NODE_MEMBER) {
        if (!chk_node_name(c, node->v.access.argument, &name, &length)) {
            return NULL;
        }
        if (chk_name_is(name, length, "ReturnType")) {
            return chk_resolve_type(c, node);
        }
        LhatType *outer = chk_resolve_type(c, node->v.access.target);
        if (outer == NULL || (outer->kind != LHAT_TYPE_TABLE &&
                              outer->kind != LHAT_TYPE_HOSTVALUE)) {
            return NULL;
        }
        const LhatTypeMember *member = chk_find_member(outer, name, length);
        if (member == NULL) {
            return NULL;
        }
        LhatType *named = member->names_type && member->named_type != NULL
                              ? member->named_type
                              : member->type;
        record_type_member(c, node->v.access.argument, named, member);
        return named;
    }
    if (!chk_node_name(c, node, &name, &length)) {
        return NULL;
    }
    Scope *found_in = NULL;
    Binding *declared = chk_scope_find(c->scope, name, length, &found_in);
    if (declared == NULL) {
        return NULL;
    }
    LhatType *named = declared->names_type && declared->named_type != NULL
                          ? declared->named_type
                          : declared->type;
    record_type_name(c, node, named);
    return named;
}

// [syntax.type.return-type]: X.ReturnType -- what a call of X answers, as a type. A
// yielding body's is the coroutine ([syntax.sub.no-contagion]); a signature answering no value
// ([syntax.type.empty]) has none, and says so rather than answering nil^.
static LhatType *resolve_return_type(Checker *c, const LhatNode *node)
{
    LhatType *callee = function_named_by(c, node->v.access.target);
    if (callee != NULL && callee->kind == LHAT_TYPE_UNKNOWN) {
        return callee;  // already reported, one step in
    }
    if (callee == NULL || callee->kind != LHAT_TYPE_FUNC) {
        chk_report(c, node, LHAT_CHECK_ERR_UNKNOWN_TYPE);
        return chk_simple(c, LHAT_TYPE_UNKNOWN);
    }
    LhatType *answer = chk_call_answer(c, callee);
    if (answer == NULL || answer->kind == LHAT_TYPE_NONE) {
        chk_report(c, node, LHAT_CHECK_ERR_NO_RESULT_TYPE);
        return chk_simple(c, LHAT_TYPE_UNKNOWN);
    }
    record_type_name(c, node->v.access.argument, answer);
    return answer;
}

// [pipeline.infer.arg-types]: whether a type name is ARGn, which names a parameter of the
// signature being read rather than a value.
static bool names_signature_argument(Checker *c, const LhatNode *node)
{
    const char *name = NULL;
    size_t length = 0;
    if ((node->kind != LHAT_NODE_IDENT && node->kind != LHAT_NODE_TYPE_NAME) ||
        !chk_node_name(c, node, &name, &length) || length <= 3 ||
        memcmp(name, "ARG", 3) != 0) {
        return false;
    }
    for (size_t i = 3; i < length; i++) {
        if (name[i] < '0' || name[i] > '9') return false;
    }
    return true;
}

static LhatType *resolve_qualified_type(Checker *c, const LhatNode *node)
{
    const char *name = NULL;
    size_t length = 0;
    // [syntax.type.return-type]: read before the left side is, since that side names a value
    // rather than a type -- except ARGn, whose .ReturnType is [pipeline.infer.arg-types]'s.
    bool return_type = chk_node_name(c, node->v.access.argument, &name, &length) &&
                       chk_name_is(name, length, "ReturnType");
    if (return_type && !names_signature_argument(c, node->v.access.target)) {
        return resolve_return_type(c, node);
    }
    LhatType *outer = chk_resolve_type(c, node->v.access.target);
    if (outer == NULL || !chk_node_name(c, node->v.access.argument, &name, &length)) {
        chk_report(c, node, LHAT_CHECK_ERR_UNKNOWN_TYPE);
        return chk_simple(c, LHAT_TYPE_UNKNOWN);
    }

    size_t projection = 0;
    bool type_argument = length > 1 && name[0] == 'T';
    for (size_t i = 1; type_argument && i < length; i++) {
        if (name[i] < '0' || name[i] > '9' ||
            projection > (SIZE_MAX - 1 - (size_t)(name[i] - '0')) / 10) {
            type_argument = false;
        } else {
            projection = projection * 10 + (size_t)(name[i] - '0');
        }
    }
    LhatTypeAttribute attribute =
        return_type ? LHAT_TYPE_ATTRIBUTE_RETURN
        : chk_name_is(name, length, "ResultType") ? LHAT_TYPE_ATTRIBUTE_RESULT
        : LHAT_TYPE_ATTRIBUTE_NONE;
    if (outer->kind == LHAT_TYPE_ARGUMENT || outer->kind == LHAT_TYPE_CORO || type_argument ||
        attribute != LHAT_TYPE_ATTRIBUTE_NONE) {
        LhatType *bound = lhat_type_argument_bound(outer);
        LhatType *result = type_argument && outer->kind == LHAT_TYPE_ARGUMENT &&
                          outer->v.argument.index == SIZE_MAX
                              ? chk_simple(c, LHAT_TYPE_ANY)
                              : attribute != LHAT_TYPE_ATTRIBUTE_NONE
                              ? lhat_type_result_attribute(c->result->types, bound, attribute)
                              : type_argument ? lhat_type_argument_attribute(c->result->types, bound, projection) : NULL;
        if (result == NULL || (outer->kind == LHAT_TYPE_ARGUMENT &&
            (outer->v.argument.attribute != LHAT_TYPE_ATTRIBUTE_NONE ||
             outer->v.argument.type_argument != 0))) {
            chk_report(c, node->v.access.argument, LHAT_CHECK_ERR_TYPE_ATTRIBUTE);
            return chk_simple(c, LHAT_TYPE_UNKNOWN);
        }
        if (outer->kind == LHAT_TYPE_ARGUMENT) {
            LhatType *expression = chk_simple(c, LHAT_TYPE_ARGUMENT);
            expression->v.argument = outer->v.argument;
            expression->v.argument.bound = result;
            expression->v.argument.attribute = attribute;
            expression->v.argument.type_argument = type_argument ? projection + 1 : 0;
            result = expression;
        }
        record_type_name(c, node->v.access.argument, result);
        return result;
    }

    if (outer->kind == LHAT_TYPE_ERROR_SET || outer->kind == LHAT_TYPE_ENUM) {
        LhatType *kind = chk_kind_of_set(outer, name, length);
        if (kind != NULL) {
            record_type_kind(c, node->v.access.argument, kind);
            return kind;
        }
        chk_report(c, node, LHAT_CHECK_ERR_UNKNOWN_TYPE);
        return chk_simple(c, LHAT_TYPE_UNKNOWN);
    }

    // [errors.kind.local]: under localerror^ is where the language keeps the kinds it
    // declares for itself. There is no errordef^ to look them up in, so the
    // names are known here -- and only under that top, so error^.CastFailure
    // is as unwritable as any other name nothing declares.
    if (outer->kind == LHAT_TYPE_ERROR && outer->v.error.local) {
        if (chk_name_is(name, length, "CastFailure")) {
            LhatType *kind = chk_cast_failure_type(c);
            record_type_name(c, node->v.access.argument, kind);
            return kind;
        }
        chk_report(c, node, LHAT_CHECK_ERR_UNKNOWN_TYPE);
        return chk_simple(c, LHAT_TYPE_UNKNOWN);
    }

    // [modules.hostvalue]: the boxes the language hangs under a host value type. The
    // names are the type's own -- `std.math.Vector3.Box^` and the get-only
    // `.ConstBox^` -- so they resolve here, off the type the path reached.
    if (outer->kind == LHAT_TYPE_HOSTVALUE) {
        bool sealed = chk_name_is(name, length, "ConstBox^");
        if (sealed || chk_name_is(name, length, "Box^")) {
            LhatType *box =
                lhat_type_hostvalue_box(c->result->types, outer, sealed);
            record_type_name(c, node->v.access.argument, box);
            return box;
        }
    }

    // [modules.deps.static]: what require^ yields is a structure, so reaching a type out
    // of it is the same member access a value uses. [errors.catch.clause] already made a
    // qualified name writable as a type; this is that form over a unit.
    //
    // [modules.hostvalue]: a host value type carries its members on the same list a
    // table does, so a name it declares -- [modules.host.panic]'s enum among them --
    // resolves here exactly as one under [modules.hostdata]'s nominal table does.
    if (outer->kind == LHAT_TYPE_TABLE || outer->kind == LHAT_TYPE_HOSTVALUE) {
        const LhatTypeMember *member = chk_find_member(outer, name, length);
        // [syntax.type.as-expr]: the member names a type by declaration, whatever the
        // value's own shape -- an exported f^ alias would fail the shape
        // reading below.
        if (member != NULL && member->names_type &&
            member->named_type != NULL) {
            record_type_member(c, node->v.access.argument, member->named_type,
                               member);
            return member->named_type;
        }
        // [syntax.juxta.command] again: a unit publishes values as well as types, and only the
        // types among them may be written here.
        if (member != NULL && names_a_type(member->type)) {
            LhatType *written = as_written_type(member->type);
            record_type_member(c, node->v.access.argument, written, member);
            return written;
        }
    }

    chk_report(c, node, LHAT_CHECK_ERR_UNKNOWN_TYPE);
    return chk_simple(c, LHAT_TYPE_UNKNOWN);
}

// [syntax.type.tuple] asks this of whatever stands beside a tuple in a union. The
// principle is not "errors only": it is that the other arm must be told
// apart by the tag in the head slot, with a construct that does the telling.
// An error qualifies (ISERROR, try^/catch^), and so does nil^ (ISNIL and the
// loop's own isDone; [errors.runtime.index]'s absence) -- which is what types a table's
// walk, whose resume answers (K, V)|nil^. Anything else has no construct
// that discriminates it, so '(A, B)|number^' stays unwritable.
bool chk_may_stand_beside_tuple(const LhatType *type)
{
    if (type == NULL) {
        return false;
    }
    switch (type->kind) {
        case LHAT_TYPE_ERROR:
        case LHAT_TYPE_ERROR_SET:
        case LHAT_TYPE_ERROR_KIND:
        case LHAT_TYPE_NIL:
            return true;
        case LHAT_TYPE_UNION:
            for (const LhatTypeList *arm = type->v.composite.arms; arm != NULL;
                 arm = arm->next) {
                if (!chk_may_stand_beside_tuple(arm->type)) {
                    return false;
                }
            }
            return true;
        default:
            return false;
    }
}

// [syntax.type.tuple] with [errors.dropped.around-value]: does an error hide anywhere in this type? A tuple
// position may not carry one -- not directly and not buried in a union --
// because a value beside the good ones is a value a program may drop, which
// is the whole of what [modules.host.initial] kept unwritable.
void chk_check_tuple_position(Checker *c, const LhatNode *at,
                              const LhatType *position)
{
    // [errors.dropped.around-value]: the error goes around the values, never among them -- the
    // same rule resolve_type applies to a written '(A, SomeError)', asked
    // here of an inferred one.
    if (chk_contains_error(position)) {
        chk_report(c, at, LHAT_CHECK_ERR_TUPLE_ERROR_POSITION);
    }
    // A position is a slot, not a run of slots.
    if (position != NULL && position->kind == LHAT_TYPE_TUPLE) {
        chk_report(c, at, LHAT_CHECK_ERR_TUPLE_MISPLACED);
    }
    // [modules.hostvalue]: a host value is as wide as its tag says, and a position is
    // one slot. A frame's answer room carries one of the two, never a
    // mixture.
    // A registered host signature is exempt: it only describes what the
    // host hands over, and the refusal lands where L^ takes the tuple
    // apart (check_focus and the other destructure sites) -- a failed
    // registration would surface as a missing member, telling the reader
    // nothing.
    if (!c->hosted_signature && chk_is_hostvalue(position)) {
        chk_report(c, at, LHAT_CHECK_ERR_HOSTVALUE_ESCAPES);
    }
}

bool chk_contains_error(const LhatType *type)
{
    if (type == NULL) {
        return false;
    }
    switch (type->kind) {
        case LHAT_TYPE_ERROR:
        case LHAT_TYPE_ERROR_SET:
        case LHAT_TYPE_ERROR_KIND:
            return true;
        case LHAT_TYPE_UNION:
        case LHAT_TYPE_INTERSECT:
            for (const LhatTypeList *arm = type->v.composite.arms; arm != NULL;
                 arm = arm->next) {
                if (chk_contains_error(arm->type)) {
                    return true;
                }
            }
            return false;
        default:
            // Deliberately shallow otherwise. An error reachable through a
            // table member or a signature's result is a value that has to be
            // asked for, not one sitting beside the answer.
            return false;
    }
}

// [syntax.type.tuple]: does a tuple hide in this type -- as itself or as a union arm? A
// name may bind neither: the union case is a walk's '(K, V)|nil^', which
// would make the name a maybe-run.
bool chk_contains_tuple(const LhatType *type)
{
    if (type == NULL) {
        return false;
    }
    if (type->kind == LHAT_TYPE_TUPLE) {
        return true;
    }
    if (type->kind == LHAT_TYPE_UNION) {
        for (const LhatTypeList *arm = type->v.composite.arms; arm != NULL;
             arm = arm->next) {
            if (chk_contains_tuple(arm->type)) {
                return true;
            }
        }
    }
    return false;
}

// The written type, resolved. chk_resolve_type below is what everything
// calls; this is its body, kept apart so the wrapper can leave what it found
// on the node.
static LhatType *resolve_written_type(Checker *c, const LhatNode *node)
{
    // [syntax.type.tuple]: the permission is for this one type, not for what it contains.
    // Taking it here means every nested resolve below starts out refusing a
    // tuple, and the few places that may hold one -- a result, a coroutine's
    // Y and T, a union's arms -- hand it back deliberately. Cleared before
    // the NULL test so an absent type consumes it too: an unwritten result
    // must not leave the permission standing for whatever resolves next.
    bool tuple_allowed = c->tuple_allowed;
    c->tuple_allowed = false;

    if (node == NULL) {
        return NULL;
    }

    switch (node->kind) {
        // An IDENT reaches here from the qualified name of a construction
        // ([errors.kind.new]), which is written in expression position but names a
        // type.
        case LHAT_NODE_IDENT:
        case LHAT_NODE_TYPE_NAME: {
            const char *name = NULL;
            size_t length = 0;
            if (!chk_node_name(c, node, &name, &length)) {
                return chk_simple(c, LHAT_TYPE_UNKNOWN);
            }
            if (length > 3 && memcmp(name, "ARG", 3) == 0) {
                size_t index = 0;
                bool digits = true;
                for (size_t i = 3; i < length; i++) {
                    if (name[i] < '0' || name[i] > '9') { digits = false; break; }
                    if (index > (SIZE_MAX - 9) / 10) { index = SIZE_MAX; break; }
                    index = index * 10 + (size_t)(name[i] - '0');
                }
                if (digits) {
                    const LhatTypeList *param = c->argument_signature != NULL
                                                   ? c->argument_signature->v.func.params : NULL;
                    size_t position = index;
                    while (param != NULL && position > 0) { param = param->next; position--; }
                    if (param == NULL) {
                        chk_report(c, node, LHAT_CHECK_ERR_TYPE_ARGUMENT_REFERENCE);
                        return chk_simple(c, LHAT_TYPE_UNKNOWN);
                    }
                    LhatType *reference = chk_simple(c, LHAT_TYPE_ARGUMENT);
                    reference->v.argument.index = index;
                    reference->v.argument.bound = param->type;
                    record_type_name(c, node, reference);
                    return reference;
                }
            }
            if (chk_name_is(name, length, "c^")) {
                LhatType *any = chk_simple(c, LHAT_TYPE_ANY);
                LhatType *top = lhat_type_coro(c->result->types, NULL, any, any, false, false);
                top->coroutine_top = true;
                record_type_name(c, node, top);
                return top;
            }
            LhatType *builtin = builtin_type(c, name, length);
            if (builtin != NULL) {
                record_type_name(c, node, builtin);
                return builtin;
            }

            // [syntax.type.self]: the type literal this is written inside. Read
            // before the environment, since [modules.env.one] has no binding for it --
            // a name for a type and nothing else has no value to stand for,
            // and putting one in scope would make it answerable to [modules.host.import]'s
            // ordering and to every other rule about names.
            //
            // [syntax.obj.name] already lets a definition say its own type by the name its
            // binding gave it. A literal written where no name is taken has
            // only this: a t^{ … } holding one of
            // itself, or a def^ with no binding, could not be written at all.
            if (chk_name_is(name, length, "Self^")) {
                // [lex.hat.repeat]: one hat is the word, and the rest count
                // outwards -- Self^^ is the literal one further out.
                uint32_t levels = node->v.name.hats > 0 ? node->v.name.hats : 1;
                const struct SelfLink *link = c->self_link;
                for (uint32_t i = 1; link != NULL && i < levels; i++) {
                    link = link->outer;
                }
                if (link == NULL) {
                    chk_report(c, node, LHAT_CHECK_ERR_SELF_TYPE_OUTSIDE);
                    return chk_simple(c, LHAT_TYPE_UNKNOWN);
                }
                record_type_name(c, node, link->type);
                return link->type;
            }

            // [syntax.type] has no such type as self^ or def^. They are the
            // receiver and the definition being built ([syntax.obj.self], [pipeline.bc.object]),
            // and both are values -- 'p^self^;' writes a receiver into a
            // signature, which is resolve_func_type's business, not a name
            // standing for a type.
            //
            // def^ needs saying separately: it is bound to the def^ under
            // construction, so names_a_type below would let it through. What
            // that answers with is a type holding itself, and the relations
            // walk one of those until the stack is gone -- a crash with no
            // diagnostic at all.
            if (chk_name_is(name, length, "type^")) return chk_typeinfo_type(c);
            if (chk_name_is(name, length, "self^") && c->receiver_type_expression) {
                LhatType *receiver = chk_simple(c, LHAT_TYPE_ARGUMENT);
                receiver->v.argument.index = SIZE_MAX;
                receiver->v.argument.bound = chk_simple(c, LHAT_TYPE_ANY);
                return receiver;
            }
            if (chk_name_is(name, length, "self^") ||
                chk_name_is(name, length, "def^")) {
                chk_report(c, node, LHAT_CHECK_ERR_UNKNOWN_TYPE);
                return chk_simple(c, LHAT_TYPE_UNKNOWN);
            }

            // [syntax.obj.structure]: the members come with the word, so a t^ standing alone
            // is not one of these names but a t^{ … } with its list left
            // off. What the top of tables asks for is nothing, which is
            // 't^{}' -- said here rather than left to "no such type", since
            // the word is right and only the braces are missing.
            if (chk_name_is(name, length, "t^")) {
                // [lsp.next]: and so the fix is the braces, written where the
                // word ends. Machine-appliable: the sentence above says
                // there is one thing this could have been.
                const LhatFixSlot braces = lhat_fix_slot(
                    LHAT_FIX_TABLE_MEMBERS, LHAT_FIX_MACHINE, node->end, 0,
                    "{}");
                chk_report_fix(c, node, LHAT_CHECK_ERR_BARE_TABLE_TYPE, NULL,
                               0, &braces, 1);
                return lhat_type_table(c->result->types);
            }

            // [modules.env.one]: one environment. A name written as a type is looked
            // up in the same place a value is, which is what [syntax.obj.name] needs --
            // it says a definition takes its name from its binding, and a
            // binding lives here.
            Scope *found_in = NULL;
            Binding *declared =
                chk_scope_find(c->scope, name, length, &found_in);

            // [syntax.obj.name]: written inside the very def^ this name is being bound to,
            // where the binding still holds the collecting pass's pending^
            // seed. What it will mean is already built, so the name says the
            // same thing here as it does anywhere else -- [syntax.obj.instance]'s instance,
            // which is what Self^ answers with too. Asked before the old-world
            // skip below: a def^'s own name is the one self-reference [syntax.obj.name]
            // grants, and this is what grants it.
            for (const struct DefLink *d = c->def_link; d != NULL;
                 d = d->outer) {
                if (declared != NULL && d->binding == declared &&
                    d->instance != NULL) {
                    record_type_name(c, node, d->instance);
                    return d->instance;
                }
            }

            // [syntax.stmt.old-world]: its own let^'s right side is the old world -- inside
            // the spelling of a type-as-value ([syntax.type.as-expr]), the name still means
            // what it meant outside, so an alias may not name itself. Only
            // there: any other type position keeps reading the collecting
            // pass's seed, which is what lets a signature deeper in a value
            // name the binding being made (the case beside [syntax.obj.name]'s).
            if (declared != NULL && declared->being_defined &&
                c->in_type_value && found_in != NULL) {
                declared = chk_scope_find(found_in->parent, name, length,
                                          NULL);
            }
            if (declared == NULL) {
                chk_report(c, node, LHAT_CHECK_ERR_UNKNOWN_TYPE);
                return chk_simple(c, LHAT_TYPE_UNKNOWN);
            }

            // [syntax.type.as-expr]: a name a let^ bound to a type^ value stands for
            // the type it spelled, whatever shape the value's own type has
            // -- an f^ alias would fail the shape reading below.
            if (declared->names_type && declared->named_type != NULL) {
#if LHAT_WITH_RESOLUTIONS
                chk_record_narrowed_resolution(c, node, declared,
                                               declared->named_type);
#else
                record_type_name(c, node, declared->named_type);
#endif
                return declared->named_type;
            }

            // [modules.env.one] gives a type to a def^ and an errordef^, and to nothing
            // else. Being in scope is not enough to be written here.
            if (declared->is_parameter || !names_a_type(declared->type)) {
                chk_report(c, node, LHAT_CHECK_ERR_UNKNOWN_TYPE);
                return chk_simple(c, LHAT_TYPE_UNKNOWN);
            }

            LhatType *written = as_written_type(declared->type);
#if LHAT_WITH_RESOLUTIONS
            // [syntax.obj.name]: what this names was bound by a let^ in some scope, and
            // that is a place to point at -- so the record carries the
            // binding rather than the type alone, and 'x : Reader' answers
            // where the Reader in an expression does.
            chk_record_narrowed_resolution(c, node, declared, written);
#else
            record_type_name(c, node, written);
#endif
            return written;
        }

        case LHAT_NODE_MEMBER:
            return resolve_qualified_type(c, node);

        case LHAT_NODE_TYPE_APPLY: {
            LhatType *base = chk_resolve_type(c, node->v.access.target);
            const LhatCheckType *args[LHAT_CHECK_MAX_TRACKED_ARGS];
            size_t count = 0;
            for (const LhatNode *arg = node->v.access.argument; arg; arg = arg->next) {
                // Type arguments describe types; they are not stored tuple values.
                c->tuple_allowed = true;
                LhatType *resolved = chk_resolve_type(c, arg);
                if (count < LHAT_CHECK_MAX_TRACKED_ARGS) args[count++] = resolved;
                else {
                    chk_report(c, node, LHAT_CHECK_ERR_TYPE_SPECIALIZATION);
                    return chk_simple(c, LHAT_TYPE_UNKNOWN);
                }
            }
            LhatInstantiationContext context = { c->result->types, c->require.hosted, 0 };
            if (base != NULL && base->kind == LHAT_TYPE_TABLE && base->v.table.is_typeinfo && count != 1) {
                chk_report(c, node, LHAT_CHECK_ERR_TYPE_SPECIALIZATION);
                return chk_simple(c, LHAT_TYPE_UNKNOWN);
            }
            LhatType *made = (LhatType *)lhat_check_type_specialize(&context, base, args, count);
            if (made == NULL) chk_report(c, node, LHAT_CHECK_ERR_TYPE_SPECIALIZATION);
            return made != NULL ? made : chk_simple(c, LHAT_TYPE_UNKNOWN);
        }

        case LHAT_NODE_TYPE_TABLE: {
            // A structural declaration introduces its own member scope. The
            // initial dependent-result grammar does not substitute its fields.
            LhatType *outer = c->argument_signature;
            bool outer_receiver = c->receiver_type_expression;
            c->argument_signature = NULL;
            c->receiver_type_expression = false;
            LhatType *table = resolve_table_type(c, node);
            c->argument_signature = outer;
            c->receiver_type_expression = outer_receiver;
            return table;
        }

        case LHAT_NODE_TYPE_FUNC:
            return chk_resolve_func_type(c, node);

        case LHAT_NODE_TYPE_TUPLE: {
            // [syntax.type.tuple]: two positions or more, in order. The parser only builds
            // one from a parenthesised list with a ',' in it, so there is no
            // one-position form to reject here.
            if (!tuple_allowed) {
                chk_report(c, node, LHAT_CHECK_ERR_TUPLE_MISPLACED);
            }
            LhatType *tuple = lhat_type_tuple(c->result->types);
            for (const LhatNode *item = node->v.list.items; item != NULL;
                 item = item->next) {
                // c->tuple_allowed is already false, so a tuple written as a
                // position lands on the refusal above -- [syntax.type.tuple] does not nest
                // them, since a position is a slot and not a run of slots.
                LhatType *position = chk_resolve_type(c, item);
                // The same three a position has to satisfy wherever it comes
                // from, so a registered '-> (number^, SomeHostValue)' does not
                // get through -- [modules.hostvalue] has no slot to spare for one.
                chk_check_tuple_position(c, item, position);
                lhat_type_add_position(c->result->types, tuple, position);
            }
            return tuple;
        }

        case LHAT_NODE_TYPE_UNION: {
            // [syntax.type.tuple]: a union is transparent to the permission. '(A, B)|E' is
            // written where a result is, and both arms are that result.
            c->tuple_allowed = tuple_allowed;
            LhatType *left = chk_resolve_type(c, node->v.binary.left);
            c->tuple_allowed = tuple_allowed;
            LhatType *right = chk_resolve_type(c, node->v.binary.right);
            // [errors.fallible.union]: what stands beside a tuple has to be discriminable
            // -- an error or nil^. This keeps [errors.dropped.around-value] all the same: an
            // error among the values rather than around them is the shape
            // 'v, err := f()' needs, and [errors.dropped.around-value] leaves it unwritable.
            if ((lhat_type_tuple_width(left) > 0 &&
                 !chk_may_stand_beside_tuple(right)) ||
                (lhat_type_tuple_width(right) > 0 &&
                 !chk_may_stand_beside_tuple(left))) {
                chk_report(c, node, LHAT_CHECK_ERR_TUPLE_UNION);
            }
            // [modules.hostvalue.union]: a host value is wide the same way, and stands in a
            // union under the same condition -- the head slot says which arm
            // turned up, so the others have to be the ones it tells apart.
            // This is also what keeps one out of an any^: the union of the
            // two would be any^ alone, and the host value would have escaped
            // by being written rather than by being put anywhere.
            if ((chk_is_hostvalue(left) && !chk_may_stand_beside_tuple(right)) ||
                (chk_is_hostvalue(right) && !chk_may_stand_beside_tuple(left))) {
                chk_report(c, node, LHAT_CHECK_ERR_HOSTVALUE_UNION);
            }
            return lhat_type_union(c->result->types, left, right);
        }

        case LHAT_NODE_TYPE_INTERSECT:
            return lhat_type_intersect(c->result->types,
                                       chk_resolve_type(c, node->v.binary.left),
                                       chk_resolve_type(c, node->v.binary.right));

        case LHAT_NODE_TYPE_CORO: {
            // [syntax.type.coroutine] with [syntax.sub.yieldable-f]: 'c^{ f^R -> Y -> T }'. An omitted R is left
            // empty rather than filled: [syntax.type.empty] makes the empty argument side a
            // statement of its own, and here it says nothing is sent in -- so
            // a resume of one of these takes no argument. Y is different; a
            // yield^ with no value really does hand nil^ to the resumer, so
            // nothing written there is nil^.
            //
            // [syntax.type.tuple]: R may be a tuple -- several parameters written in the
            // signature, sent by one resume(a, b) and taken apart by the
            // yield^'s own binding. The positions are checked by the TUPLE
            // arm above, host values included.
            LhatType *receive = NULL;
            if (node->v.coroutine.receive != NULL) {
                c->tuple_allowed = true;
                receive = chk_resolve_type(c, node->v.coroutine.receive);
            }
            // [syntax.type.tuple]: Y and T are results -- what the body yields and what it
            // finally answers -- so a tuple may be written in either.
            LhatType *produce = chk_simple(c, LHAT_TYPE_NIL);
            if (node->v.coroutine.produce != NULL) {
                c->tuple_allowed = true;
                produce = chk_resolve_type(c, node->v.coroutine.produce);
            }
            c->tuple_allowed = true;
            // Left empty, the body ends without a value and [syntax.sub.coroutine-members]'s nil^
            // joins Y|T where the resume reads it. Written '-', it cannot end
            // and nothing joins. The parser tells the two apart.
            LhatType *result = node->v.coroutine.result != NULL
                                   ? chk_resolve_type(c, node->v.coroutine.result)
                                   : NULL;
            // [modules.hostvalue]: a host value may be written in any of the three
            // positions -- the type describes an interface, and a host walk
            // hands one over whole (step_host_walk). What cannot carry one
            // refuses at its own site: an L^ body's yield^ (chk_unify_yield),
            // a tuple position (chk_check_tuple_position), a destructure.
            LhatType *type = lhat_type_coro(c->result->types, receive, produce, result,
                                  node->v.coroutine.endless,
                                  node->v.coroutine.is_function);
            type->receive_any = node->v.coroutine.receive_any;
            type->produce_any = node->v.coroutine.produce_any;
            type->result_any = node->v.coroutine.result_any;
            type->kind_any = node->v.coroutine.kind_any;
            type->coroutine_top = type->receive_any && type->produce_any &&
                                  type->result_any && type->kind_any;
            return type;
        }

        default:
            chk_report(c, node, LHAT_CHECK_ERR_UNKNOWN_TYPE);
            return chk_simple(c, LHAT_TYPE_UNKNOWN);
    }
}

LhatType *chk_resolve_type(Checker *c, const LhatNode *node)
{
    LhatType *resolved = resolve_written_type(c, node);

    // [modules.hostvalue] with 08: what the compiler does with a written type it has to
    // settle itself -- fits^, as^, an overload^ed parameter -- is match the
    // words against what the host registered (compile.c's lower_type). That
    // cannot see through a name bound to a type or to a module:
    //
    //     let^ vector3 = import^ std.math.vector3
    //     let^ Vector3 = vector3.Vector3
    //     v fits^ Vector3          -- the words are not the registered ones
    //
    // The name was resolved properly right here, so what it came to is left
    // on the node and lower_type reads it where the words run out. Only the
    // two kinds it gives up on: an IDENT reaching here is the qualified name
    // of a construction ([errors.kind.new]), written in expression position, and
    // nothing of an expression's own stamp should be written over.
    if (node != NULL && resolved != NULL && node->kind != LHAT_NODE_IDENT &&
        node->kind != LHAT_NODE_HAT_IDENT) {
        ((LhatNode *)node)->checked_type = resolved;
    }
    return resolved;
}

// ---------------------------------------------------------------------------
// Parameter types ([pipeline.infer])
// ---------------------------------------------------------------------------

// A parameter still being decided, found by the pending^ object standing for
// it. [syntax.type.fits]'s narrowing is what keeps a narrowed use from arriving here: infer
// answers a narrowed path with the narrowed type, which is a different object,
// so the demand a branch makes of the narrowed value never reaches the
// parameter itself.
ParamVar *chk_param_var_for(Checker *c, const LhatType *type)
{
    if (type == NULL || type->kind != LHAT_TYPE_PENDING) {
        return NULL;
    }
    for (ParamVar *pv = c->param_vars; pv != NULL; pv = pv->next) {
        if (pv->slot == type) {
            return pv;
        }
    }
    return NULL;
}

// [syntax.sub.yieldable-f]: whether an f^ coroutine is reachable anywhere in this type. The
// result of an f^ is read with this, since one reaches the outside through a
// member or a nested signature as readily as by being the result itself.
//
// A HOST TYPE IS AN ATOM, for the reason chk_type_touches_local gives above
// and one more. [modules.hostdata]'s members are signatures the host registered in C,
// settled before any body ran; a coroutine THIS body made is a value, and no
// value becomes part of a registered declaration. So a c^ found in there was
// the host's own (an iterate^, say) and never an escape -- walking in can
// only answer wrongly.
//
// It is also the whole cost. A host type's member is a signature whose result
// is another host type, whose members are signatures again, and the bound
// below is a bound on depth, not on work: an engine's API re-enters the same
// few hundred types along every path that reaches them. An f^ answering
// godot.Node took 94 ms of this, and one answering godot.Node3D under a
// millisecond -- the difference being only how soon a path happened to find a
// coroutine and stop, which is no difference a writer could have predicted.
bool chk_mentions_function_coroutine(const LhatType *type, unsigned depth)
{
    if (type == NULL || depth > 8) {
        return false;
    }
    switch (type->kind) {
        case LHAT_TYPE_CORO:
            return type->v.coroutine.is_function;

        case LHAT_TYPE_TABLE:
            if (type->v.table.nominal) {
                return false;
            }
            // A def^'s instance is walked: [syntax.obj.def] lets a field be written c^,
            // and a body may put its own coroutine in one and answer that.
            for (const LhatTypeMember *m = type->v.table.members; m != NULL;
                 m = m->next) {
                if (chk_mentions_function_coroutine(m->type, depth + 1)) {
                    return true;
                }
            }
            return chk_mentions_function_coroutine(type->v.table.variadic,
                                                   depth + 1) ||
                   chk_mentions_function_coroutine(type->v.table.index_key,
                                                   depth + 1) ||
                   chk_mentions_function_coroutine(type->v.table.index_value,
                                                   depth + 1);

        case LHAT_TYPE_FUNC: {
            for (const LhatTypeList *p = type->v.func.params; p != NULL;
                 p = p->next) {
                if (chk_mentions_function_coroutine(p->type, depth + 1)) {
                    return true;
                }
            }
            // [syntax.sub.yieldable-f]: a yielding signature makes a fresh coroutine at
            // each call, which is no coroutine leaving; what its body returns is.
            const LhatType *answer = type->v.func.result;
            if (type->v.func.yields && answer != NULL && answer->kind == LHAT_TYPE_CORO) {
                answer = answer->v.coroutine.result;
            }
            return chk_mentions_function_coroutine(answer, depth + 1) ||
                   chk_mentions_function_coroutine(type->v.func.variadic, depth + 1);
        }

        case LHAT_TYPE_UNION:
        case LHAT_TYPE_INTERSECT:
            for (const LhatTypeList *arm = type->v.composite.arms; arm != NULL;
                 arm = arm->next) {
                if (chk_mentions_function_coroutine(arm->type, depth + 1)) {
                    return true;
                }
            }
            return false;

        default:
            return false;
    }
}

// Whether a demand would contain the very variable it is about, which would
// leave the settled type pointing at itself.
//
// A HOST TYPE IS AN ATOM, as it is for the walk above and for one reason more
// exact: `slot` is a node THIS check made for a name it has not settled yet,
// and [modules.host.import] has every registration resolved before the first check runs.
// A type the registrations built cannot hold a node that did not exist when
// they ran, so the answer under one is always no -- and it is the whole cost
// of asking, since an engine's API re-enters its own few hundred types along
// every path down to the bound. Inferring one parameter of a def^ that wraps
// godot.Node asked this 4.9 million times.
static bool demand_mentions(const LhatType *type, const LhatType *slot,
                            unsigned depth)
{
    if (type == NULL || depth > 8) {
        return false;
    }
    if (type == slot) {
        return true;
    }
    switch (type->kind) {
        case LHAT_TYPE_TABLE:
            if (type->v.table.nominal) {
                return false;
            }
            for (const LhatTypeMember *m = type->v.table.members; m != NULL;
                 m = m->next) {
                if (demand_mentions(m->type, slot, depth + 1)) {
                    return true;
                }
            }
            return demand_mentions(type->v.table.variadic, slot, depth + 1) ||
                   demand_mentions(type->v.table.index_key, slot, depth + 1) ||
                   demand_mentions(type->v.table.index_value, slot, depth + 1);

        case LHAT_TYPE_FUNC:
            for (const LhatTypeList *p = type->v.func.params; p != NULL;
                 p = p->next) {
                if (demand_mentions(p->type, slot, depth + 1)) {
                    return true;
                }
            }
            return demand_mentions(type->v.func.result, slot, depth + 1) ||
                   demand_mentions(type->v.func.variadic, slot, depth + 1);

        case LHAT_TYPE_UNION:
        case LHAT_TYPE_INTERSECT:
            for (const LhatTypeList *arm = type->v.composite.arms; arm != NULL;
                 arm = arm->next) {
                if (demand_mentions(arm->type, slot, depth + 1)) {
                    return true;
                }
            }
            return false;

        default:
            return false;
    }
}

// Two demands held at once. [syntax.obj.compose]'s intersection is the general answer, but two
// structural demands describe one table rather than two: [syntax.obj.structure] reads 't^{ … }'
// as "at least these", so asking for 'a' and asking for 'b' is asking for a
// table carrying both, and writing that out is what the writer would have
// written by hand.
static LhatType *merge_demand(Checker *c, LhatType *a, LhatType *b)
{
    if (a == NULL) {
        return b;
    }
    if (b == NULL || lhat_type_equal(a, b)) {
        return a;
    }
    if (a->kind != LHAT_TYPE_TABLE || b->kind != LHAT_TYPE_TABLE ||
        a->v.table.is_definition || b->v.table.is_definition ||
        a->v.table.nominal || b->v.table.nominal ||
        a->v.table.index_key != NULL || b->v.table.index_key != NULL) {
        return lhat_type_intersect(c->result->types, a, b);
    }

    LhatType *merged = lhat_type_table(c->result->types);
    if (merged == NULL) {
        return a;
    }
    for (const LhatTypeMember *m = a->v.table.members; m != NULL; m = m->next) {
        lhat_type_add_member(c->result->types, merged, m->name, m->name_length,
                             m->type);
    }
    for (const LhatTypeMember *m = b->v.table.members; m != NULL; m = m->next) {
        LhatTypeMember *into =
            (LhatTypeMember *)chk_find_member(merged, m->name, m->name_length);
        if (into != NULL) {
            into->type = merge_demand(c, into->type, m->type);
        } else {
            lhat_type_add_member(c->result->types, merged, m->name,
                                 m->name_length, m->type);
        }
    }
    merged->v.table.variadic =
        merge_demand(c, a->v.table.variadic, b->v.table.variadic);
    return merged;
}

// [pipeline.infer]: `value` was used somewhere that wants `wanted`. When the value is
// a parameter whose type is still being decided, that is a demand on it rather
// than something to report.
void chk_constrain(Checker *c, LhatType *value, LhatType *wanted)
{
    ParamVar *pv = chk_param_var_for(c, value);
    if (pv == NULL || pv->closed || wanted == NULL) {
        return;
    }
    // A position that would take anything says nothing about what arrives.
    switch (wanted->kind) {
        case LHAT_TYPE_UNKNOWN:
        case LHAT_TYPE_PENDING:
        case LHAT_TYPE_ANY:
        case LHAT_TYPE_NONE:
            return;
        default:
            break;
    }
    if (demand_mentions(wanted, pv->slot, 0)) {
        pv->closed = true;
        return;
    }

    // [pipeline.infer]: only what always runs decides. What a branch asks is added when it
    // agrees with that, and dropped when it does not -- the branch reports for
    // itself, and intersecting the two would leave a type no value inhabits.
    if (c->conditional > 0) {
        if (pv->conditional_dropped) {
            return;
        }
        if (pv->conditional == NULL) {
            pv->conditional = wanted;
        } else if (lhat_type_disjoint(pv->conditional, wanted)) {
            pv->conditional = NULL;
            pv->conditional_dropped = true;
        } else {
            pv->conditional = merge_demand(c, pv->conditional, wanted);
        }
        return;
    }

    if (pv->demanded == NULL) {
        pv->demanded = wanted;
    } else if (lhat_type_disjoint(pv->demanded, wanted)) {
        pv->closed = true;  // [syntax.repl.same]: undecided rather than reported
    } else {
        pv->demanded = merge_demand(c, pv->demanded, wanted);
    }
}

// [syntax.repl.same]: what arrives is no longer what the name holds, so nothing after this
// says anything about the parameter.
void chk_close_param_var(Checker *c, const LhatType *value)
{
    ParamVar *pv = chk_param_var_for(c, value);
    if (pv != NULL) {
        pv->closed = true;
    }
}

// [syntax.repl.same]: reading a member off a parameter demands a structure carrying it. The
// member's own type is left unknown^ -- what may be done with it is decided by
// what the member turns out to be, and a call through it says too little to
// pin a signature down ([syntax.type.signature] asks the f^/p^ distinction and [syntax.type.empty] the presence
// of a result, and neither is readable off one call).
void chk_constrain_member(Checker *c, LhatType *target, const char *name,
                          size_t length)
{
    if (chk_param_var_for(c, target) == NULL) {
        return;
    }
    LhatType *shape = lhat_type_table(c->result->types);
    if (shape == NULL ||
        lhat_type_add_member(c->result->types, shape, name, length,
                             chk_simple(c, LHAT_TYPE_UNKNOWN)) == NULL) {
        return;
    }
    chk_constrain(c, target, shape);
}

// [pipeline.infer.call-shapes]: the call shapes. One flat list for the whole unit -- a
// round rolls back what a walk said, and these are what it learned, so they
// live outside the rounds the way a binding's seed does.
Instantiation *chk_instantiation_add(Checker *c, const LhatNode *func,
                                     LhatType *const *args, size_t count)
{
    bool seen = false;
    for (Instantiation *i = c->instantiations; i != NULL; i = i->next) {
        if (i->func == func) seen = true;
        if (i->func != func || i->count != count) {
            continue;
        }
        bool same = true;
        for (size_t k = 0; k < count && same; k++) {
            same = lhat_type_equal(i->args[k], args[k]);
        }
        if (same) {
            return i;
        }
    }
    Instantiation *made = lhat_type_semantic_alloc(c->result->types, sizeof *made);
    if (made == NULL) {
        return NULL;
    }
    made->concrete = lhat_type_function_instance(c->result->types);
    if (made->concrete == NULL ||
        (made->concrete->body = lhat_type_clone_body(c->result->types, func)) == NULL) {
        return NULL;
    }
    if (!seen) ((LhatNode *)func)->checked_instances = NULL;
    LhatFunctionInstance **tail = &((LhatNode *)func)->checked_instances;
    size_t arm = 1;
    while (*tail != NULL) {
        tail = &(*tail)->next;
        arm++;
    }
    if (arm > UINT16_MAX) {
        return NULL;
    }
    *tail = made->concrete;
    made->arm = (uint16_t)arm;
    made->func = func;
    made->count = count;
    for (size_t k = 0; k < count; k++) {
        made->args[k] = args[k];
    }
    made->next = c->instantiations;
    c->instantiations = made;
    return made;
}

bool chk_instantiations_exist(const Checker *c, const LhatNode *func)
{
    for (const Instantiation *i = c->instantiations; i != NULL; i = i->next) {
        if (i->func == func) {
            return true;
        }
    }
    return false;
}

void chk_detach_instantiations(Checker *c)
{
    // Records and concrete trees live as long as their semantic arena.
    c->instantiations = NULL;
}

ParamVar *chk_push_param_var(Checker *c, LhatType *slot, const LhatNode *node)
{
    ParamVar *pv = (ParamVar *)lhat_calloc(1, sizeof *pv);
    if (pv == NULL) {
        return NULL;
    }
    pv->slot = slot;
    pv->node = node;
    pv->next = c->param_vars;
    c->param_vars = pv;
    return pv;
}

// The body has been read, so the demands are all in. Written through the slot
// rather than returned: the binding and the signature hold that same pointer,
// which is what let a single walk decide both.
void chk_settle_param_vars(Checker *c, ParamVar *mark)
{
    while (c->param_vars != mark) {
        ParamVar *pv = c->param_vars;
        c->param_vars = pv->next;

        LhatType *settled = NULL;
        if (!pv->closed) {
            settled = pv->demanded;
            if (settled == NULL) {
                settled = pv->conditional_dropped ? NULL : pv->conditional;
            } else if (pv->conditional != NULL &&
                       !lhat_type_disjoint(settled, pv->conditional)) {
                settled = merge_demand(c, settled, pv->conditional);
            }
        }
        // [pipeline.infer]: nothing was demanded, or the demands did not agree. Inference
        // did not decide, so the slot is left as what it already is -- a
        // constraint nobody wrote, which [pipeline.strict.per-unit] reports under strict and
        // relaxed forgives ([pipeline.strict.relaxed]: no check is inserted; a real mismatch
        // meets the machine's own instruction check where it lands). Not
        // any^: [pipeline.strict.relaxed] makes that the reading under which relaxed would be the
        // stricter setting.
        if (settled != NULL && settled != pv->slot) {
            *pv->slot = *settled;
        } else if (settled == NULL && pv->node != NULL) {
            // [pipeline.strict.per-unit]③: strict leaves nothing undecided in a unit. There is no
            // demand to read this off, and no annotation either, so the
            // signature this parameter is part of has a hole in it that every
            // caller would be handed. [syntax.type.variadic]'s any^ is how a writer says a
            // position really does take anything.
            const LhatNode *name_node = pv->node->kind == LHAT_NODE_PARAM
                ? pv->node->v.param.name : pv->node;
            const char *name = NULL;
            size_t length = 0;
            chk_node_name(c, name_node, &name, &length);
            chk_report_named(c, pv->node, LHAT_CHECK_ERR_PARAM_UNDECIDED, name, length);
        }
        lhat_free(pv);
    }
}

// ---------------------------------------------------------------------------
// [pipeline.infer.rounds]: walking the same ground again
// ---------------------------------------------------------------------------

// Opens the loop. `count` is how many elements are being walked, which is what
// bounds the number of walks.
void chk_rounds_begin(Checker *c, Rounds *r, size_t count)
{
    r->diagnostics = c->result->diagnostic_count;
#if LHAT_WITH_RESOLUTIONS
    r->resolutions = c->result->resolution_count;
    r->member_sites = c->result->member_site_count;
    r->try_routes = c->result->try_route_count;
    r->binding_sites = c->result->binding_site_count;
#endif
    r->round = 0;
    r->instantiations = c->instantiations;
    r->cap = count + 1;
    r->changed = false;
    r->read_provisional_outside = c->read_provisional;
    c->read_provisional = false;
}

// Answers whether to walk again, and rolls the diagnostics back when it does.
// The caller has just finished a walk and has said, through `r->changed`,
// whether any answer moved.
bool chk_rounds_next(Checker *c, Rounds *r)
{
    // Nothing was read ahead, so another walk reads the same things and says
    // the same things -- or this walk answered exactly what the last one did,
    // which is the fixpoint. New call shapes are additional semantic work:
    // their concrete bodies still need checking even when types stay equal.
    bool new_shapes = c->instantiations != r->instantiations;
    if (!c->read_provisional || (!r->changed && !new_shapes) ||
        r->round + 1 >= r->cap) {
        return false;
    }
    r->round++;
    r->instantiations = c->instantiations;
    r->changed = false;
    c->read_provisional = false;
    c->result->diagnostic_count = r->diagnostics;
#if LHAT_WITH_RESOLUTIONS
    c->result->resolution_count = r->resolutions;
    // Rolled back with the rest: a site left by a walk that was thrown
    // away carries the receiver that walk guessed at, and qsort being
    // unstable the settle could keep either of the two.
    c->result->member_site_count = r->member_sites;
    c->result->try_route_count = r->try_routes;
    c->result->binding_site_count = r->binding_sites;
#endif
    return true;
}

// Closes the loop. A def^ or a statement list inside this one ran its own
// walks, and a seed it read may as easily have been this one's as its own --
// the two are not told apart, so the reading is carried outward. One walk too
// many costs a walk; one too few costs the writer an annotation.
void chk_rounds_end(Checker *c, Rounds *r)
{
    c->read_provisional = r->read_provisional_outside || c->read_provisional;
}

// ---------------------------------------------------------------------------
// [syntax.annot.literal-args]: the kind an argument stands for. A bare name arrives as a
// string^ -- what the name means is the host's to decide, and the checker
// has nothing to resolve it against.
static LhatType *annotation_argument_type(Checker *c, const LhatNode *node)
{
    if (node == NULL) {
        return NULL;
    }
    switch (node->kind) {
        case LHAT_NODE_INT:
        case LHAT_NODE_FLOAT:
            return chk_simple(c, LHAT_TYPE_NUMBER);
        case LHAT_NODE_UNARY:  // [syntax.annot.literal-args]'s leading '-', which only a number takes
            return chk_simple(c, LHAT_TYPE_NUMBER);
        case LHAT_NODE_STRING:
        case LHAT_NODE_IDENT:
            return chk_simple(c, LHAT_TYPE_STRING);
        case LHAT_NODE_HAT_IDENT:
            return chk_simple(c, LHAT_TYPE_BOOL);
        default:
            return NULL;
    }
}

// [syntax.type.variadic]'s floor and [syntax.type.signature]'s exact count, over a list the parser already kept
// to literals -- so this asks the kinds and nothing else.
static bool chk_annotation_arguments_fit(Checker *c, const LhatNode *at,
                                         const LhatType *signature)
{
    if (signature->kind != LHAT_TYPE_FUNC) {
        return false;
    }
    const LhatTypeList *want = signature->v.func.params;
    const LhatNode *given = at->v.named.members;

    for (; want != NULL; want = want->next) {
        if (given == NULL) {
            return false;
        }
        LhatType *is = annotation_argument_type(c, given);
        if (is == NULL || !lhat_type_conforms(is, want->type)) {
            return false;
        }
        given = given->next;
    }
    if (given != NULL && signature->v.func.variadic == NULL) {
        return false;
    }
    for (; given != NULL; given = given->next) {
        LhatType *is = annotation_argument_type(c, given);
        if (is == NULL ||
            !lhat_type_conforms(is, signature->v.func.variadic)) {
            return false;
        }
    }
    return true;
}

// [syntax.annot.host.two-answers]: whether this one and something already written in the file were
// registered as two answers to one question. Read from both sides -- what is
// written now naming what is there, and what is there naming it -- so a host
// that said it once has said it, and the order the two were written in is
// nothing the answer turns on.
static bool chk_annotation_excluded(Checker *c,
                                    const LhatAnnotationDecl *found,
                                    size_t which)
{
    for (size_t i = 0; i < c->require.annotation_count; i++) {
        if (i == which || !c->annotation_seen[i]) {
            continue;
        }
        const LhatAnnotationDecl *other = &c->require.annotations[i];
        for (size_t k = 0; k < found->exclusive_count; k++) {
            if (strcmp(found->exclusives[k], other->name) == 0) {
                return true;
            }
        }
        for (size_t k = 0; k < other->exclusive_count; k++) {
            if (strcmp(other->exclusives[k], found->name) == 0) {
                return true;
            }
        }
    }
    return false;
}


// [syntax.annot.host.alone]: whether one of the names the host said this needs is written on
// the same declaration. `list` is that declaration's annotations, which is
// the whole of what "beside" means -- a name found elsewhere in the file is
// somewhere else.
//
// Answers which one was wanted where none is there, so the diagnostic can
// name it: the caret is already on what was written, and what a reader is
// short of is the other one. Several may be said and any one of them does,
// so the first is the one named.
static const char *chk_annotation_missing(const LhatAnnotationDecl *found,
                                          const LhatNode *list,
                                          Checker *c)
{
    if (found->requisite_count == 0) {
        return NULL;
    }
    for (size_t k = 0; k < found->requisite_count; k++) {
        size_t wanted = strlen(found->requisites[k]);
        for (const LhatNode *at = list; at != NULL; at = at->next) {
            const char *name = NULL;
            size_t length = 0;
            if (chk_node_name(c, at->v.named.name, &name, &length) &&
                length == wanted &&
                memcmp(found->requisites[k], name, length) == 0) {
                return NULL;
            }
        }
    }
    return found->requisites[0];
}
// [syntax.annot.host]: an annotation is only what the host registered, only where the
// registration allows, and only with the arguments it declared. The three
// are separate refusals because what a writer changes differs.
void chk_check_annotations(Checker *c, const LhatNode *list, uint32_t target)
{
    for (const LhatNode *at = list; at != NULL; at = at->next) {
        const char *name = NULL;
        size_t length = 0;
        if (!chk_node_name(c, at->v.named.name, &name, &length)) {
            continue;
        }

        const LhatAnnotationDecl *found = NULL;
        for (size_t i = 0; i < c->require.annotation_count; i++) {
            const LhatAnnotationDecl *decl = &c->require.annotations[i];
            if (strlen(decl->name) == length &&
                memcmp(decl->name, name, length) == 0) {
                found = decl;
                break;
            }
        }
        if (found == NULL) {
            // [lsp.next]: the nearest the host did register -- for what this is
            // written above, since one registered elsewhere would be the
            // next diagnostic.
            ChkNearest near;
            if (chk_nearest_start(c, &near, name, length)) {
                for (size_t i = 0; i < c->require.annotation_count; i++) {
                    const LhatAnnotationDecl *decl = &c->require.annotations[i];
                    if ((decl->targets & target) != 0) {
                        chk_nearest_offer(&near, decl->name,
                                          strlen(decl->name));
                    }
                }
            }
            chk_report_near(c, at->v.named.name,
                            LHAT_CHECK_ERR_NO_SUCH_ANNOTATION, name, length,
                            at->v.named.name, &near);
            continue;
        }
        if ((found->targets & target) == 0) {
            chk_report_named(c, at->v.named.name,
                             LHAT_CHECK_ERR_ANNOTATION_MISPLACED, name,
                             length);
            continue;
        }

        // [syntax.annot.host]: written once in a file. Counted per registration rather than
        // between them, so two names both registered this way are each once
        // and not once together -- what makes two of them one choice is the
        // exclusion below, which the host is the one to say.
        size_t which = (size_t)(found - c->require.annotations);
        if (c->annotation_seen != NULL) {
            // The claim is the node, not a flag: [pipeline.infer.rounds]'s rounds walk the
            // same statements again, and the same annotation seen on the
            // second walk is the one writing it always was.
            if ((found->targets & LHAT_ANNOTATION_FILEUNIQUE) != 0 &&
                c->annotation_seen[which] != NULL &&
                c->annotation_seen[which] != at) {
                LhatFixSlot away;
                chk_report_fix(c, at->v.named.name,
                               LHAT_CHECK_ERR_ANNOTATION_REPEATED, name,
                               length, &away, annotation_removal(c, at, &away));
                continue;
            }
            c->annotation_seen[which] = at;
        }

        // [syntax.annot.host.two-answers]: and what it may not stand beside. Read both ways -- this
        // one naming what is already there, or what is already there naming
        // this one -- so which of a pair was written first changes nothing,
        // and a host saying it from one side is enough.
        if (c->annotation_seen != NULL &&
            chk_annotation_excluded(c, found, which)) {
            LhatFixSlot away;
            chk_report_fix(c, at->v.named.name,
                           LHAT_CHECK_ERR_ANNOTATION_EXCLUSIVE, name, length,
                           &away, annotation_removal(c, at, &away));
            continue;
        }

        // [syntax.annot.host.alone]: and what it has to stand beside. On this declaration --
        // the list being walked is this declaration's -- so a mark that only
        // works with another is refused where it was written rather than
        // where its other half is not.
        const char *missing = chk_annotation_missing(found, list, c);
        if (missing != NULL) {
            chk_report_named(c, at->v.named.name,
                             LHAT_CHECK_ERR_ANNOTATION_REQUISITE, missing,
                             strlen(missing));
            continue;
        }

        // [syntax.annot.literal-args] keeps every argument a literal, so what is asked of them is a
        // count and a kind -- the same question a call asks, over a list the
        // parser already refused anything else into.
        if (found->signature != NULL &&
            !chk_annotation_arguments_fit(c, at, found->signature)) {
            chk_report_named(c, at->v.named.name,
                             LHAT_CHECK_ERR_ANNOTATION_ARGUMENTS, name,
                             length);
        }
    }
}

// Entry point
// ---------------------------------------------------------------------------

void lhat_check_unit(const LhatNode *unit, const LhatLexer *lexer, bool strict,
                     LhatTypeArena *arena, const LhatRequire *require,
                     LhatCheckResult *result)
{
    memset(result, 0, sizeof *result);
    if (arena != NULL) {
        result->types = arena;
    } else {
        lhat_type_arena_init(&result->owned);
        result->types = &result->owned;
    }

    if (unit == NULL || lexer == NULL) {
        return;
    }

    Scope scope;
    chk_scope_open(&scope, NULL, unit, false);
    // [lsp.hover]: the file's scope is the file and not the last statement in
    // it. A cursor being typed at is past every node there is, and the names
    // the unit bound are in scope there as much as anywhere.
    scope.from = 0;
    scope.to = (uint32_t)lexer->source->length + 1;

    Checker checker;
    memset(&checker, 0, sizeof checker);
    checker.lexer = lexer;
    checker.unit = unit;
    checker.result = result;
    result->strict = strict;
    checker.scope = &scope;
    if (require != NULL) {
        checker.require = *require;
    }
    // [syntax.annot.host]: one flag per registration, for the ones written once in a
    // file. Dropped rather than failing the check when there is no room --
    // what is lost is a repeat going unreported, and refusing to check a unit
    // over that would cost more than it saves.
    if (checker.require.annotation_count > 0) {
        checker.annotation_seen = (const LhatNode **)lhat_alloc(
            checker.require.annotation_count * sizeof *checker.annotation_seen);
        if (checker.annotation_seen != NULL) {
            memset(checker.annotation_seen, 0,
                   checker.require.annotation_count *
                       sizeof *checker.annotation_seen);
        }
    }

    // [modules.unit]: read before the statements, so a diagnostic about the path
    // does not wait behind the body.
    result->module_name = chk_read_module_name(&checker, unit->v.list.items);

    // [syntax.type.variadic] with [modules.unit.optional]: a script's top level is 'p^...' -- whatever
    // runs it (lhat_run, a require^, std.load) hands it arguments, and '...'
    // is the collector, any^ throughout. A module^ unit takes nothing.
    if (result->module_name == NULL) {
        LhatType *collected = lhat_type_table(result->types);
        if (collected != NULL) {
            collected->v.table.variadic = chk_simple(&checker, LHAT_TYPE_ANY);
            Binding *b = chk_scope_add(&scope, "...", 3, collected, 0);
            if (b != NULL) {
                b->reached = true;
                b->is_parameter = true;
                b->declaration = unit;
                ((LhatNode *)unit)->checked_binding = unit;
            }
        }
    }

    // [syntax.annot.where]: the unit's own, written at its head.
    chk_check_annotations(&checker, unit->v.list.annotations,
                          LHAT_ANNOTATION_UNIT);

    // [pipeline.infer.expected]: read before the walk, since what an operator demands of a
    // parameter depends on it and [syntax.stmt.visible] puts every name of the unit in reach of
    // every body.
    chk_collect_operator_carriers(&checker, unit->v.list.items);

    chk_check_statements(&checker, unit->v.list.items);

    // [modules.public]: gathered once the whole unit has been checked, so a public^
    // name written late is not missed. [modules.host.import] already makes every name visible
    // throughout the scope, so this only reads what is there.
    result->exports = chk_collect_exports(&checker, unit->v.list.items);
    chk_check_task_initialization(&checker);

#if LHAT_WITH_RESOLUTIONS
    // [lsp.hover]: last, so that everything the walk recorded is in hand --
    // what lhat_check_resolution_at's binary search needs is an order the
    // walk itself does not quite give.
    chk_settle_resolutions(result);
    chk_settle_member_sites(result);
#endif

    chk_dispose_operator_carriers(&checker);
    chk_detach_instantiations(&checker);
    lhat_free(checker.annotation_seen);
    chk_scope_close(&checker, &scope);
    result->unit = unit;
}

// [pipeline.machine]: the names the inputs so far have bound, with their types. The
// names are copies -- a Binding points into the source it was read from, and
// that source goes when its input does. The arena is the session's so the
// types outlive any one input, the same way [modules.deps] shares one across a
// program's units.
struct LhatCheckSession {
    LhatTypeArena types;
    LhatAstArena seeds;
    struct {
        char *name;
        size_t length;
        LhatType *type;
        LhatType *named_type;
        const LhatNode *declaration;
        const LhatDefinition *definition;
        const LhatModuleRoot *module_root;
        bool import_root;
        // [syntax.stmt.let]: which word bound it, so that a let^ in one input is still a
        // let^ when a later one writes ':=' -- and a var^ over it makes the
        // name writable again, the way [pipeline.machine] makes a redefinition the
        // same place written afresh.
        bool immutable;
    } *names;
    size_t count;
    size_t capacity;

    // [modules.host.machine]: made on the first input that mentions L^ and kept, so a
    // module one input registers is still there in the next.
    LhatType *environment;

    // [syntax.obj.typeof]: made on the first input that mentions typeof^ and kept,
    // the same way -- so a typeof^ result bound in one input is still the
    // same nominal type when a later input names it in a comparison.
    LhatType *typeinfo_type;

    // [modules.host.machine] and [modules.host.initial]: what the host put in L^, and the names it bound to
    // members of it. A prompt has no LhatProgram to carry these.
    LhatType *globals;
    const char *const *initial_names;
    const char *const *initial_members;
    size_t initial_count;

    // [modules.host.import]: the host registry import^ resolves against, when a host made
    // an LhatProgram to hold one and handed it over. NULL when it did not,
    // and then a prompt's import^ finds nothing.
    LhatType *hosted;
};

void lhat_check_session_bind(LhatCheckSession *session,
                             const char *const *names,
                             const char *const *members, size_t count)
{
    if (session == NULL) {
        return;
    }
    session->initial_names = names;
    session->initial_members = members;
    session->initial_count = count;
}

void lhat_check_session_hosted(LhatCheckSession *session, LhatType *hosted,
                               LhatType *globals)
{
    if (session == NULL) {
        return;
    }
    session->hosted = hosted;
    // [modules.host.machine]: the two arrive together, since both are what the same registry
    // registered. Replacing rather than merging is what makes handing over a
    // program the whole answer.
    if (globals != NULL) {
        session->globals = globals;
    }
}

LhatCheckSession *lhat_check_session_new(void)
{
    LhatCheckSession *session =
        (LhatCheckSession *)lhat_calloc(1, sizeof *session);
    if (session != NULL) {
        lhat_type_arena_init(&session->types);
        lhat_arena_init(&session->seeds);
    }
    return session;
}

void lhat_check_session_dispose(LhatCheckSession *session)
{
    if (session == NULL) {
        return;
    }
    for (size_t i = 0; i < session->count; i++) {
        lhat_free(session->names[i].name);
    }
    lhat_free(session->names);
    lhat_type_arena_dispose(&session->types);
    lhat_arena_dispose(&session->seeds);
    lhat_free(session);
}

// Keeps what an input bound, replacing what an earlier one bound under the
// same name -- [syntax.stmt.visible] makes a second let^ a new name, and at the top level of a
// REPL that is what a writer means by it.
static void session_keep(LhatCheckSession *session, const char *name,
                         size_t length, LhatType *type, bool immutable,
                         LhatType *named_type, const LhatNode *declaration,
                         const LhatDefinition *definition,
                         const LhatModuleRoot *module_root, bool import_root)
{
    for (size_t i = 0; i < session->count; i++) {
        if (session->names[i].length == length &&
            memcmp(session->names[i].name, name, length) == 0) {
            session->names[i].type = type;
            session->names[i].immutable = immutable;
            session->names[i].named_type = named_type;
            session->names[i].declaration = declaration;
            session->names[i].definition = definition;
            session->names[i].module_root = module_root;
            session->names[i].import_root = import_root;
            return;
        }
    }
    LHAT_GROW(session->names, session->count, session->capacity, 16, return);
    char *kept = (char *)lhat_alloc(length + 1);
    if (kept == NULL) {
        return;
    }
    memcpy(kept, name, length);
    kept[length] = '\0';
    session->names[session->count].name = kept;
    session->names[session->count].length = length;
    session->names[session->count].type = type;
    session->names[session->count].immutable = immutable;
    session->names[session->count].named_type = named_type;
    session->names[session->count].declaration = declaration;
    session->names[session->count].definition = definition;
    session->names[session->count].module_root = module_root;
    session->names[session->count].import_root = import_root;
    session->count++;
}

const LhatNode *lhat_check_session_seed(LhatCheckSession *session, const char *name,
                             size_t length)
{
    LhatNode *declaration = lhat_node_new(&session->seeds, LHAT_NODE_IDENT, NULL);
    if (declaration == NULL) return NULL;
    session_keep(session, name, length,
                 lhat_type_simple(&session->types, LHAT_TYPE_UNKNOWN), false,
                 NULL, declaration, NULL, NULL, false);
    for (size_t i = 0; i < session->count; i++) {
        if (session->names[i].declaration == declaration) return declaration;
    }
    return NULL;
}

void lhat_check_next(LhatCheckSession *session, const LhatNode *unit,
                     const LhatLexer *lexer, bool strict,
                     LhatCheckResult *result)
{
    memset(result, 0, sizeof *result);
    result->types = &session->types;

    if (unit == NULL || lexer == NULL) {
        return;
    }

    // [pipeline.machine]: one scope across the whole session. A name written again is
    // the same place written again, so the top level of a prompt keeps the
    // slot it had rather than taking another.
    Scope scope;
    chk_scope_open(&scope, NULL, NULL, false);

    Checker checker;
    memset(&checker, 0, sizeof checker);
    checker.lexer = lexer;
    checker.unit = unit;
    checker.result = result;
    result->strict = strict;
    checker.scope = &scope;
    checker.session = true;  // [modules.hostvalue]: a prompt's top level, see Checker
    // [modules.host.machine]: one L^ for the whole session, so what an input registers in
    // it is still there in the next.
    checker.environment = session->environment;
    checker.typeinfo_type = session->typeinfo_type;
    // [modules.host.machine] and [modules.host.initial]: what the host put in L^ and bound names to. A file
    // gets these through LhatRequire; a prompt has no program to carry them.
    checker.require.globals = session->globals;
    checker.require.initial_names = session->initial_names;
    checker.require.initial_members = session->initial_members;
    checker.require.initial_count = session->initial_count;
    // [modules.host.import]: and what import^ resolves against, when a host handed a
    // registry over. NULL leaves import^ finding nothing, as before.
    checker.require.hosted = session->hosted;

    // [pipeline.machine]: the last statement is what the input answers with, when it
    // is an expression. What that changes here is [syntax.sub.await]'s reasoning about a
    // call whose value goes nowhere.
    if (unit->v.list.items != NULL) {
        const LhatNode *last = unit->v.list.items;
        while (last->next != NULL) {
            last = last->next;
        }
        if (last->kind == LHAT_NODE_CALL_STMT) {
            checker.answering = last;
        }
    }

    // What earlier inputs bound is settled already -- [syntax.stmt.visible]'s "used before
    // defined" is about the statements of this input.
    for (size_t i = 0; i < session->count; i++) {
        Binding *b = chk_scope_add(&scope, session->names[i].name,
                                   session->names[i].length,
                                   session->names[i].type, 0);
        if (b != NULL) {
            b->reached = true;
            b->from_session = true;
            b->declaration = session->names[i].declaration;
            b->definition = session->names[i].definition;
            b->module_root = session->names[i].module_root;
            b->import_root = session->names[i].import_root;
            b->immutable = session->names[i].immutable;
            b->named_type = session->names[i].named_type;
            b->names_type = b->named_type != NULL;
        }
    }

    // [pipeline.infer.expected]: this input's own, as a unit's are. What an earlier input
    // bound is not read here -- a prompt's answer stays what the line in
    // front of the writer says.
    chk_collect_operator_carriers(&checker, unit->v.list.items);

    chk_check_statements(&checker, unit->v.list.items);
    result->exports = chk_collect_exports(&checker, unit->v.list.items);

#if LHAT_WITH_RESOLUTIONS
    chk_settle_member_sites(result);
    chk_settle_resolutions(result);  // as lhat_check_unit does, and why
#endif

    // What this input bound joins the session, replacing what an earlier one
    // bound under the same name. The types are in the session's arena
    // already, so only the names are copied.
    for (Binding *b = scope.bindings; b != NULL; b = b->next) {
        session_keep(session, b->name, b->name_length, b->type, b->immutable,
                     b->named_type, b->declaration, b->definition,
                     b->module_root, b->import_root);
    }
    session->environment = checker.environment;
    session->typeinfo_type = checker.typeinfo_type;

    chk_dispose_operator_carriers(&checker);
    chk_detach_instantiations(&checker);
    chk_scope_close(&checker, &scope);
    result->unit = unit;
}

void lhat_check(const LhatNode *unit, const LhatLexer *lexer, bool strict,
                LhatCheckResult *result)
{
    lhat_check_unit(unit, lexer, strict, NULL, NULL, result);
}

// [modules.host.import]: from written text to a type, with no unit around it. The
// checker needs a scope for the names a type may mention, so `named`'s
// members are seeded into one -- which is how a registration names a type an
// earlier one made, and why nothing else is reachable from here.
LhatType *lhat_type_of_text(const char *text, size_t length,
                            LhatTypeArena *arena, LhatType *named,
                            LhatType *self)
{
    LhatSource source;
    if (!lhat_source_init_from_string(&source, "<signature>", text, length)) {
        return NULL;
    }

    LhatLexer lexer;
    lhat_lexer_init(&lexer, &source);

    LhatParseResult parsed;
    lhat_parse_type_only(&lexer, &parsed);

    LhatType *type = NULL;
    if (parsed.diagnostic_count == 0 && lexer.diagnostic_count == 0 &&
        parsed.root != NULL) {
        LhatCheckResult result;
        memset(&result, 0, sizeof result);
        result.types = arena;

        Scope scope;
        chk_scope_open(&scope, NULL, NULL, false);

        Checker checker;
        memset(&checker, 0, sizeof checker);
        checker.lexer = &lexer;
        checker.result = &result;
        result.strict = true;
        checker.hosted_signature = true;
        checker.scope = &scope;
        // [syntax.type.self]: the owner is the structure the text is written inside.
        struct SelfLink owner = { self, NULL };
        if (self != NULL) {
            checker.self_link = &owner;
        }

        if (named != NULL && named->kind == LHAT_TYPE_TABLE) {
            for (const LhatTypeMember *m = named->v.table.members; m != NULL;
                 m = m->next) {
                Binding *b = chk_scope_add(&scope, m->name, m->name_length,
                                           m->type, 0);
                if (b != NULL) {
                    b->reached = true;
                    b->names_type = m->names_type;
                    b->named_type = m->named_type;
                }
            }
        }

        type = chk_resolve_type(&checker, parsed.root);
        // A name the scope does not hold resolves to UNKNOWN rather than to
        // nothing, and a signature that says nothing is not one.
        if (type != NULL &&
            (type->kind == LHAT_TYPE_UNKNOWN || result.diagnostic_count > 0)) {
            type = NULL;
        }
        // The scope first: closing one records into the result ([lsp.hover]),
        // and this one has no source to record against -- but the order is
        // what makes that a fact rather than a thing to be checked.
        chk_scope_close(&checker, &scope);
        lhat_check_result_dispose(&result);
    }

    lhat_parse_result_dispose(&parsed);
    lhat_lexer_dispose(&lexer);
    lhat_source_dispose(&source);
    return type;
}

#if LHAT_WITH_RESOLUTIONS
const LhatTryRoute *lhat_check_try_route_at(const LhatCheckResult *result,
                                            uint32_t offset, size_t *cursor)
{
    // The word is the whole of what a hover on a try^ is about; its operand has
    // answers of its own.
    static const uint32_t word = sizeof "try^" - 1;
    for (; result != NULL && *cursor < result->try_route_count; (*cursor)++) {
        const LhatTryRoute *route = &result->try_routes[*cursor];
        if (route->at <= offset && offset <= route->at + word) {
            (*cursor)++;
            return route;
        }
    }
    return NULL;
}

const LhatResolution *lhat_check_resolution_at(const LhatCheckResult *result,
                                               uint32_t offset)
{
    if (result == NULL) {
        return NULL;
    }
    // chk_settle_resolutions put these in `use` order, so this can halve its
    // way in rather than scanning.
    size_t low = 0;
    size_t high = result->resolution_count;
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        const LhatResolution *entry = &result->resolutions[mid];
        if (offset < entry->use) {
            high = mid;
        } else if (offset >= entry->use_end) {
            low = mid + 1;
        } else {
            return entry;
        }
    }
    return NULL;
}

const LhatMemberSite *lhat_check_member_site_at(const LhatCheckResult *result,
                                                uint32_t offset)
{
    if (result == NULL) {
        return NULL;
    }
    size_t low = 0;
    size_t high = result->member_site_count;
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        const LhatMemberSite *entry = &result->member_sites[mid];
        // Open below and closed above -- the cursor after a freshly typed
        // '.' stands exactly on `end`, and a cursor on the dot itself has
        // not yet asked about it.
        if (offset <= entry->dot) {
            high = mid;
        } else if (offset > entry->end) {
            low = mid + 1;
        } else {
            return entry;
        }
    }
    return NULL;
}

void lhat_check_bindings_at(const LhatCheckResult *result, uint32_t offset,
                            LhatBindingSink sink, void *context)
{
    if (result == NULL || sink == NULL) {
        return;
    }
    // A scan rather than a search: these spans nest instead of dividing the
    // source between them, so no order over them puts the ones holding a
    // position together. What the recording order does give is the one thing
    // that matters -- a scope closes before the one around it, so the
    // innermost holder of any position comes first.
    for (size_t i = 0; i < result->binding_site_count; i++) {
        const LhatBindingSite *site = &result->binding_sites[i];
        if (offset >= site->from && offset < site->to) {
            sink(context, site);
        }
    }
}

#endif  // LHAT_WITH_RESOLUTIONS

// [syntax.obj.structure-repeat]: a table is a sequence as well as a mapping, and the sequence half
// is members whose names are the digits of their position. [lex.ident.chars] spells
// a name as an identifier, so a program can never write one of these.
static bool is_positional(const LhatTypeMember *m)
{
    if (m->name_length == 0) {
        return false;
    }
    for (size_t i = 0; i < m->name_length; i++) {
        if (m->name[i] < '0' || m->name[i] > '9') {
            return false;
        }
    }
    return true;
}

void lhat_check_written_members(const LhatType *receiver, LhatMemberSink sink,
                                void *context)
{
    if (receiver == NULL || sink == NULL) {
        return;
    }
    // [errors.kind.errordef]: an error kind keeps its fields on a list of their own.
    if (receiver->kind == LHAT_TYPE_ERROR_KIND) {
        for (const LhatTypeMember *m = receiver->v.error.fields; m != NULL;
             m = m->next) {
            sink(context, m);
        }
        return;
    }
    if (receiver->kind != LHAT_TYPE_TABLE &&
        receiver->kind != LHAT_TYPE_HOSTVALUE) {
        return;
    }

    // [modules.hostdata.base] and [syntax.obj.delegate]: two of the three places a member can be are
    // links, so the chain is what the machine walks and this walks it too.
    //
    // Unlike rttype.c's walk (src/rttype.c), this does NOT stop at a host
    // type's tag. That one keeps the tag rather than copying an engine's
    // whole API into every descriptor, which is a size decision about what
    // travels; here the host's API is the very thing a reader is asking for.
    LhatChain walk = lhat_type_chain(receiver);
    const LhatType *up;
    while ((up = lhat_chain_next(&walk)) != NULL) {
        for (const LhatTypeMember *m = up->v.table.members; m != NULL;
             m = m->next) {
            // The one rule, and it is the lookup's own: a member the search
            // does not answer with is one a nearer type shadows, or one a
            // delegate does not lend ([syntax.obj.delegate] lends only what takes a
            // receiver).
            if (lhat_type_find_member(receiver, m->name, m->name_length) != m) {
                continue;
            }
            if (m->ambiguous || is_positional(m)) {
                continue;
            }
            sink(context, m);
        }
    }
}

// [lsp.hover]: the built-ins, asked for one spelling at a time.
//
// A probe Checker: zeroed but for the result, which is every field the
// cascade reads that a question needs. `strict` stays false and costs
// nothing -- [errors.runtime.nil]'s nil^ rule is applied by whoever settled the receiver
// (LhatMemberSite records it already stripped), not here. `lexer` stays NULL
// because a name arrives as a spelling rather than as a node to read one off.
static LhatType *probe(Checker *c, LhatType *receiver, const char *name,
                       size_t length)
{
    LhatType *answer = chk_member_of(c, receiver, name, length, NULL, NULL);
    // Everything the cascade refuses comes back as one of these two, so this
    // is the whole of "does it answer".
    if (answer == NULL || answer->kind == LHAT_TYPE_UNKNOWN ||
        answer->kind == LHAT_TYPE_PENDING) {
        return NULL;
    }
    return answer;
}

void lhat_check_builtin_members(LhatCheckResult *result, LhatType *receiver,
                                LhatBuiltinSink sink, void *context)
{
    if (result == NULL || receiver == NULL || sink == NULL) {
        return;
    }
    // The same refusal chk_infer_member makes before it reaches the cascade:
    // a receiver nothing decided answers nothing. Asking anyway would reach
    // the tail, where tostring answers for every type -- an answer a real
    // access never gets, because it never gets that far.
    if (receiver->kind == LHAT_TYPE_UNKNOWN ||
        receiver->kind == LHAT_TYPE_PENDING) {
        return;
    }
    Checker checker;
    memset(&checker, 0, sizeof checker);
    checker.result = result;

    // [syntax.enum]: an enum's members are its own names rather than words from
    // the list, and the declaration holds them -- so they are read off the
    // type instead of guessed at. What each answers is the wide type, which
    // is what a member access there answers too.
    if (receiver->kind == LHAT_TYPE_ENUM) {
        for (const LhatTypeList *k = receiver->v.error.kinds; k != NULL;
             k = k->next) {
            sink(context, k->type->v.error.name, k->type->v.error.name_length,
                 receiver);
        }
    }

    for (size_t i = 0; i < chk_builtin_word_count; i++) {
        const char *word = chk_builtin_words[i];
        size_t length = strlen(word);

        LhatType *bare = probe(&checker, receiver, word, length);
        if (bare != NULL) {
            sink(context, word, length, bare);
        }
        // [lex.hat.repeat]: 'length' and 'length^' are two names, and which of them
        // a receiver answers is the cascade's to say -- a plain table takes
        // the hatted spelling of iterate and a def^ takes both. Both are
        // tried and whichever answers is offered, so no rule is repeated.
        char hatted[64];
        if (length + 2 > sizeof hatted) {
            continue;
        }
        memcpy(hatted, word, length);
        hatted[length] = '^';
        hatted[length + 1] = '\0';
        LhatType *worn = probe(&checker, receiver, hatted, length + 1);
        if (worn != NULL) {
            sink(context, hatted, length + 1, worn);
        }
    }
}

void lhat_check_number_constants(LhatCheckResult *result,
                                 LhatBuiltinSink sink, void *context)
{
    if (result == NULL || sink == NULL) {
        return;
    }
    Checker checker;
    memset(&checker, 0, sizeof checker);
    checker.result = result;
    for (size_t i = 0;; i++) {
        size_t length = 0;
        const char *name = lhat_number_constant_at(i, &length);
        if (name == NULL) {
            break;
        }
        sink(context, name, length, chk_simple(&checker, LHAT_TYPE_NUMBER));
    }
}

void lhat_check_result_dispose(LhatCheckResult *result)
{
    lhat_free(result->diagnostics);
    result->diagnostics = NULL;
    result->diagnostic_count = 0;
    result->diagnostic_capacity = 0;
#if LHAT_WITH_RESOLUTIONS
    lhat_free(result->resolutions);
    result->resolutions = NULL;
    result->resolution_count = 0;
    result->resolution_capacity = 0;
    lhat_free(result->member_sites);
    lhat_free(result->try_routes);
    result->member_sites = NULL;
    result->member_site_count = 0;
    result->member_site_capacity = 0;
    lhat_free(result->binding_sites);
    result->binding_sites = NULL;
    result->binding_site_count = 0;
    result->binding_site_capacity = 0;
#endif
    lhat_free(result->module_name);
    result->module_name = NULL;
    while (result->fix_texts != NULL) {
        struct LhatFixText *next = result->fix_texts->next;
        lhat_free(result->fix_texts);
        result->fix_texts = next;
    }
    // Only the arena this result made for itself; a shared one belongs to
    // whoever passed it in.
    if (result->types == &result->owned) {
        lhat_type_arena_dispose(&result->owned);
    }
    result->types = NULL;
}

const LhatTypeMember *lhat_check_unimplemented_member(const LhatType *type)
{
    // The walk itself is chk_unimplemented_member's, which [syntax.obj.new] already asks
    // before letting a new through -- named here so that a reader outside the
    // checker asks the same question rather than a second one that looks like
    // it.
    return chk_unimplemented_member(type);
}

// [l10n.id]: the ID and the English text of every LhatCheckErrorCode, indexed by
// the code. The ID is the one a translation is written against and never
// changes meaning; the English is the reference every other language translates
// ([l10n.principle.english]).
static const LhatMessageEntry CHECK_MESSAGES[] = {
    [LHAT_CHECK_ERR_NONE] = {"check.none", "no error"},
    [LHAT_CHECK_ERR_UNDEFINED] =
        {"check.undefined", "no such name in scope: {name}"},
    [LHAT_CHECK_ERR_USED_BEFORE_DEFINED] = {"check.used-before-defined",
        "this name is read before its let^ has run"},
    [LHAT_CHECK_ERR_REDEFINED] = {"check.redefined",
        "this name is already defined in this scope: {name}"},
    [LHAT_CHECK_ERR_UNKNOWN_TYPE] = {"check.unknown-type", "no such type"},
    [LHAT_CHECK_ERR_NO_SUCH_ANNOTATION] = {"check.no-such-annotation",
        "no host registered an annotation of this name: {annotation}"},
    [LHAT_CHECK_ERR_ANNOTATION_MISPLACED] = {"check.annotation-misplaced",
        "this annotation was not registered for what it is "
        "written above: {annotation}"},
    [LHAT_CHECK_ERR_ANNOTATION_REPEATED] = {"check.annotation-repeated",
        "this annotation may be written once in a file, and "
        "already was: {annotation}"},
    [LHAT_CHECK_ERR_ANNOTATION_EXCLUSIVE] = {"check.annotation-exclusive",
        "this annotation and another written in this file are two "
        "answers to one question: write whichever one applies, "
        "not both: {annotation}"},
    [LHAT_CHECK_ERR_ANNOTATION_REQUISITE] = {"check.annotation-requisite",
        "this annotation means nothing on its own, and the one it "
        "has to stand beside is missing: {annotation}"},
    [LHAT_CHECK_ERR_ANNOTATION_ARGUMENTS] = {"check.annotation-arguments",
        "these are not the arguments the annotation was "
        "registered with: {annotation}"},
    [LHAT_CHECK_ERR_BARE_TABLE_TYPE] = {"check.bare-table-type",
        "a t^ is written with the members it asks for, and the top "
        "of tables asks for none: write 't^{}'"},
    [LHAT_CHECK_ERR_MISMATCH] =
        {"check.mismatch", "this value does not fit where it is written"},
    [LHAT_CHECK_ERR_NOT_NUMBER] =
        {"check.not-number", "arithmetic needs number^"},
    [LHAT_CHECK_ERR_NOT_BOOL] = {"check.not-bool", "this has to be bool^"},
    [LHAT_CHECK_ERR_BAD_STEP] = {"check.bad-step", "step^ must be a number greater than zero"},
    [LHAT_CHECK_ERR_MATCH_NOT_EXHAUSTIVE] = {"check.match-not-exhaustive",
        "a match written as an expression answers in every case, "
        "and these arms do not cover every value the subject may "
        "hold; write 'other^: ...' before the ';'"},
    [LHAT_CHECK_ERR_NOT_CALLABLE] =
        {"check.not-callable", "this is not a function or a procedure"},
    [LHAT_CHECK_ERR_CALL_UNION_NARROW] =
        {"check.call-union-narrow", "this union has no common callable signature; narrow it with fits^ to a function or procedure signature before calling"},
    [LHAT_CHECK_ERR_FUNCTION_CALLS_UNION] =
        {"check.function-calls-union", "this value may be a procedure; inside f^, narrow it with fits^ {member} before calling"},
    [LHAT_CHECK_ERR_FUNCTION_CALLS_PROCEDURE] =
        {"check.function-calls-procedure",
         "f^ may call only f^, and this callee is a p^"},
    [LHAT_CHECK_ERR_ARITY] = {"check.arity", "the wrong number of arguments"},
    [LHAT_CHECK_ERR_ARITY_SPREAD] = {"check.arity-spread",
        "the wrong number of arguments -- each position of a spread "
        "tuple is one of them"},
    [LHAT_CHECK_ERR_NOT_VARIADIC] = {"check.not-variadic",
        "only a tuple spreads into the parameters; anything else '...' "
        "spreads continues a variadic tail, and this callee takes none"},
    [LHAT_CHECK_ERR_SPREAD_NOT_LAST] = {"check.spread-not-last",
        "only a tuple's width is known before it runs, so anything else "
        "'...' spreads -- a table, or a value whose type is not settled "
        "-- comes last"},
    [LHAT_CHECK_ERR_SPREAD_NOT_SEQUENCE] = {"check.spread-not-sequence",
        "'...' spreads a tuple, or a table whose type says what its "
        "sequence holds (t^{ T[] }); this is neither"},
    [LHAT_CHECK_ERR_NO_MEMBER] =
        {"check.no-member", "this value has no such member"},
    [LHAT_CHECK_ERR_NO_RESULT_TYPE] = {"check.no-result-type",
        "this signature answers no value, so it has no ReturnType"},
    [LHAT_CHECK_ERR_KIND_AS_VALUE] = {"check.kind-as-value",
        "a kind is a type, not a value; error^Kind{ ... } is what "
        "makes one: {kind}"},
    [LHAT_CHECK_ERR_CANNOT_FAIL] = {"check.cannot-fail",
        "the left of catch^ cannot return an error"},
    [LHAT_CHECK_ERR_CANNOT_BE_NIL] =
        {"check.cannot-be-nil", "the left of ?? cannot be nil^"},
    [LHAT_CHECK_ERR_TRY_OUTSIDE] = {"check.try-outside",
        "try^ would return an error this subroutine cannot return"},
    [LHAT_CHECK_ERR_LOCAL_ERROR_ESCAPES] = {"check.local-error-escapes",
        "this error has to be resolved here: a localerror^ is not "
        "one a subroutine may return. Write catch^, or a catch^ arm "
        "after the statements that takes it"},
    [LHAT_CHECK_ERR_LOCAL_ERROR_WRITTEN] = {"check.local-error-written",
        "a localerror^ cannot be written where a caller would "
        "receive it -- not in a result, a yield, or a declared "
        "field"},
    [LHAT_CHECK_ERR_REQUIRE_FAILED] =
        {"check.require-failed", "this unit could not be required"},
    [LHAT_CHECK_ERR_NOT_COROUTINE] =
        {"check.not-coroutine", "this has no coroutine to walk or delegate to"},
    [LHAT_CHECK_ERR_AWAIT_NOT_COROUTINE] = {"check.await-not-coroutine",
        "await^ waits for something that finishes, and this is not "
        "one -- what may be awaited is a coroutine (15.14)"},
    [LHAT_CHECK_ERR_YIELD_AT_TOP] = {"check.yield-at-top",
        "nothing could resume this top level -- only a script's may "
        "yield^ or await^, not a module^ unit's or a prompt's"},
    [LHAT_CHECK_ERR_COROUTINE_MISMATCH] = {"check.coroutine-mismatch",
        "the coroutine this body makes is not the one written "
        "here; the yield^ sites and the c^{ … } have to agree"},
    [LHAT_CHECK_ERR_YIELDABLE_RESULT] = {"check.yieldable-result",
        "this body yields, so a call answers a coroutine; write c^{ … } "
        "here, or leave the result unwritten"},
    [LHAT_CHECK_ERR_YIELD_NEEDS_ANNOTATION] = {"check.yield-needs-annotation",
        "a yield^ that is bound needs a written type there"},
    [LHAT_CHECK_ERR_YIELD_TYPE_MISMATCH] = {"check.yield-type-mismatch",
        "every yield^ in one body has to agree on what it sends "
        "and what it answers"},
    [LHAT_CHECK_ERR_PHANTOM_YIELD_BINDS] = {"check.phantom-yield-binds",
        "a _yield^ never runs, so nothing arrives here; bind _^ "
        "-- the annotations are all this statement says"},
    [LHAT_CHECK_ERR_NOT_INDEXABLE] = {"check.not-indexable",
        "this is not a table, so a key reaches nothing; a "
        "string^'s characters answer at(i)"},
    [LHAT_CHECK_ERR_PATH_NOT_TABLE] = {"check.path-not-table",
        "this holds the name written after it, so it has to be a "
        "table"},
    [LHAT_CHECK_ERR_PATH_IS_DEFINITION] = {"check.path-is-definition",
        "a def^ says what its instances carry, so a member cannot "
        "be added to one here"},
    [LHAT_CHECK_ERR_MODULE_UNNAMED] = {"check.module-unnamed",
        "this unit declares no module^, so there is no path to "
        "bind it under; write 'let^ name = require^ ...' instead"},
    [LHAT_CHECK_ERR_NOT_HOSTED] = {"check.not-hosted",
        "import^ reaches what the host registered, and no module "
        "of this name is there; a unit read from a file comes in "
        "with require^ \"path\""},
    [LHAT_CHECK_ERR_COROUTINE_DROPPED] = {"check.coroutine-dropped",
        "this call makes a coroutine and runs none of the body; "
        "write await^ to delegate, or let^ to keep it"},
    [LHAT_CHECK_ERR_MISSING_FIELD] = {"check.missing-field",
        "this field has no default, so it has to be written"},
    [LHAT_CHECK_ERR_INCOMPARABLE] = {"check.incomparable",
        "these can never be equal, so the comparison is fixed already"},
    [LHAT_CHECK_ERR_NO_IDENTITY] = {"check.no-identity",
        "is^ requires object identity; numbers, booleans and host values "
        "have none; use '=' for value equality or number.eq(other, 0) "
        "for exact numeric equality"},
    [LHAT_CHECK_ERR_AS_IMPOSSIBLE] = {"check.as-impossible",
        "nothing is both of these, so this as^ could never succeed"},
    [LHAT_CHECK_ERR_BAD_KEY] = {"check.bad-key",
        "nil^ is how a table spells 'not there', so it cannot be a key"},
    [LHAT_CHECK_ERR_NO_OPERATOR] = {"check.no-operator",
        "an operator is answered by what stands to its left, or by "
        "what stands to its right when that side writes the self^ "
        "last; neither answers this one"},
    [LHAT_CHECK_ERR_OPERATOR_ON_MAYBE_ERROR] = {"check.operator-on-maybe-error",
        "this operand may be an error value; handle it with try^ or catch^, "
        "or narrow it with fits^ before using this operator"},
    [LHAT_CHECK_ERR_TYPE_ARGUMENT_REFERENCE] = {"check.type-argument-reference",
        "ARGn must name a fixed parameter of the enclosing signature, in its result type"},
    [LHAT_CHECK_ERR_TYPE_ATTRIBUTE] = {"check.type-attribute",
        "this type has no such type attribute; ResultType requires a coroutine type, ReturnType a subroutine type, and Tn a nominal type argument"},
    [LHAT_CHECK_ERR_TYPE_SPECIALIZATION] = {"check.type-specialization",
        "type arguments require an unspecialized nominal host type"},
    [LHAT_CHECK_ERR_TYPE_ARGUMENT_RUNTIME] = {"check.type-argument-runtime",
        "type arguments are erased at runtime; runtime type descriptors, fits^ and as^ must use the unspecialized type"},
    [LHAT_CHECK_ERR_ACCESS_ON_MAYBE_NIL] = {"check.access-on-maybe-nil",
        "this access may reach nil^; narrow the receiver or use optional access"},
    [LHAT_CHECK_ERR_OPERATOR_ON_MAYBE_NIL] = {"check.operator-on-maybe-nil",
        "this may be nil^, and nil^ answers no operator; '?\?' "
        "gives it a value, or bind it to a name and narrow that -- "
        "an index is not narrowed where it stands"},
    [LHAT_CHECK_ERR_BAD_OPERATOR] = {"check.bad-operator",
        "an op^ is an f^ taking self^ and one argument, and it may "
        "not yield^; the self^ is whichever operand it is written "
        "as -- first for the left one, last for the right. Only "
        "op^- is also written with self^ alone, which is the unary "
        "one"},
    [LHAT_CHECK_ERR_COMPARE_NOT_NUMBER] = {"check.compare-not-number",
        "op^<=> answers a number^: '<' and the rest read which "
        "side of zero the answer falls on"},
    [LHAT_CHECK_ERR_EQUAL_NOT_BOOL] = {"check.equal-not-bool",
        "op^= answers a bool^: '=' takes it as it stands, and "
        "'\xE2\x89\xA0' negates it"},
    [LHAT_CHECK_ERR_NOT_ORDERED] = {"check.not-ordered",
        "nothing here says how these compare: an ordering is read "
        "off '<=>', and neither side carries one that takes the "
        "other"},
    [LHAT_CHECK_ERR_SELF_LAST_NOT_OPERATOR] = {"check.self-last-not-operator",
        "only an op^ writes its self^ last, to say the right "
        "operand is the receiver; everywhere else the receiver is "
        "what stands before the dot"},
    [LHAT_CHECK_ERR_ISA_ALWAYS_TRUE] = {"check.isa-always-true",
        "any^ holds of every value, so this asks nothing"},
    [LHAT_CHECK_ERR_MEMBER_EXISTS] = {"check.member-exists",
        "this name is already a member; write override^ or overload^"},
    [LHAT_CHECK_ERR_FIELD_METHOD_COLLISION] = {"check.field-method-collision",
        "this name belongs to an instance field and cannot also name a shared method: {member}"},
    [LHAT_CHECK_ERR_ALREADY_PROVIDED] = {"check.already-provided",
        "something in the chain already provides this member, so "
        "there is nothing for an abstract^ to ask for"},
    [LHAT_CHECK_ERR_ABSTRACT_PROVIDED_HERE] = {"check.abstract-provided-here",
        "an abstract^ asks a composition for what this def^ does "
        "not have, and this one is written here as well; the "
        "members reach each other whatever the order, so drop the "
        "declaration"},
    [LHAT_CHECK_ERR_MUTABLE_DEFAULT] = {"check.mutable-default",
        "a field's default lives on the prototype: an immutable "
        "value, a table written out as a literal (each instance "
        "is given its own copy), or a definition (shared by "
        "design); what something else made -- a name, a call's "
        "answer -- is given inside new: {field}"},
    [LHAT_CHECK_ERR_NEW_TARGET] = {"check.new-target",
        "new^ requires a structural table type, a constructible definition, "
        "or a concrete error kind"},
    [LHAT_CHECK_ERR_NEW_CONSTRUCTOR] = {"check.new-constructor",
        "the default new has been overridden; use .new(...) to construct "
        "this definition"},
    [LHAT_CHECK_ERR_NEW_RETURNS] = {"check.new-returns",
        "construction answers the instance itself, so a new body "
        "has no return^; what it writes through self^ is already "
        "on what the caller gets"},
    [LHAT_CHECK_ERR_SELF_TABLE_OUTSIDE_NEW] = {"check.self-table-outside-new",
        "self^{ ... } is the construction notation, which only a "
        "written new runs; a method writes its receiver one "
        "field at a time, through self^.name"},
    [LHAT_CHECK_ERR_PROTOTYPE_SEALED] = {"check.prototype-sealed",
        "a definition's self^ is the prototype every instance "
        "starts as; it is read here and written by no one -- a "
        "default is settled where the field is written"},
    [LHAT_CHECK_ERR_MUTABLE_KEY] = {"check.mutable-key",
        "a key's hash is the box's bytes, and only a sealed box "
        "keeps them still; write constbox^ to make one"},
    [LHAT_CHECK_ERR_BOX_FIELD_WRITE] = {"check.box-field-write",
        "a box's field is read straight off its bytes; writing "
        "goes through set(...), which a ConstBox^ does not have"},
    [LHAT_CHECK_ERR_HOSTVALUE_TEMPORARY_WRITE] = {"check.hostvalue-temporary-write",
        "this host value is a temporary copy; bind it to a local before "
        "changing it (the change does not write back to its source)"},
    [LHAT_CHECK_ERR_NOT_BOXABLE] = {"check.not-boxable",
        "box^ takes a host value; everything else already lives "
        "on the heap and needs no box"},
    [LHAT_CHECK_ERR_STILL_ABSTRACT] = {"check.still-abstract",
        "this definition is still waiting on a composition -- a "
        "member is declared with nothing providing it, or an "
        "override^ has met nothing to replace: {member}"},
    [LHAT_CHECK_ERR_FIELD_UNPROVIDED] = {"check.field-unprovided",
        "this field is declared with no value, so an instance "
        "would hold nothing under it; a composition has to give it "
        "one, or an override^ new has to write it: {field}"},
    [LHAT_CHECK_ERR_LENT_DOES_NOT_PROVIDE] = {"check.lent-does-not-provide",
        "this member is declared with abstract^, and what a "
        "delegate^ lends does not provide it -- to use the "
        "delegate's, drop the abstract^; otherwise compose "
        "something that provides it: {member}"},
    [LHAT_CHECK_ERR_LENT_IS_NOT_REPLACED] = {"check.lent-is-not-replaced",
        "this override^ has nothing to replace -- what a delegate^ "
        "lends is not a member to replace, so super^ would reach "
        "nothing; compose what it replaces, or drop the override^: {member}"},
    [LHAT_CHECK_ERR_AMBIGUOUS_MEMBER] = {"check.ambiguous-member",
        "both sides of the composition carry this name, so it "
        "reaches no one answer; name the side you mean"},
    [LHAT_CHECK_ERR_CONCAT_COLLIDES] = {"check.concat-collides",
        "both tables carry this key, so the concatenation holds "
        "no one value for it"},
    [LHAT_CHECK_ERR_COMPOSE_COLLIDES] = {"check.compose-collides",
        "both definitions carry a member of this name, and a "
        "marker can only be written inside a def^: {member}"},
    [LHAT_CHECK_ERR_NOTHING_TO_OVERRIDE] = {"check.nothing-to-override",
        "there is no member of this name to override^ or overload^"},
    [LHAT_CHECK_ERR_NOT_SUBSTITUTABLE] = {"check.not-substitutable",
        "override^ has to be usable where the original was"},
    [LHAT_CHECK_ERR_OVERLOAD_OVERLAPS] =
        {"check.overload-overlaps", "overload^ overlaps an existing signature"},
    [LHAT_CHECK_ERR_ERROR_DROPPED] = {"check.error-dropped",
        "this can fail, and dropping the answer drops the failure "
        "with it; write try^ to hand it back, catch^ to answer "
        "instead, or a name to bind it and narrow"},
    [LHAT_CHECK_ERR_DISCARD_READ] = {"check.discard-read",
        "'_^' throws the value away, so it is not a name and there "
        "is nothing here to read; write a name where the value is "
        "wanted"},
    [LHAT_CHECK_ERR_HAT_NAME_RESERVED] = {"check.hat-name-reserved",
        "a name with a hat is the language's, not one to declare; "
        "the only ones written as a member are tostring^ and "
        "iterate^ -- drop the hat"},
    [LHAT_CHECK_ERR_NOT_DISPOSABLE]= {"check.not-disposable",
        "with^ needs a value with a dispose() that returns nothing"},
    [LHAT_CHECK_ERR_FUNCTION_FALLS_OUT] = {"check.function-falls-out",
        "a function answers on every path; this one has a path "
        "that leaves without a value"},
    [LHAT_CHECK_ERR_FALLS_OUT_OF_RESULT] = {"check.falls-out-of-result",
        "this body has a path that leaves without a value, which "
        "the result type it was given does not admit"},
    [LHAT_CHECK_ERR_SUPER_OUTSIDE] = {"check.super-outside",
        "super^ is the member an override^ replaces, so it is a "
        "name only inside one"},
    [LHAT_CHECK_ERR_THIS_OUTSIDE] = {"check.this-outside",
        "this^ names the subroutine running, and none is here"},
    [LHAT_CHECK_ERR_NEVER_RETURNS] = {"check.never-returns",
        "every way out of this body calls it again, so it never "
        "produces a value"},
    [LHAT_CHECK_ERR_RESULT_UNDECIDED] = {"check.result-undecided",
        "the result type did not come out of this body, so it has "
        "to be written"},
    [LHAT_CHECK_ERR_OPERATOR_UNSETTLED] = {"check.operator-unsettled",
        "several types here carry this operator and nothing says "
        "which is meant; write one of the types the signature "
        "names, or narrow to it with fits^"},
    [LHAT_CHECK_ERR_TEMPLATE_CONTEXT] = {"check.template-context",
        "template^ requires a source unit and fixed positional parameters"},
    [LHAT_CHECK_ERR_UNINITIALIZED_TASK_CAPTURE] = {"check.uninitialized-task-capture",
        "when this call transfers the task, captured binding '{name}' has not been initialized yet"},
    [LHAT_CHECK_ERR_CAPTURE_INITIALIZER] = {"check.capture-initializer",
        "this declaration initializes the captured binding"},
    [LHAT_CHECK_ERR_SHAPE_REFUSED] = {"check.shape-refused",
        "instantiation failed for this call"},
    [LHAT_CHECK_ERR_PARAM_UNDECIDED] = {"check.param-undecided",
        "cannot infer this parameter's type"},
    [LHAT_CHECK_ERR_TYPE_UNDECIDED] = {"check.type-undecided",
        "inference did not decide what this name holds, so its "
        "type has to be written"},
    [LHAT_CHECK_ERR_SCOPE_TOO_FAR] = {"check.scope-too-far",
        "this reaches out past more scopes than are open here"},
    [LHAT_CHECK_ERR_SCOPE_ON_DEFINE] = {"check.scope-on-define",
        "let^ makes a name here, so it takes no scope specifier; "
        "':=' is what writes one that is already there"},
    [LHAT_CHECK_ERR_FUNCTION_WRITES_OUT] = {"check.function-writes-out",
        "an f^ assigns to local variables only, and this name was "
        "bound outside its body"},
    [LHAT_CHECK_ERR_FUNCTION_CHANGES_TABLE] = {"check.function-changes-table",
        "an f^ may change only a table its own body made; this one "
        "came from somewhere else"},
    [LHAT_CHECK_ERR_OWN_LOWERED] = {"check.own-lowered",
        "this puts something this f^ did not make where the body changes "
        "things elsewhere; that change would reach whoever it came from"},
    [LHAT_CHECK_ERR_FUNCTION_CLEANUP] = {"check.function-cleanup",
        "an f^ has nothing to clean up, so finally^ and with^ belong in a p^"},
    [LHAT_CHECK_ERR_FUNCTION_REQUIRES] = {"check.function-requires",
        "an f^ may not require^: requiring runs the unit's body, "
        "which nothing holds to an f^'s rules"},
    [LHAT_CHECK_ERR_BAD_TOSTRING] = {"check.bad-tostring",
        "tostring is an f^self^ -> string^, since a string hole calls "
        "it with no call written; this member is not one"},
    [LHAT_CHECK_ERR_PATH_IS_OPAQUE] = {"check.path-is-opaque",
        "this type carries what registered it, so what it holds "
        "cannot be written over"},
    [LHAT_CHECK_ERR_ADVANCES_OUTSIDE] = {"check.advances-outside",
        "an f^ may advance only a coroutine its own body made; "
        "this one came from somewhere else"},
    [LHAT_CHECK_ERR_MUTATES_OUTSIDE] = {"check.mutates-outside",
        "an f^ may write through mutable^self^ only into a table "
        "its own body made; this one came from somewhere else"},
    [LHAT_CHECK_ERR_ANSWER_NOT_FRESH] = {"check.answer-not-fresh",
        "a fresh^ answer has to be made by this body -- a "
        "literal, a new(), or another fresh^ call's answer"},
    [LHAT_CHECK_ERR_COROUTINE_ESCAPES] = {"check.coroutine-escapes",
        "an f^ coroutine may not leave the body that made it"},
    [LHAT_CHECK_ERR_TABLE_IS_SEALED] = {"check.table-is-sealed",
        "this table belongs to the machine; what it holds is "
        "written by the host, not from here"},
    [LHAT_CHECK_ERR_ASSIGN_TO_LET] = {"check.assign-to-let",
        "this name was bound by a let^ and is not reassigned; "
        "write var^ where the name has to change: {name}"},
    [LHAT_CHECK_ERR_ASSIGN_TO_FORM] = {"check.assign-to-form",
        "the construct that introduces this name is what gives it a "
        "value -- a with^ holds it for the block, a for^ advances "
        "or rebinds its focus -- so nothing else writes it: {name}"},
    [LHAT_CHECK_ERR_PUBLIC_IS_IMMUTABLE] = {"check.public-is-immutable",
        "a public^ declaration binds with let^; another unit would "
        "otherwise see a name change under it"},
    [LHAT_CHECK_ERR_SELF_TYPE_OUTSIDE] = {"check.self-type-outside",
        "Self^ names the t^ or def^ written around it, and there is "
        "no such literal here -- or a second hat counted past the "
        "outermost one"},
    [LHAT_CHECK_ERR_CATCHES_NOTHING] = {"check.catches-nothing",
        "nothing before these catch^ arms can fail: an error reaches "
        "them by being written try^, and none is"},
    [LHAT_CHECK_ERR_ARM_NEVER_RUNS] = {"check.arm-never-runs",
        "this catch^ arm never runs: nothing the try^ before it can find "
        "is left for it to take"},
    [LHAT_CHECK_ERR_HOSTVALUE_ESCAPES] = {"check.hostvalue-escapes",
        "a host value lives on the stack and nowhere else; box it "
        "into the container type its library provides to keep it"},
    [LHAT_CHECK_ERR_TUPLE_MISPLACED] = {"check.tuple-misplaced",
        "(A, B) is what a subroutine answers with, and it is "
        "written nowhere else -- not as an argument, a name, a "
        "table member, or a position of another tuple; pack^ makes "
        "a t^{ A, B } of one"},
    [LHAT_CHECK_ERR_HOSTVALUE_UNION] = {"check.hostvalue-union",
        "a host value takes several slots, so what stands beside "
        "it in a union has to be told apart by the first of them "
        "-- nil^ or an error, the same as beside (A, B)"},
    [LHAT_CHECK_ERR_TUPLE_UNION] = {"check.tuple-union",
        "(A, B) may be written in a union only with nil^ or an "
        "error, which the first slot tells apart"},
    [LHAT_CHECK_ERR_TUPLE_ARITY] = {"check.tuple-arity",
        "this answers a different number of values than there are "
        "names to take them"},
    [LHAT_CHECK_ERR_TUPLE_ERROR_POSITION] = {"check.tuple-error-position",
        "an error goes around the values, not among them; write "
        "(A, B)|SomeError rather than (A, SomeError)"},
};

// [l10n.compose.holes]: the codes reported both with a name and without one. Without, the
// text is CHECK_MESSAGES'; with, it is this table's -- an ID of its own, the
// name in a hole. A code only ever reported with a name holds the hole in its
// one text instead, so it has no row here.
static const LhatMessageEntry CHECK_NAMED_MESSAGES[] = {
    [LHAT_CHECK_ERR_PARAM_UNDECIDED] = {"check.param-undecided.named",
        "cannot infer the type of parameter '{name}'"},
    [LHAT_CHECK_ERR_NO_OPERATOR] = {"check.no-operator.named",
        "cannot apply operator '{name}' to left operand of type '{left}' and right operand of type '{right}'"},
    [LHAT_CHECK_ERR_MISMATCH] = {"check.mismatch.named",
        "this value does not fit where it is written: {member}"},
    [LHAT_CHECK_ERR_NO_MEMBER] =
        {"check.no-member.named", "this value has no such member: {member}"},
    [LHAT_CHECK_ERR_AMBIGUOUS_MEMBER] = {"check.ambiguous-member.named",
        "both sides of the composition carry this name, so it reaches no one "
        "answer; name the side you mean: {member}"},
    [LHAT_CHECK_ERR_NOT_SUBSTITUTABLE] = {"check.not-substitutable.named",
        "override^ has to be usable where the original was: {member}"},
};

// The codes reported with the two types they compare as well: the value's,
// and the one where it is written.
static const LhatMessageEntry CHECK_TYPED_MESSAGES[] = {
    [LHAT_CHECK_ERR_MISMATCH] = {"check.mismatch.typed",
        "this value does not fit where it is written: expected '{expected}', got '{actual}'"},
};

const char *lhat_check_error_message(LhatCheckErrorCode code)
{
    const LhatMessageEntry *entry = LHAT_MESSAGE_AT(CHECK_MESSAGES, code);
    return entry != NULL ? entry->text : "unknown error";
}

const char *lhat_check_error_id(LhatCheckErrorCode code)
{
    const LhatMessageEntry *entry = LHAT_MESSAGE_AT(CHECK_MESSAGES, code);
    return entry != NULL ? entry->id : NULL;
}

static const LhatMessageEntry *check_entry(
    const LhatCheckDiagnostic *diagnostic)
{
    const LhatMessageEntry *named =
        diagnostic->name != NULL
            ? LHAT_MESSAGE_AT(CHECK_NAMED_MESSAGES, diagnostic->code)
            : NULL;
    const LhatMessageEntry *typed =
        named == NULL && diagnostic->left_type != NULL &&
                diagnostic->right_type != NULL
            ? LHAT_MESSAGE_AT(CHECK_TYPED_MESSAGES, diagnostic->code)
            : NULL;
    return named != NULL ? named
         : typed != NULL ? typed
                         : LHAT_MESSAGE_AT(CHECK_MESSAGES, diagnostic->code);
}

LHAT_MESSAGE_TABLES(lhat_check_message_tables,
    {CHECK_MESSAGES, LHAT_MESSAGE_COUNT(CHECK_MESSAGES)},
    {CHECK_NAMED_MESSAGES, LHAT_MESSAGE_COUNT(CHECK_NAMED_MESSAGES)},
    {CHECK_TYPED_MESSAGES, LHAT_MESSAGE_COUNT(CHECK_TYPED_MESSAGES)})

const char *lhat_check_message_id(const LhatCheckDiagnostic *diagnostic)
{
    const LhatMessageEntry *entry =
        diagnostic != NULL ? check_entry(diagnostic) : NULL;
    return entry != NULL ? entry->id : NULL;
}

// What a diagnostic knows besides its code is one name, and the hole it goes
// into is named for what the name is -- a member, a field. The text says
// which, so the name is offered under each of them.
static const char *const NAME_HOLES[] = {"name", "member", "field",
                                         "annotation", "kind"};
#define NAME_HOLE_COUNT (sizeof NAME_HOLES / sizeof NAME_HOLES[0])

size_t lhat_check_message_write(const struct LhatProgram *program,
                                const LhatCheckDiagnostic *diagnostic,
                                char *out, size_t capacity)
{
    const LhatMessageEntry *entry =
        diagnostic != NULL ? check_entry(diagnostic) : NULL;
    LhatMessageArg args[NAME_HOLE_COUNT + 4];
    size_t count = 0;
    if (entry != NULL && diagnostic->name != NULL) {
        for (; count < NAME_HOLE_COUNT; count++) {
            args[count].name = NAME_HOLES[count];
            args[count].value = diagnostic->name;
            args[count].length = diagnostic->name_length;
        }
    }
    // Each type under both of the names a text may call it by.
    if (diagnostic != NULL && diagnostic->left_type != NULL) {
        size_t length = strlen(diagnostic->left_type);
        args[count++] = (LhatMessageArg){"left", diagnostic->left_type, length};
        args[count++] = (LhatMessageArg){"actual", diagnostic->left_type, length};
    }
    if (diagnostic != NULL && diagnostic->right_type != NULL) {
        size_t length = strlen(diagnostic->right_type);
        args[count++] = (LhatMessageArg){"right", diagnostic->right_type, length};
        args[count++] = (LhatMessageArg){"expected", diagnostic->right_type, length};
    }
    const char *text =
        entry != NULL ? lhat_program_text(program, entry->id, entry->text)
                      : "unknown error";
    size_t used = lhat_message_render(text, args, count, out, capacity);
    if (diagnostic != NULL && diagnostic->cause != NULL) {
        const LhatCheckDiagnostic *cause = diagnostic->cause;
        int added = snprintf(out != NULL && used < capacity ? out + used : NULL,
                             out != NULL && used < capacity ? capacity - used : 0,
                             " (%s:%u:%u: ",
                             diagnostic->cause_path != NULL ? diagnostic->cause_path : "",
                             (unsigned)cause->line, (unsigned)cause->column);
        if (added > 0) used += (size_t)added;
        used += lhat_check_message_write(program, cause,
                    out != NULL && used < capacity ? out + used : NULL,
                    out != NULL && used < capacity ? capacity - used : 0);
        if (out != NULL && used + 1 < capacity) out[used] = ')';
        used++;
        if (out != NULL && capacity != 0) out[used < capacity ? used : capacity - 1] = '\0';
    }
    return used;
}
