// L^ (lhat) -- syntax tree.
//
// Section numbers in the comments refer to DesignDocuments/02-syntax.md
// unless prefixed with "01", which means 01-lexical-structure.md.
//
// Nodes come from an arena and are never freed individually; disposing the
// arena frees the whole tree. Lists are singly linked through `next` rather
// than held in arrays, so a node can be appended without ever reallocating
// what an earlier node already points at.

#ifndef LHAT_AST_H
#define LHAT_AST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lhat/token.h"

typedef enum {
    // ---- expressions ----
    LHAT_NODE_INT,
    LHAT_NODE_FLOAT,
    LHAT_NODE_STRING,
    LHAT_NODE_NAME,          // [lex.ident.id-hat]: id^name -- the spelling as a value
    LHAT_NODE_IDENT,
    LHAT_NODE_HAT_IDENT,     // used as a value: true^, nil^, self^ ...
    // [syntax.for.it]: the focus for^ introduced when no name was written. Carries no
    // span, since 'it^' need not appear in the source for it to be there.
    LHAT_NODE_FOCUS,
    LHAT_NODE_SCOPE,         // $x, $$x, $^x  ([lex.scope]). Repeating '$'
                             // counts inwards from the unit and '^' outwards
                             // from here. The sigil is glued to the name and
                             // nothing may come between, so '$.x' is not
                             // another way of writing it
    LHAT_NODE_INTERP,        // $"..." -- list of INTERP_TEXT / INTERP_HOLE
    LHAT_NODE_INTERP_TEXT,
    LHAT_NODE_INTERP_HOLE,   // expression plus an optional format string
    LHAT_NODE_TABLE,         // { ... }
    LHAT_NODE_TABLE_ENTRY,   // key := value, or a positional value
    LHAT_NODE_TUPLE,         // (a, b)  ([syntax.type.tuple]). The value side of the type
                             // grammar's own rule: a ',' inside the
                             // parentheses makes a tuple, none leaves the
                             // grouping '(x)' was always read as
    LHAT_NODE_DEF,           // def^{ ... }                  ([syntax.obj])
    LHAT_NODE_SELF_TABLE,    // self^{ ... }                 ([syntax.obj.template], [syntax.obj.new])
    LHAT_NODE_ERROR_NEW,     // error^Kind{ ... }            ([errors.kind.new])
    LHAT_NODE_NEW,           // new^Type{ ... }; named.name is a type syntax
    LHAT_NODE_TRY,           // try^ expr                    ([errors.try])
    LHAT_NODE_TYPEOF,        // typeof^(expr)                ([syntax.obj.typeof])
    LHAT_NODE_TYPE_VALUE,    // type^ <written type>         ([syntax.type.as-expr])
    LHAT_NODE_SPREAD,        // expr...  as the last call argument ([syntax.type.variadic])
    LHAT_NODE_REQUIRE,       // require^ "path"              ([modules.require])
    LHAT_NODE_REQUIRE_STMT,  // require^ "path" on its own   ([modules.require.short])
    LHAT_NODE_IMPORT,        // import^ a.b.c                ([modules.host.import])
    LHAT_NODE_IMPORT_STMT,   // import^ a.b.c on its own     ([modules.host.import])
    LHAT_NODE_UNARY,
    LHAT_NODE_BINARY,
    LHAT_NODE_COMPARE_CHAIN, // a < b < c  ((5) of [syntax.op.precedence])
    LHAT_NODE_MEMBER,        // a.b   a?.b
    LHAT_NODE_INDEX,         // a[i]  a?[i]
    LHAT_NODE_CALL,          // f(x)  f?(x)
    LHAT_NODE_AS,            // expr as^ Type
    LHAT_NODE_PACK,          // pack^ expr            ([syntax.type.tuple]): the one bridge
                             // from a tuple to a table a name can hold
    LHAT_NODE_BOX,           // box^ expr             ([modules.hostvalue]): a host value
                             // put in the box the heap can hold
    LHAT_NODE_FUNC,          // f^... { ... } / p^... { ... }
    LHAT_NODE_IF_EXPR,       // if^c: e el^c: e el^: e ;

    // ---- statements ----
    LHAT_NODE_DEFINE,        // let^ a, b = x, y
    LHAT_NODE_REASSIGN,      // a, b := x, y
    LHAT_NODE_CALL_STMT,
    LHAT_NODE_BLOCK,         // do^{ ... } and any braced body
    LHAT_NODE_IF_STMT,
    LHAT_NODE_RETURN,
    LHAT_NODE_BREAK,
    LHAT_NODE_NEXT,          // next^ / skip^ / continue^          ([syntax.loop.next])
    LHAT_NODE_PANIC,         // panic^ expr                 ([errors.runtime.panic])
    LHAT_NODE_YIELD,
    LHAT_NODE_AWAIT,         // await^ expr                  ([syntax.sub.await], [syntax.sub.await-done])
    LHAT_NODE_WITH,          // with^ x = e  ... { ... }    ([syntax.with])
    LHAT_NODE_FOR,           // for^ ...                    ([syntax.for])
    LHAT_NODE_REPEAT,        // repeat^ ...                 ([syntax.for.repeat])
    LHAT_NODE_LOOP_CLAUSE,   // prolog^: first^: last^: ... ([lton.text])
    LHAT_NODE_ERRORDEF,      // errordef^ Name { ... }      ([errors.kind.errordef])
    LHAT_NODE_ERROR_KIND,    // one kind inside an errordef^
    LHAT_NODE_ENUMDEF,       // enum^ Name { ... }          ([syntax.enum])
    LHAT_NODE_ENUM_MEMBER,   // one member inside an enum^; `members` is its
                             // written value expression, NULL for the
                             // numbered kind
    LHAT_NODE_MODULE,        // module^ a.b.c               ([modules.unit])
    // [syntax.annot]: '@name' or '@name(a, b)'. `v.named.name` is the name it was
    // written with, `v.named.members` the arguments -- literals only, so
    // nothing here has to be run to be read.
    LHAT_NODE_ANNOTATION,

    // ---- pieces ----
    LHAT_NODE_IF_CLAUSE,     // condition (may be absent) plus a body
    LHAT_NODE_PARAM,         // name:type = default, or ...:type
    LHAT_NODE_MEMBER_DECL,   // name : type   inside t^{ ... }

    // ---- types ([errors.open]) ----
    LHAT_NODE_TYPE_NAME,     // number^, FooBar
    LHAT_NODE_TYPE_FUNC,     // f^A, B -> C;
    LHAT_NODE_TYPE_CORO,     // c^{ f^recv -> yield -> ret }  ([syntax.type.coroutine], [syntax.sub.yieldable-f])
    LHAT_NODE_TYPE_APPLY,    // nominal<arguments>; access target and argument list
    LHAT_NODE_TYPE_TABLE,    // t^{ member : type }
    LHAT_NODE_TYPE_TUPLE,    // (A, B)  ([syntax.type.tuple]). Two positions or more: '(T)'
                             // is the grouping the type grammar already had
    LHAT_NODE_TYPE_UNION,    // A | B
    LHAT_NODE_TYPE_INTERSECT,// A & B

    // What a parse function leaves where it could not build the construct.
    // [lsp.hover]: where the construct failed but an expression under it had
    // already been read, that expression stays in `v.jump.value` -- the
    // borrowed slot CALL_STMT and RETURN use, and NULL when there was
    // nothing to keep. Everything downstream refuses this node either way;
    // what the value buys is a tool that can still answer about the run
    // inside a statement the parser turned down (parser.c's `refused`).
    LHAT_NODE_ERROR,

    LHAT_NODE_KIND_COUNT
} LhatNodeKind;

