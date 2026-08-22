// L^ (lhat) -- the regular-expression engine behind std.regex.
//
// Two halves: a recursive-descent compiler from the pattern text to a node
// tree, and a backtracking matcher that walks the tree with an explicit
// continuation chain. The continuations live on the C stack of the matcher's
// own recursion, and an explicit depth counter is what keeps a pathological
// pattern from running that stack out -- the cap is generous for real
// patterns and the overrun is reported, never felt.

#include "regex_engine.h"

#include <stdint.h>
#include <string.h>

#include "lhat/port.h"

// ---------------------------------------------------------------------------
// The tree
// ---------------------------------------------------------------------------

typedef enum {
    NODE_LIT,       // a run of literal bytes
    NODE_ANY,       // . -- one code point
    NODE_CLASS,     // [...] -- code-point ranges, possibly negated
    NODE_SEQ,       // children in a row
    NODE_ALT,       // children as choices, first match wins
    NODE_REPEAT,    // child, min..max times
    NODE_GROUP,     // capturing (index 1..15) or plain (index 0)
    NODE_BOL,       // ^
    NODE_EOL        // $
} NodeKind;

typedef struct {
    unsigned lo;
    unsigned hi;
} ClassRange;

typedef struct Node {
    NodeKind kind;
    struct Node *next;      // the SEQ/ALT sibling chain

    // LIT
    const char *bytes;      // aimed into the arena's copy of the pattern
    size_t byte_count;

    // CLASS
    ClassRange *ranges;
    size_t range_count;
    bool negated;

    // SEQ / ALT / REPEAT / GROUP
    struct Node *child;     // first child, chained by ->next

    // REPEAT
    size_t min;
    size_t max;             // SIZE_MAX for an open end
    bool lazy;

    // GROUP
    size_t group;           // 0 for (?:...)
} Node;

struct LhatRegex {
    Node *root;
    size_t groups;          // written groups
    char *pattern_copy;     // LIT bytes aim into this
    // A simple arena: every node and range array allocated during the
    // compile, freed in one sweep.
    void **held;
    size_t held_count;
    size_t held_capacity;
};

// ---------------------------------------------------------------------------
// The compiler
// ---------------------------------------------------------------------------

typedef struct {
    LhatRegex *out;
    const char *pattern;
    size_t length;
    size_t at;
    const char *error;
    size_t error_at;
} Compiler;

static void *arena_alloc(Compiler *c, size_t size)
{
    void *taken = lhat_alloc(size);
    if (taken == NULL) {
        return NULL;
    }
    if (c->out->held_count == c->out->held_capacity) {
        size_t wider = c->out->held_capacity ? c->out->held_capacity * 2 : 16;
        void **grown = (void **)lhat_alloc(wider * sizeof *grown);
        if (grown == NULL) {
            lhat_free(taken);
            return NULL;
        }
        memcpy(grown, c->out->held, c->out->held_count * sizeof *grown);
        lhat_free(c->out->held);
        c->out->held = grown;
        c->out->held_capacity = wider;
    }
    c->out->held[c->out->held_count++] = taken;
    return taken;
}

static Node *node(Compiler *c, NodeKind kind)
{
    Node *made = (Node *)arena_alloc(c, sizeof *made);
    if (made != NULL) {
        memset(made, 0, sizeof *made);
        made->kind = kind;
    }
    return made;
}

static bool fail(Compiler *c, const char *why)
{
    if (c->error == NULL) {
        c->error = why;
        c->error_at = c->at;
    }
    return false;
}

static bool at_end(const Compiler *c) { return c->at >= c->length; }
static char peek(const Compiler *c)
{
    return c->at < c->length ? c->pattern[c->at] : '\0';
}

// One code point off the pattern (or the text), by the lead byte's shape.
// Malformed sequences read as single bytes, which keeps every position
// reachable rather than making a bad byte invisible.
static unsigned decode(const char *text, size_t length, size_t at,
                       size_t *width)
{
    unsigned char lead = (unsigned char)text[at];
    size_t need = lead < 0x80 ? 1 : (lead & 0xE0) == 0xC0 ? 2
                             : (lead & 0xF0) == 0xE0     ? 3
                             : (lead & 0xF8) == 0xF0     ? 4
                                                         : 1;
    if (need == 1 || at + need > length) {
        *width = 1;
        return lead;
    }
    unsigned point = lead & (unsigned)(0x7F >> need);
    for (size_t i = 1; i < need; i++) {
        unsigned char cont = (unsigned char)text[at + i];
        if ((cont & 0xC0) != 0x80) {
            *width = 1;
            return lead;
        }
        point = (point << 6) | (cont & 0x3Fu);
    }
    *width = need;
    return point;
}