// [syntax.for.not-loop]: what the clause after the focus does with it.
typedef enum {
    LHAT_FOR_TO,      // to^ limit [step^ n]
    LHAT_FOR_DOWNTO,  // downto^ limit [step^ n]
    LHAT_FOR_IN,      // in^ iterable
    LHAT_FOR_WHILE,   // while^ cond [next^ stmt]
    LHAT_FOR_UNTIL,   // until^ cond [next^ stmt]
    LHAT_FOR_IF,      // if^ cond -- not a loop at all ([syntax.for.forms])
    LHAT_FOR_WHEN,    // when^ patterns -- not a loop either ([syntax.match])
    LHAT_FOR_ONCE     // do^: expr, and the outer one of a run of for^ ([syntax.for.forms]):
                      // a focus with no clause to drive it, so the body is
                      // reached once
} LhatForKind;

// [syntax.for.repeat]: repeat^ carries no focus.
typedef enum {
    LHAT_REPEAT_FOREVER,
    LHAT_REPEAT_COUNT,
    LHAT_REPEAT_WHILE,
    LHAT_REPEAT_UNTIL
} LhatRepeatKind;

// [syntax.obj.override]. Marks a member that replaces or extends an inherited one; without
// a marker, a member of the same name is an error.
typedef enum {
    LHAT_DEF_PLAIN,
    LHAT_DEF_OVERRIDE,
    LHAT_DEF_OVERLOAD,
    LHAT_DEF_ABSTRACT,  // [syntax.obj.abstract]: declared, and left for a composition to give
    // [syntax.obj.delegate]: not a member at all, the way the template is not. What follows
    // is the name this definition delegates to -- 'self^.field' for one of
    // the template's, a bare name for one of the definition's own. The entry
    // carries that name in `value` and no key.
    LHAT_DEF_DELEGATE
} LhatDefModifier;

// [syntax.loop.order], in the order they must be written -- which is also the order they run
// in. [syntax.loop.pre]'s pre^ is the one that runs before the condition is tested, so it
// stands between prolog^ and first^.
typedef enum {
    LHAT_CLAUSE_PROLOG = 0,
    LHAT_CLAUSE_PRE = 1,
    LHAT_CLAUSE_FIRST = 2,
    LHAT_CLAUSE_MAIN = 3,
    LHAT_CLAUSE_LAST = 4,
    LHAT_CLAUSE_EPILOG = 5,
    LHAT_CLAUSE_FINALLY = 6
} LhatClauseKind;

// [syntax.loop.body]: the clauses that are the loop's body. At least one has to be there,
// and an unheaded statement list is an implicit main^.
#define LHAT_CLAUSE_IS_BODY(k)                                                \
    ((k) == LHAT_CLAUSE_PRE || (k) == LHAT_CLAUSE_FIRST ||                    \
     (k) == LHAT_CLAUSE_MAIN || (k) == LHAT_CLAUSE_LAST)

// 06§4.2: a stretch of the source text, for a tool that shows or rewrites
// it. A `length` of 0 means nothing was written there -- the construct bound
// the name itself, and there is no word to rewrite.
typedef struct {
    uint32_t offset;
    uint32_t length;
} LhatSpan;

// [syntax.sub.table-write]: an ownership depth with no bottom -- every level is the
// body's own. See LhatNode.checked_own and the checker's Binding.own.
#define LHAT_OWN_DEEP 255

typedef struct LhatNode LhatNode;

struct LhatNode {
    LhatNodeKind kind;

    // Where the construct starts, for diagnostics.
    uint32_t offset;
    uint32_t line;
    uint32_t column;

    // One past the last byte the construct consumed, so [offset, end) is the
    // whole of it. Diagnostics only ever needed the start, but a tool that
    // shows or replaces the source of one node needs the span (06§4.2).
    //
    // A node built from a single token gets this from that token. Anything
    // longer sets it on the way out of the construct, so a parse function
    // that forgets leaves a span that is too short rather than one that runs
    // past the construct -- and test_parser.c's span_encloses_children walk
    // finds the omission, since a parent that ends before a child does is
    // always a missed one.
    uint32_t end;

    LhatNode *next;  // next sibling when this node sits in a list

#if LHAT_WITH_COMMENTS
    // [lex.comment.kept]: the comments written against this node, in source order,
    // threaded through their own `next_for_node`. They live in the lexer's
    // table, so the lexer has to outlive the tree -- as it already must for
    // a string's bytes.
    //
    // Filled in by parser.c's attach_comments, once the whole unit is parsed
    // and the table has stopped growing.
    LhatComment *comments;
#endif