static Node *parse_alternation(Compiler *c, int depth);

// The predefined classes, shared by \d and [\d].
static bool class_ranges_for(Compiler *c, char which, Node *into)
{
    static const ClassRange digits[] = { { '0', '9' } };
    static const ClassRange words[] = {
        { '0', '9' }, { 'A', 'Z' }, { '_', '_' }, { 'a', 'z' }
    };
    static const ClassRange spaces[] = {
        { '\t', '\r' }, { ' ', ' ' }
    };
    const ClassRange *from = NULL;
    size_t count = 0;
    switch (which) {
        case 'd': case 'D': from = digits; count = 1; break;
        case 'w': case 'W': from = words;  count = 4; break;
        case 's': case 'S': from = spaces; count = 2; break;
        default: return false;
    }
    ClassRange *copy =
        (ClassRange *)arena_alloc(c, count * sizeof *copy);
    if (copy == NULL) {
        return fail(c, "out of memory");
    }
    memcpy(copy, from, count * sizeof *copy);
    into->ranges = copy;
    into->range_count = count;
    into->negated = which == 'D' || which == 'W' || which == 'S';
    return true;
}

// The escapes that stand for one literal byte.
static bool escape_literal(char written, char *byte)
{
    switch (written) {
        case 'n': *byte = '\n'; return true;
        case 't': *byte = '\t'; return true;
        case 'r': *byte = '\r'; return true;
        case 'f': *byte = '\f'; return true;
        case 'v': *byte = '\v'; return true;
        default:
            // Any punctuation escaped is itself -- \. \* \( and the rest.
            if ((unsigned char)written < 0x80 &&
                !((written >= '0' && written <= '9') ||
                  (written >= 'a' && written <= 'z') ||
                  (written >= 'A' && written <= 'Z'))) {
                *byte = written;
                return true;
            }
            return false;
    }
}

// [...] -- ranges over code points, so a non-ASCII literal works in one.
static Node *parse_class(Compiler *c)
{
    Node *made = node(c, NODE_CLASS);
    if (made == NULL) {
        fail(c, "out of memory");
        return NULL;
    }
    c->at++;  // [
    if (peek(c) == '^') {
        made->negated = true;
        c->at++;
    }
    ClassRange gathered[64];
    size_t count = 0;
    bool first = true;
    while (!at_end(c) && (peek(c) != ']' || first)) {
        first = false;
        if (count >= sizeof gathered / sizeof gathered[0]) {
            fail(c, "the class holds too many ranges");
            return NULL;
        }
        unsigned lo;
        if (peek(c) == '\\') {
            c->at++;
            if (at_end(c)) {
                fail(c, "the pattern ends inside an escape");
                return NULL;
            }
            char which = peek(c);
            char literal = 0;
            if (which == 'd' || which == 'D' || which == 'w' ||
                which == 'W' || which == 's' || which == 'S') {
                // A predefined class inside [...]: its ranges join the set.
                // The negated forms would need subtraction, which one flat
                // list cannot say.
                if (which == 'D' || which == 'W' || which == 'S') {
                    fail(c, "a negated escape cannot stand inside [...]");
                    return NULL;
                }
                Node scratch;
                memset(&scratch, 0, sizeof scratch);
                if (!class_ranges_for(c, which, &scratch)) {
                    return NULL;
                }
                for (size_t i = 0; i < scratch.range_count; i++) {
                    if (count >= sizeof gathered / sizeof gathered[0]) {
                        fail(c, "the class holds too many ranges");
                        return NULL;
                    }
                    gathered[count++] = scratch.ranges[i];
                }
                c->at++;
                continue;
            }
            if (!escape_literal(which, &literal)) {
                fail(c, "not an escape the dialect has");
                return NULL;
            }
            lo = (unsigned char)literal;
            c->at++;
        } else {
            size_t width = 0;
            lo = decode(c->pattern, c->length, c->at, &width);
            c->at += width;
        }
        unsigned hi = lo;
        if (peek(c) == '-' && c->at + 1 < c->length &&
            c->pattern[c->at + 1] != ']') {
            c->at++;  // -
            if (peek(c) == '\\') {
                c->at++;
                char literal = 0;
                if (at_end(c) || !escape_literal(peek(c), &literal)) {
                    fail(c, "not an escape the dialect has");
                    return NULL;
                }
                hi = (unsigned char)literal;
                c->at++;
            } else {
                size_t width = 0;
                hi = decode(c->pattern, c->length, c->at, &width);
                c->at += width;
            }
            if (hi < lo) {
                fail(c, "the range runs backwards");
                return NULL;
            }
        }
        gathered[count].lo = lo;
        gathered[count].hi = hi;
        count++;
    }
    if (at_end(c)) {
        fail(c, "the pattern ends inside [...]");
        return NULL;
    }
    c->at++;  // ]
    made->ranges = (ClassRange *)arena_alloc(c, count * sizeof *made->ranges);
    if (made->ranges == NULL) {
        fail(c, "out of memory");
        return NULL;
    }
    memcpy(made->ranges, gathered, count * sizeof *made->ranges);
    made->range_count = count;
    return made;
}