    // FUNC only: the whole signature infer_func (check.c) built for this
    // literal, an LhatType* from the checker's arena. void* here rather than
    // LhatType* so this parse-stage header need not depend on the check
    // stage's; compile_subroutine (vm.c) is the reader, and casts it back.
    // NULL until checking runs, and unset entirely when it never does.
    void *checked_type;
    bool checked_array_index; // Single-focus built-in table walk provides index^.
    bool checked_numeric; // BINARY: both operands number^, so no operator runs.
    // Independently checked bodies, owned by the semantic type arena.
    struct LhatFunctionInstance *checked_instances;
    const struct LhatFunctionInstance *checked_instance; // Selected concrete callee.
    uint64_t checked_transfer_arguments; // Host calls that snapshot argument captures.
    const struct LhatType *checked_callable_type; // Callable/coroutine provenance, also without tooling.
    // FITS binary / comparison-chain marker: operand type before narrowing.
    const struct LhatType *checked_fits_type;
    // Stable declaration identity for lexical bindings. Written by semantic
    // analysis even without tool resolutions; codegen maps it to storage.
    // Borrowed from the parsed trees, which outlive checking and compilation.
    const struct LhatNode *checked_binding;
    const struct LhatNode *checked_this_body; // Function literal named by this^/this^^.
    const struct LhatNode *checked_receiver; // Implicit self^ for super calls / self{...}.
    bool checked_super_call; // A direct super^ call, including static replacements.
    bool checked_scope_invalid; // Analysis rejected a scope specifier's reach.
    const struct LhatDefinition *checked_definition;
    const struct LhatTypeMember *checked_host_member; // Resolved member of L^.
    const struct LhatModuleRoot *checked_module_root;
    bool checked_import_global; // Read this root from L^.modules, not a capture.
    // A named type used where a type^ value is expected. Keep the original
    // name/member tree for tooling; compile this occurrence as a descriptor.
    void *descriptor_type;

#if LHAT_WITH_RESOLUTIONS
    // Tooling's value type, independent of the compiler's selective stamps.
    void *display_type;
#endif

    // [spec.arms]: which arm of an overloaded member ([syntax.obj.override]) the checker
    // settled on, as the index into that member's arms plus one -- zero means
    // it settled nothing, which is what an unchecked compile always sees.
    //
    // CALL: the candidate this call means, written only under strict, where
    // resolving is not optional (a call that fits no arm is a MISMATCH). The
    // compiler turns it into the PICKARM that spares the run the search.
    //
    // TABLE_ENTRY marked override^: the arm it replaces, written whatever the
    // strictness -- it decides the shape of the group rather than how a call
    // reads one, and [pipeline.repl.no-fork] keeps that the same either way.
    uint16_t checked_arm;
    // [syntax.sub.fresh]: this CALL's resolved callee promised '-> fresh^T', so what
    // it answers is something nobody held before -- which is what lets the
    // origin rules ([syntax.sub.mutable-self], [syntax.sub.yieldable-f]) count the answer as the body's own.
    // Stamped by the checker beside checked_arm.
    bool call_answers_fresh;
    // [syntax.sub.table-write]: TABLE, NEW and CALL -- how many levels of what this answers the
    // expression itself made: 1 for a table holding something that arrived,
    // LHAT_OWN_DEEP when everything it reaches was made here. A call counts
    // as a table built around its arguments, which is what a new() is.
    uint8_t checked_own;

    union {
        struct {
            uint64_t value;
            uint8_t base;
        } integer;

        LhatReal real;

        // STRING / INTERP_TEXT: bytes live in the lexer's decoded string
        // storage, so the lexer must outlive the tree.
        struct {
            uint32_t offset;
            uint32_t length;
            LhatStringKind kind;
        } string;

        // IDENT / HAT_IDENT / TYPE_NAME / NAME: a span of the source text.
        struct {
            uint32_t offset;
            uint32_t length;
            uint32_t hats;

            // [lex.ident.chars]: a name written with backticks is spelled by neither
            // the delimiters nor a doubled backtick, so its spelling is not
            // the span above -- the lexer decoded it into its string storage
            // and this is where. Zero length everywhere else, which is every
            // name that is its own source slice.
            uint32_t text_offset;
            uint32_t text_length;
        } name;

        // [lex.scope]: the sigil is glued to a name, so the two are one node.
        struct {
            LhatScopeKind kind;
            uint32_t depth;
            LhatNode *name;
        } scope;

        struct {
            LhatOpKind op;
            LhatNode *operand;
        } unary;

        struct {
            LhatOpKind op;
            LhatNode *left;
            LhatNode *right;
        } binary;

        // COMPARE_CHAIN: operands are a list of length n, operators a list of
        // length n-1 held as UNARY nodes carrying just the operator.
        struct {
            LhatNode *operands;
            LhatNode *operators;
        } chain;