// {n} {n,} {n,m} -- answered through *min/*max; false when the braces are
// not a quantifier at all (regex treats a stray '{' as a literal, and so
// does this dialect).
static bool parse_braces(Compiler *c, size_t *min, size_t *max)
{
    size_t look = c->at + 1;  // past {
    size_t lo = 0;
    bool any = false;
    while (look < c->length && c->pattern[look] >= '0' &&
           c->pattern[look] <= '9') {
        lo = lo * 10 + (size_t)(c->pattern[look] - '0');
        if (lo > 4096) {
            fail(c, "the repeat count is past reason");
            return false;
        }
        look++;
        any = true;
    }
    if (!any) {
        return false;  // '{' with no digits: a literal brace
    }
    size_t hi = lo;
    if (look < c->length && c->pattern[look] == ',') {
        look++;
        hi = SIZE_MAX;
        size_t written = 0;
        bool bounded = false;
        while (look < c->length && c->pattern[look] >= '0' &&
               c->pattern[look] <= '9') {
            written = written * 10 + (size_t)(c->pattern[look] - '0');
            if (written > 4096) {
                fail(c, "the repeat count is past reason");
                return false;
            }
            look++;
            bounded = true;
        }
        if (bounded) {
            hi = written;
        }
    }
    if (look >= c->length || c->pattern[look] != '}') {
        return false;  // still a literal brace
    }
    if (hi != SIZE_MAX && hi < lo) {
        fail(c, "the repeat range runs backwards");
        return false;
    }
    *min = lo;
    *max = hi;
    c->at = look + 1;
    return true;
}

static Node *parse_atom(Compiler *c, int depth)
{
    char head = peek(c);
    if (head == '*' || head == '+' || head == '?') {
        fail(c, "the repeat has nothing before it");
        return NULL;
    }
    if (head == '(') {
        c->at++;
        Node *made = node(c, NODE_GROUP);
        if (made == NULL) {
            fail(c, "out of memory");
            return NULL;
        }
        if (peek(c) == '?') {
            if (c->at + 1 < c->length && c->pattern[c->at + 1] == ':') {
                c->at += 2;  // (?:  -- grouping alone
            } else {
                fail(c, "only (?: is taken after '(?'");
                return NULL;
            }
        } else {
            if (c->out->groups + 1 >= LHAT_REGEX_MAX_GROUPS) {
                fail(c, "the pattern holds too many groups");
                return NULL;
            }
            made->group = ++c->out->groups;
        }
        made->child = parse_alternation(c, depth + 1);
        if (c->error != NULL) {
            return NULL;
        }
        if (peek(c) != ')') {
            fail(c, "the pattern ends inside (...)");
            return NULL;
        }
        c->at++;
        return made;
    }
    if (head == '[') {
        return parse_class(c);
    }
    if (head == '.') {
        c->at++;
        return node(c, NODE_ANY);
    }
    if (head == '^') {
        c->at++;
        return node(c, NODE_BOL);
    }
    if (head == '$') {
        c->at++;
        return node(c, NODE_EOL);
    }
    if (head == '\\') {
        if (c->at + 1 >= c->length) {
            fail(c, "the pattern ends inside an escape");
            return NULL;
        }
        char which = c->pattern[c->at + 1];
        if (which == 'd' || which == 'D' || which == 'w' || which == 'W' ||
            which == 's' || which == 'S') {
            Node *made = node(c, NODE_CLASS);
            if (made == NULL || !class_ranges_for(c, which, made)) {
                if (made != NULL && c->error == NULL) {
                    fail(c, "out of memory");
                }
                return NULL;
            }
            c->at += 2;
            return made;
        }
        char literal = 0;
        if (!escape_literal(which, &literal)) {
            fail(c, "not an escape the dialect has");
            return NULL;
        }
        Node *made = node(c, NODE_LIT);
        if (made == NULL) {
            fail(c, "out of memory");
            return NULL;
        }
        char *copy = (char *)arena_alloc(c, 1);
        if (copy == NULL) {
            fail(c, "out of memory");
            return NULL;
        }
        copy[0] = literal;
        made->bytes = copy;
        made->byte_count = 1;
        c->at += 2;
        return made;
    }
    // A literal: one code point's worth of the pattern's own copy.
    size_t width = 0;
    decode(c->pattern, c->length, c->at, &width);
    Node *made = node(c, NODE_LIT);
    if (made == NULL) {
        fail(c, "out of memory");
        return NULL;
    }
    made->bytes = c->out->pattern_copy + c->at;
    made->byte_count = width;
    c->at += width;
    return made;
}

static Node *parse_repeat(Compiler *c, int depth)
{
    Node *atom = parse_atom(c, depth);
    if (atom == NULL) {
        return NULL;
    }
    size_t min = 0;
    size_t max = 0;
    char head = peek(c);
    if (head == '*') {
        min = 0;
        max = SIZE_MAX;
        c->at++;
    } else if (head == '+') {
        min = 1;
        max = SIZE_MAX;
        c->at++;
    } else if (head == '?') {
        min = 0;
        max = 1;
        c->at++;
    } else if (head == '{') {
        if (!parse_braces(c, &min, &max)) {
            return c->error == NULL ? atom : NULL;
        }
    } else {
        return atom;
    }
    if (atom->kind == NODE_BOL || atom->kind == NODE_EOL) {
        fail(c, "an anchor repeats nothing");
        return NULL;
    }
    Node *made = node(c, NODE_REPEAT);
    if (made == NULL) {
        fail(c, "out of memory");
        return NULL;
    }
    made->child = atom;
    made->min = min;
    made->max = max;
    if (peek(c) == '?') {
        made->lazy = true;
        c->at++;
    }
    return made;
}

static Node *parse_sequence(Compiler *c, int depth)
{
    Node *made = node(c, NODE_SEQ);
    if (made == NULL) {
        fail(c, "out of memory");
        return NULL;
    }
    Node *tail = NULL;
    while (!at_end(c) && peek(c) != '|' && peek(c) != ')') {
        Node *piece = parse_repeat(c, depth);
        if (piece == NULL) {
            return NULL;
        }
        if (tail == NULL) {
            made->child = piece;
        } else {
            tail->next = piece;
        }
        tail = piece;
    }
    return made;
}

static Node *parse_alternation(Compiler *c, int depth)
{
    if (depth > 64) {
        fail(c, "the pattern nests past reason");
        return NULL;
    }
    Node *first = parse_sequence(c, depth);
    if (first == NULL) {
        return NULL;
    }
    if (peek(c) != '|') {
        return first;
    }
    Node *made = node(c, NODE_ALT);
    if (made == NULL) {
        fail(c, "out of memory");
        return NULL;
    }
    made->child = first;
    Node *tail = first;
    while (peek(c) == '|') {
        c->at++;
        Node *choice = parse_sequence(c, depth);
        if (choice == NULL) {
            return NULL;
        }
        tail->next = choice;
        tail = choice;
    }
    return made;
}

LhatRegex *lhat_regex_compile(const char *pattern, size_t length,
                              const char **error, size_t *error_at)
{
    if (error != NULL) {
        *error = NULL;
    }
    if (error_at != NULL) {
        *error_at = 0;
    }
    LhatRegex *made = (LhatRegex *)lhat_alloc(sizeof *made);
    if (made == NULL) {
        if (error != NULL) {
            *error = "out of memory";
        }
        return NULL;
    }
    memset(made, 0, sizeof *made);
    made->pattern_copy = (char *)lhat_alloc(length ? length : 1);
    if (made->pattern_copy == NULL) {
        lhat_free(made);
        if (error != NULL) {
            *error = "out of memory";
        }
        return NULL;
    }
    memcpy(made->pattern_copy, pattern, length);

    Compiler c;
    memset(&c, 0, sizeof c);
    c.out = made;
    c.pattern = made->pattern_copy;
    c.length = length;

    made->root = parse_alternation(&c, 0);
    if (made->root != NULL && !at_end(&c)) {
        // A ')' with no '(' is the one way to stop early without an error.
        fail(&c, "a ')' closes nothing");
    }
    if (c.error != NULL || made->root == NULL) {
        if (error != NULL) {
            *error = c.error != NULL ? c.error : "out of memory";
        }
        if (error_at != NULL) {
            *error_at = c.error_at;
        }
        lhat_regex_free(made);
        return NULL;
    }
    return made;
}