        // MEMBER / INDEX / CALL. `nil_safe` marks the ?. ?[ ?( forms.
        struct {
            LhatNode *target;
            LhatNode *argument;  // member name, index expression, or arg list
            bool nil_safe;
            // [syntax.op.nil-safe-chain]: the last access of a postfix run that a '?' guarded.
            // What the run answers gains the nil^ arm here and nowhere else,
            // so an arm met before this one came from the writer's own type
            // and is refused as ever.
            bool nil_chain_end;
            // [syntax.type.return-type]: the checker read this MEMBER as a type spelling
            // (X.ReturnType) and stamped what it names on `argument`. The
            // compiler folds it into a descriptor constant, and a let^
            // binding it names a type.
            bool type_spelling;
        } access;

        struct {
            LhatNode *value;
            LhatNode *type;
        } ascription;

        // FUNC and TYPE_FUNC. `is_function` separates f^ from p^ ([syntax.sub]).
        struct {
            LhatNode *params;
            LhatNode *return_type;  // NULL when nothing is returned
            LhatNode *body;         // NULL for a type
            bool is_function;
            bool yields;            // [syntax.sub.yieldable]: inferred from the body
            bool is_template;
            // [syntax.sub.fresh]: written '-> fresh^T', so what a call answers is new
            // -- nothing could reach it before the call. A promise, written
            // on the result's seat.
            bool answers_fresh;
        } func;

        // PARAM. `variadic` marks the '...' form ([syntax.type.variadic]).
        struct {
            LhatNode *name;     // NULL for a variadic or an unnamed type slot
            LhatNode *type;     // NULL when not annotated
            LhatNode *fallback; // default value, NULL when absent
            bool variadic;
            // [syntax.sub.mutable-self]: written 'mutable^self^' -- the one parameter this may
            // decorate is the receiver marker, and it says the body writes
            // through it.
            bool mutable_receiver;
        } param;

        // TABLE_ENTRY / MEMBER_DECL. Inside a def^ the key is NULL when the
        // entry is the self^{ ... } template, whose value carries it.
        struct {
            LhatNode *key;    // NULL for a positional entry or the template
            LhatNode *value;
            LhatDefModifier modifier; // [syntax.obj.override], def^ entries only
            // [syntax.obj.keys-equals]: the key was written '[ ... ]', so it is an expression
            // rather than the name it would otherwise be read as.
            bool computed;
            // MEMBER_DECL: computed means '[key type]:value type'.
            // [syntax.obj.abstract]: written 'abstract^ name : type', so `value` is a type
            // and the definition provides nothing under the name. What is
            // composed onto it has to. Set on a template field too, where
            // [syntax.obj.override]'s modifier has no place.
            bool declared;
            // [syntax.obj.template] / [syntax.obj.keys] with [modules.host.machine]: a keyed entry written 'name : type = value'.
            // The type is what the field holds; the value is what [syntax.obj.new]
            // bakes onto the prototype. NULL when only a value was written,
            // and for positional entries.
            LhatNode *type;
            // [syntax.obj.structure]: MEMBER_DECL only. Written 'type[]' in a t^{ ... }
            // -- the unbounded run past the fixed positions. `value` is the
            // element type, `key` is unused.
            bool variadic;
            // [syntax.obj.structure-repeat]: MEMBER_DECL only, and only on a positional entry.
            // 'type[n]' is n positions of that type, which is what a run of
            // them written out means -- so this is a count of positions to
            // make, one without the brackets.
            uint32_t repeat;
            // [syntax.annot.where]: a def^ member and a self^{ … } field both take one.
            LhatNode *annotations;
        } entry;