void lhat_regex_free(LhatRegex *regex)
{
    if (regex == NULL) {
        return;
    }
    for (size_t i = 0; i < regex->held_count; i++) {
        lhat_free(regex->held[i]);
    }
    lhat_free(regex->held);
    lhat_free(regex->pattern_copy);
    lhat_free(regex);
}

size_t lhat_regex_groups(const LhatRegex *regex)
{
    return regex != NULL ? regex->groups : 0;
}

// ---------------------------------------------------------------------------
// The matcher
// ---------------------------------------------------------------------------

// What comes after the current position in the walk. A plain node list, the
// closing of a group, or another round of a repeat -- told apart by kind, so
// no synthesized node ever has to impersonate a real one. The links live on
// the matcher's own C stack.
typedef enum {
    CONT_LIST,       // go on with `list` (a sibling chain), then `rest`
    CONT_GROUP_END,  // record group `index`'s end, then `rest`
    CONT_REPEAT      // another round of `repeat`, `done` rounds in
} ContKind;

typedef struct Cont {
    ContKind kind;
    const Node *list;
    size_t index;
    const Node *repeat;
    size_t done;
    size_t entered_at;   // where the round began; an empty round stops
    const struct Cont *rest;
} Cont;

typedef struct {
    const char *text;
    size_t length;
    LhatRegexSpan *spans;
    size_t depth;
    size_t steps;
    bool exceeded;
} Matcher;

// Two guards, for the two ways a backtracker goes wrong. The depth cap is
// what keeps the C stack whole; the step budget is what stops the other
// blowup -- '(a+)+b' explores exponentially many paths that are each
// shallow, so no depth cap ever sees it. Both are generous for any pattern
// a person meant, and the overrun is reported, never felt.
#define MATCH_DEPTH_LIMIT 10000
#define MATCH_STEP_LIMIT 250000

static bool match_node(Matcher *m, const Node *n, const Cont *k, size_t pos);
static bool match_repeat(Matcher *m, const Node *n, size_t done,
                         const Cont *k, size_t pos);

static bool match_list(Matcher *m, const Node *first, const Cont *k,
                       size_t pos)
{
    if (first == NULL) {
        // Nothing left here; the continuation decides.
        if (k == NULL) {
            m->spans[0].end = pos;
            return true;
        }
        switch (k->kind) {
            case CONT_LIST:
                return match_list(m, k->list, k->rest, pos);
            case CONT_GROUP_END: {
                size_t saved = m->spans[k->index].end;
                m->spans[k->index].end = pos;
                if (match_list(m, NULL, k->rest, pos)) {
                    return true;
                }
                m->spans[k->index].end = saved;
                return false;
            }
            case CONT_REPEAT:
                if (pos == k->entered_at) {
                    // The round moved nothing; growing further never will,
                    // so the repeat settles for what is already done.
                    return match_list(m, NULL, k->rest, pos);
                }
                return match_repeat(m, k->repeat, k->done, k->rest, pos);
        }
        return false;
    }
    Cont tail;
    tail.kind = CONT_LIST;
    tail.list = first->next;
    tail.rest = k;
    tail.index = 0;
    tail.repeat = NULL;
    tail.done = 0;
    tail.entered_at = 0;
    return match_node(m, first, &tail, pos);
}

// One node against the text, the continuation saying what must follow.
static bool match_node(Matcher *m, const Node *n, const Cont *k, size_t pos)
{
    if (++m->depth > MATCH_DEPTH_LIMIT || ++m->steps > MATCH_STEP_LIMIT) {
        m->exceeded = true;
        m->depth--;
        return false;
    }
    bool answered = false;
    switch (n->kind) {
        case NODE_LIT:
            answered = pos + n->byte_count <= m->length &&
                       memcmp(m->text + pos, n->bytes, n->byte_count) == 0 &&
                       match_list(m, NULL, k, pos + n->byte_count);
            break;

        case NODE_ANY: {
            if (pos >= m->length) {
                break;
            }
            size_t width = 0;
            decode(m->text, m->length, pos, &width);
            answered = match_list(m, NULL, k, pos + width);
            break;
        }

        case NODE_CLASS: {
            if (pos >= m->length) {
                break;
            }
            size_t width = 0;
            unsigned point = decode(m->text, m->length, pos, &width);
            bool inside = false;
            for (size_t i = 0; i < n->range_count; i++) {
                if (point >= n->ranges[i].lo && point <= n->ranges[i].hi) {
                    inside = true;
                    break;
                }
            }
            answered = inside != n->negated &&
                       match_list(m, NULL, k, pos + width);
            break;
        }

        case NODE_BOL:
            answered = pos == 0 && match_list(m, NULL, k, pos);
            break;

        case NODE_EOL:
            answered = pos == m->length && match_list(m, NULL, k, pos);
            break;

        case NODE_SEQ:
            answered = match_list(m, n->child, k, pos);
            break;

        case NODE_ALT:
            for (const Node *choice = n->child; choice != NULL;
                 choice = choice->next) {
                // A choice is one sequence; its siblings are the other
                // choices, so it is matched alone, never as a list.
                Cont alone;
                alone.kind = CONT_LIST;
                alone.list = NULL;
                alone.rest = k;
                alone.index = 0;
                alone.repeat = NULL;
                alone.done = 0;
                alone.entered_at = 0;
                if (match_node(m, choice, &alone, pos)) {
                    answered = true;
                    break;
                }
                if (m->exceeded) {
                    break;
                }
            }
            break;

        case NODE_REPEAT:
            answered = match_repeat(m, n, 0, k, pos);
            break;

        case NODE_GROUP: {
            if (n->group == 0) {
                answered = match_list(m, n->child, k, pos);
                break;
            }
            LhatRegexSpan saved = m->spans[n->group];
            m->spans[n->group].begin = pos;
            m->spans[n->group].end = (size_t)-1;
            Cont closing;
            closing.kind = CONT_GROUP_END;
            closing.index = n->group;
            closing.rest = k;
            closing.list = NULL;
            closing.repeat = NULL;
            closing.done = 0;
            closing.entered_at = 0;
            answered = match_list(m, n->child, &closing, pos);
            if (!answered) {
                m->spans[n->group] = saved;
            }
            break;
        }
    }
    m->depth--;
    return answered;
}

static bool match_repeat(Matcher *m, const Node *n, size_t done,
                         const Cont *k, size_t pos)
{
    if (++m->depth > MATCH_DEPTH_LIMIT || ++m->steps > MATCH_STEP_LIMIT) {
        m->exceeded = true;
        m->depth--;
        return false;
    }
    bool answered = false;
    bool may_stop = done >= n->min;
    bool may_go = n->max == SIZE_MAX || done < n->max;

    if (n->lazy && may_stop) {
        answered = match_list(m, NULL, k, pos);
        if (answered || m->exceeded) {
            goto out;
        }
    }
    if (may_go && n->child != NULL) {
        Cont again;
        again.kind = CONT_REPEAT;
        again.repeat = n;
        again.done = done + 1;
        again.entered_at = pos;
        again.rest = k;
        again.list = NULL;
        again.index = 0;
        answered = match_node(m, n->child, &again, pos);
        if (answered || m->exceeded) {
            goto out;
        }
    }
    if (!n->lazy && may_stop) {
        answered = match_list(m, NULL, k, pos);
    }
out:
    m->depth--;
    return answered;
}

bool lhat_regex_search(const LhatRegex *regex, const char *text,
                       size_t length, size_t from,
                       LhatRegexSpan spans[LHAT_REGEX_MAX_GROUPS],
                       bool *depth_exceeded)
{
    if (depth_exceeded != NULL) {
        *depth_exceeded = false;
    }
    if (regex == NULL || from > length) {
        return false;
    }
    // One budget for the whole search, restarts included -- a pattern that
    // blows up at every start would otherwise multiply the limit by the
    // text's length.
    Matcher m;
    m.text = text;
    m.length = length;
    m.spans = spans;
    m.depth = 0;
    m.steps = 0;
    m.exceeded = false;
    for (size_t start = from;;) {
        for (size_t i = 0; i < LHAT_REGEX_MAX_GROUPS; i++) {
            spans[i].begin = (size_t)-1;
            spans[i].end = (size_t)-1;
        }
        spans[0].begin = start;
        if (match_node(&m, regex->root, NULL, start)) {
            return true;
        }
        if (m.exceeded) {
            if (depth_exceeded != NULL) {
                *depth_exceeded = true;
            }
            return false;
        }
        if (start >= length) {
            return false;
        }
        size_t width = 0;
        decode(text, length, start, &width);
        start += width;
    }
}