        // DEFINE / REASSIGN take a list of targets and a list of values;
        // LET takes a list of targets and a single value ([syntax.type.destructure]).
        struct {
            LhatNode *targets;
            LhatNode *values;
            bool exported; // [modules.public]: written public^
            // [syntax.stmt.let]. DEFINE only: written let^ (or with^) rather than var^, so
            // the names it introduces refuse a later ':='. What the machine
            // does to a slot is not a write in this sense -- [syntax.for.direction]'s to^ and
            // downto^ advance their focus themselves, and [syntax.stmt.visible]'s LOADNIL seeds
            // one before its own definition runs; both leave the name still
            // one nothing written in the source may reassign.
            bool immutable;
            // [syntax.stmt.let] with [syntax.with.form] and [syntax.for.from]: the construct bound it, not a word
            // the writer chose -- a with^, or the focus of a counted for^.
            // There is no var^ to write instead, so the diagnostic for a ':='
            // on one of these must not offer it.
            bool bound_by_form;
            // [modules.hostdata.base]. DEFINE only, and only from let^: written ':=' rather
            // than '='. For a plain name this changes nothing ([modules.host.machine] already
            // makes the two spellings the same there); for a path target
            // whose last segment already exists it makes this a reassignment
            // instead of the redefinition error [syntax.stmt.visible] would otherwise report.
            bool via_reassign_op;
            // [modules.label.typeof]. REASSIGN only, and only from 'target op= value': the
            // plain operator the compound spelling stands for (e.g. '+=' ->
            // LHAT_OP_ADD). `values` holds just the right-hand side, not the
            // expanded 'target op value' -- target is evaluated once, and
            // where that matters (a path target) is a checker/compiler
            // concern, not a parse-time expansion.
            bool has_compound_op;
            LhatOpKind compound_op;
            // [syntax.stmt.nil-assign]. The '?' spellings: an absent place is left as it is,
            // and the right-hand side is not evaluated either. Set on '?:='
            // too, where `has_compound_op` above is false -- a plain
            // assignment stands for no operator, and what the '?' adds is
            // the same thing it adds to the eight. Whether the place is
            // written is all the two forms share, so nothing else here has
            // to tell them apart.
            bool compound_nil_safe;
            // [syntax.annot.where]: what was written above this declaration. NULL when
            // nothing was. A list of LHAT_NODE_ANNOTATION.
            LhatNode *annotations;
            // [syntax.stmt.let]: where the let^ or var^ itself stands. The node begins at
            // the word only when nothing was written before it -- a public^
            // takes the start over ([modules.public]) -- and [syntax.with.form]'s with^ and
            // [syntax.for.from]'s focus are bound by their construct, with no word to
            // point at. Empty in both of those, so [lsp.next]'s fixes can tell.
            LhatSpan keyword;
        } binding;

        // Statement or expression lists: BLOCK, TABLE, IF_STMT, IF_EXPR,
        // INTERP, CALL argument lists, WITH bindings.
        //
        // For BLOCK, `items` holds the unlabelled statements, which are the
        // main clause, and `extra` holds the other clauses of [lton.text] as a list
        // of LHAT_NODE_LOOP_CLAUSE. A block with no clause markers keeps
        // `extra` empty, so the common case is unchanged.
        struct {
            LhatNode *items;
            // WITH: body. BLOCK: clause list. IF_STMT: its finally^ (02
            // [syntax.finally.block]), the one clause it may carry.
            LhatNode *extra;
            // [errors.catch.clause]: BLOCK and IF_STMT -- the catch^ arms written at the
            // end of the braces, as IF_CLAUSE nodes whose `condition` is the
            // written type (NULL on the bare one, which takes what is left).
            // A try^ in the block's main^, or in any of the if^'s bodies,
            // leaves for them. NULL when none was written.
            LhatNode *arms;
            // [syntax.annot.where]: the unit's own, written at its head. Only the root
            // block of a unit ever carries these.
            LhatNode *annotations;
            // [syntax.sub.yieldable] with [modules.unit.optional]: the unit's own top level holds a
            // yield^ or an await^ -- a script's 'p^...' is then yieldable.
            // Only the root block of a unit ever sets it.
            bool yields;
        } list;

        // [syntax.for.forms]. `focus` is a list: bindings, destructuring targets, or one
        // expression when the focus is unnamed and reached through it^.
        struct {
            LhatForKind kind;
            LhatNode *focus;
            LhatNode *bound;    // limit, iterable or condition
            LhatNode *step;     // step^, absent for every kind but to^/downto^
            LhatNode *advance;  // next^ statements
            LhatNode *body;
            // [syntax.if.stmt] and [syntax.match.form]: written in the form opened by ':' rather than
            // by '{', so it answers a value. Only if^ and the when^ clauses
            // have that form; the driving clauses iterate and answer nothing.
            bool is_expression;
        } loop;

        struct {
            LhatRepeatKind kind;
            LhatNode *bound;  // count or condition, absent when forever
            LhatNode *body;
        } repeat;

        struct {
            LhatClauseKind kind;
            LhatNode *body;
        } loop_clause;

        // ERRORDEF / ERROR_KIND / ERROR_NEW ([errors.kind.errordef], [errors.kind.new]). A name and the
        // list it introduces: kinds for a declaration, MEMBER_DECL fields for
        // a kind -- the same shape t^{ ... } already uses -- and TABLE_ENTRY
        // values for a construction. ERROR_NEW's name is a qualified path;
        // NEW's name holds the complete type syntax.
        struct {
            LhatNode *name;
            LhatNode *members;  // NULL when a kind declares no fields
            bool exported;      // [modules.public]: written public^
            // [errors.kind.local]: ERRORDEF only -- written localerrordef^, so what it
            // declares is under localerror^ rather than under error^. The
            // declaration is the unit, not the kind: the two tops are
            // disjoint, and one set's kinds cannot span both.
            bool local;
        } named;

        // IF_CLAUSE. `condition` is NULL for the final else.
        struct {
            LhatNode *condition;
            LhatNode *body;
        } clause;

        // INTERP_HOLE. `format` is the raw text after ':' ([lex.string.interp]).
        struct {
            LhatNode *value;
            LhatNode *format;
        } hole;

        // [syntax.loop.exit]: how many loops a break^ leaves, counting the innermost
        // as 1. Written either as the hats on the word (break^^^) or in
        // brackets after it (break^[3]); the parser settles the two into
        // this. Unused by the other jumps.
        //
        // BREAK only: `value` holds what was bracketed when it was not a
        // count -- [syntax.loop.exit]'s label form, which nothing labels a loop for yet.
        //
        // RETURN / YIELD carry an optional value; BREAK carries a level;
        // PANIC's value is required (`phantom` unused).
        struct {
            LhatNode *value;
            // YIELD only. [syntax.sub.phantom-yield]: written '_yield^', which types exactly as a
            // yield^ does and makes the body yieldable, but never suspends.
            bool phantom;
            // BOX only. [modules.hostvalue.union]: written 'constbox^', which makes the
            // sealed, get-only box -- the keyable one.
            bool sealing;
            // BREAK: the number of loops to leave, and never zero -- a plain
            // break^ is one.
            //
            // RETURN / YIELD ([syntax.type.tuple]): how many values are written, when
            // several are. They hang off `value` as a list. 0 and 1 both mean
            // one, which is every return^ and yield^ written before tuples.
            uint32_t level;
            // RETURN only. [syntax.sub.expr-body]: made by the parser out of a body that is
            // one expression, rather than written return^. [syntax.obj.new] reads
            // it: a new body answers nothing of its own, so a written
            // return^ is refused there -- and a sole expression is not one.
            bool implicit;
        } jump;

        // TYPE_CORO: the three types of [syntax.type.coroutine]. A NULL `receive` or `result` is
        // an empty slot, written by leaving it out -- "nothing is sent in",
        // "the body ends without a value". `endless` is the third slot's other
        // absence, written '-': a body that cannot end has no last resume for
        // a result to be the type of, which is a different statement from
        // ending with nothing.
        struct {
            LhatNode *receive;
            LhatNode *produce;
            LhatNode *result;
            bool receive_any, produce_any, result_any, kind_any;
            bool endless;
            // [syntax.sub.yieldable-f]: which kind of body it came from, written as the front
            // half of [syntax.type.coroutine]'s form ('c^{ f^R -> Y -> T }'). What may advance a
            // coroutine, and where it may be held, follows from this.
            bool is_function;
        } coroutine;
    } v;
};

// Arena holding every node of one tree.
typedef struct LhatArenaBlock LhatArenaBlock;

typedef struct {
    LhatArenaBlock *blocks;
    size_t node_count;
} LhatAstArena;

void lhat_arena_init(LhatAstArena *arena);
void lhat_arena_dispose(LhatAstArena *arena);

// Returns NULL only when out of memory.
LhatNode *lhat_node_new(LhatAstArena *arena, LhatNodeKind kind,
                        const LhatToken *at);

const char *lhat_node_kind_name(LhatNodeKind kind);

// Appends to a list built from `next` pointers. `head` and `tail` are the
// caller's cursors; passing the tail avoids walking the list every time.
void lhat_node_append(LhatNode **head, LhatNode **tail, LhatNode *node);

size_t lhat_node_list_length(const LhatNode *head);

// Calls `visit` once per child, in source order, for every kind of node.
//
// Nothing is visited twice and nothing is skipped, so a caller can walk the
// whole tree without a case of its own per kind -- which is what a tool that
// renders or serialises the tree wants, unlike semantic_tokens.c, whose whole
// job is to treat each kind differently.
//
// `field` names the place the child came from, spelled as the union member in
// this header ("left", "items", "targets"), so a serialised tree can be read
// back without knowing the order each kind writes its children in. `in_list`
// says that place holds a list rather than a single child -- true even when
// the list happens to hold one, so a reader can shape it the same way every
// time.
typedef void (*LhatNodeVisitor)(void *context, const char *field, bool in_list,
                                const LhatNode *child);
void lhat_node_visit_children(const LhatNode *node, LhatNodeVisitor visit,
                              void *context);

// Where the construct actually begins: the least offset in the subtree.
//
// Not the node's own offset. An infix or postfix node is written starting at
// its operator -- the '+' of 'a + b', the '(' of 'f(1)' -- with the left side
// hanging below it, so [lhat_node_span_start(n), n->end) is the whole of the
// construct while [n->offset, n->end) is only the part from the operator on.
uint32_t lhat_node_span_start(const LhatNode *node);

// [lex.comment.disabled]: a block comment whose body opens with '~' -- '#[~ ... ]#' -- is
// code switched off rather than prose. The language reads it as the comment
// it lexes as; the tools read what it holds as statements. Only a closed one
// counts, since an unterminated comment has no ']#' to take away.
#if LHAT_WITH_COMMENTS
bool lhat_comment_is_disabled_code(const LhatComment *comment,
                                   const char *source, size_t source_length);
#endif

// [lex.comment.kept]: the comment block written directly above this node -- what the
// language calls the thing's description, with the markers stripped so it
// reads as prose and the lines joined by newlines.
//
// A blank line ends the block, which is Go's rule and the one [lex.comment.kept] names: a
// comment with a gap under it was written about whatever came before the
// gap. A comment left at the end of the construct's own last line is not
// part of it either -- split_comments hands a node both, and only what
// stands above lhat_node_span_start is what it says about itself.
//
// Follows lhat_report_write: answers how many bytes it wants, not counting
// the terminating NUL, and fills up to `capacity` including it. So measuring
// is a call with (NULL, 0). Answers 0 where comments were not kept
// (LHAT_WITH_COMMENTS), so no caller has to ask whether they were.
size_t lhat_node_documentation(const LhatNode *node, const char *source,
                               size_t source_length, char *out,
                               size_t capacity);

// [lex.hat.repeat]: the hat is part of the name. A hat identifier's name is the
// word plus one hat -- 'self^' is a different name from 'self' -- and a
// spelling with more hats than one is that same name reached further out:
// this^^^ is the this^ two levels up, its name still 'this^' with the count
// kept on the node (v.name.hats) for the constructs that stack. What this
// answers is that canonical spelling, cut from `source_text` (the text the
// node's offsets index into). The checker looks a member up under it and
// the compiler uses it as a key, so one definition is what keeps the two
// agreeing byte for byte. The operator name in op^:: is canonicalized to
// the literal ".."; its source offsets still describe the written spelling.
//
// [l10n.scope.shown]: a name written with backticks is spelled in `strings` instead -- the
// lexer's decoded storage, since the delimiters are not part of the name.
// Both bases are handed over and the node says which one it is spelled in.
bool lhat_node_name(const LhatNode *node, const char *source_text,
                    const char *strings, const char **text, size_t *length);

bool lhat_name_is(const char *text, size_t length, const char *literal);

// [syntax.obj.number-consts]: the constant number^ carries under `name` as a static
// member (inf, nan), or NULL. The checker asks whether there is
// one and the compiler what it is, off this one table.
const LhatReal *lhat_number_constant(const char *name, size_t length);

// The same table read as a list: the name at `index`, or NULL past the end.
// [lsp.hover] lists them, since 'number^' is a word rather than a type and
// nothing else can be asked what it carries.
const char *lhat_number_constant_at(size_t index, size_t *length);

// [modules.host.machine]: L^ names the machine's own table. Only the hatted spelling
// means it, so an ordinary name `L` is untouched.
bool lhat_node_is_environment(const LhatNode *node, const char *source_text,
                              const char *strings);

// The name a let^ target carries, whether or not it was annotated.
const LhatNode *lhat_define_target_name(const LhatNode *target);

// [modules.hostdata]: 'let^ a.b.c = v' introduces c inside a table reached through a and
// b. Only the root is a name a scope holds; the rest are members.
const LhatNode *lhat_define_target_root(const LhatNode *target);
bool lhat_define_target_is_path(const LhatNode *target);

#endif  // LHAT_AST_H
