#include "moar.h"

/*  TODO:
- add a tunable global determining at what depth it should
    start flattening.  Write a rope-tree-flattening function
    (not like string_flatten) that puts all the sub strands
    in one MVMStrand array.
    (optimization)
- add a tunable global determining under which size result
    string should it just do the old copying behavior, for
    join, concat, split, and repeat.
    This might be related to MVMString object size?
    (optimization)
- make the uc, lc, tc functions intelligently
    create ropes from the originals when deemed advantageous.
    (optimization)
*/

static MVMStrandIndex find_strand_index(MVMString *s, MVMStringIndex index);

/* for physical strings, applies the consumer function directly. For
 * "ropes", performs the binary search as directed by the strands table
 * to find the first starting strand index of the desired substring.
 * Then recursively traverses more strands until the desired number of
 * characters are passed to the consumer function from the strand tree.
 * Both this and the consumer function return a boolean saying whether
 * to abort the traversal early. */
MVMuint8 MVM_string_traverse_substring(MVMThreadContext *tc, MVMString *a, MVMStringIndex start, MVMStringIndex length, MVMStringIndex top_index, MVMSubstringConsumer consumer, void *data) {

    switch(STR_FLAGS(a)) {
        case MVM_STRING_TYPE_INT32:
        case MVM_STRING_TYPE_UINT8:
            return consumer(tc, a, start, length, top_index, data);
        case MVM_STRING_TYPE_ROPE: {
            MVMStrand *strands = a->body.strands;
            MVMStrandIndex lower = 255;
            MVMStringIndex index = start;
            MVMStrandIndex strand_index = find_strand_index(a, index);
            MVMStrand *strand = a->body.strands + strand_index;
            for(;;) {
                MVMuint8 return_val;
                /* get the number of available codepoints from
                 * this strand. */
                MVMStringIndex substring_length =
                    strands[strand_index + 1].compare_offset - index;
                /* determine how many codepoints to actually consume */
                if (length < substring_length)
                    substring_length = length;
                /* call ourself on the sub-strand */
                return_val = MVM_string_traverse_substring(tc, strand->string,
                    index - strand->compare_offset + strand->string_offset,
                    substring_length, top_index + index, consumer, data);
                /* if we've been instructed to, abort early */
                if (return_val)
                    return return_val;
                /* reduce the number of codepoints requested by the number
                 * consumed. */
                length -= substring_length;
                /* if we've consumed all the codepoints, return without
                 * indicating abort early. */
                if (!length)
                    return 0;
                /* advance the index by the number of codepoints consumed. */
                index += substring_length;
                /* advance in the strand table. The caller must guarantee
                 * this won't overflow by checking lengths. */
                strand = &strands[++strand_index];
            }
        }
        default:
            MVM_exception_throw_adhoc(tc, "internal string corruption");
    }
    MVM_exception_throw_adhoc(tc, "internal string corruption");
}

/* stack record created with each invocation of the string descent. */
typedef struct CompareCursor {
    MVMString *string; /* string into which descended */
    struct CompareCursor *parent; /* parent cursor */
    MVMint16 *gaps; /* number of gaps can ascend */
    MVMStringIndex string_idx; /* in string, not strand */
    MVMStringIndex end_idx; /* index past last codepoint needed */
    MVMStrandIndex strand_idx; /* current strand index */
    MVMuint8 owns_buffer; /* whether it owns the last buffer.
            for ropes, whether it's uninitialized. */
    MVMuint8 isa; /* whether it's from string tree a */
} CompareCursor;

typedef struct {
    CompareCursor *cursora, *cursorb; /* cursors */
    MVMCodepoint32 *int32s; /* last physical string's buffer */
    MVMCodepoint8 *uint8s; /* last physical string's buffer */
    MVMStringIndex available; /* number of codepoints in the buffer */
    MVMStringIndex needed; /* number of codepoints still needed */
} CompareDescentState;

/* uses the computed binary search table to find the strand containing the index */
static MVMStrandIndex find_strand_index(MVMString *s, MVMStringIndex index) {
    MVMStrand *strands = s->body.strands;
    MVMStrandIndex strand_index = s->body.num_strands / 2;
    MVMStrandIndex upper = s->body.num_strands, lower = 0;
    while (1) {
        MVMStrand *strand = strands + strand_index;
        if (strand_index == lower) return strand_index;
        *(index >= strand->compare_offset ? &lower : &upper) = strand_index;
        strand_index = lower + (upper - lower) / 2;
    }
}

#define descend_sized_chew_success(member, other_member) \
st->needed -= tocompare; \
if (!st->needed) { \
    *result = 2; \
    return; \
} \
else if (wehave > tocompare) { \
    st->member = c->string->body.member + c->string_idx + tocompare; \
    st->available = wehave - tocompare; \
    c->owns_buffer = 1; \
    st->other_member = NULL; \
} \
else { \
    st->other_member += tocompare; \
    st->available -= tocompare; \
} \

#define descend_sized(member, other_member, size, other_size, cp, other_cp) \
MVMStringIndex wehave = c->end_idx - c->string_idx; \
if (st->available) { \
    MVMStringIndex tocompare = st->available < wehave ? st->available : wehave; \
    if (st->member) { \
        if (memcmp(st->member, c->string->body.member + c->string_idx, \
               wehave * sizeof(size))) { \
            *result = 1; \
            return; \
        } \
        descend_sized_chew_success(member, other_member) \
    } \
    else { \
        size \
            *cp = c->string->body.member + c->string_idx, \
            *final = cp + tocompare; \
        other_size *other_cp = st->other_member; \
        for (; cp < final; ) { \
            if (*cp32++ != (MVMCodepoint32) *cp8++) { \
                *result = 1; \
                return; \
            } \
        } \
        descend_sized_chew_success(other_member, member) \
    } \
    if (c->isa) { st->cursora = c->parent; } \
    else { st->cursorb = c->parent; } \
    return; \
} \
st->member = c->string->body.member + c->string_idx; \
st->other_member = NULL; \
st->available = wehave; \
c->owns_buffer = 1; \
continue;

/* If a is composed of strands, create a descent record, initialize it, and
 * descend into the substring. If result is set when it returns, return.
 * If a is a real string, grab some codepoints from a string, compare
 * with codepoints from the other buffer. If different, set result nonzero and
 * it will fast return. Otherwise, reset buffer indexes
 * to zero. Loop if `needed` for the current string is nonzero. */
static void compare_descend(MVMThreadContext *tc, CompareDescentState *st,
        MVMuint8 *result, CompareCursor *orig) {
    /* while we still need to compare some things */
    while (st->needed) {
        /* pick a tree from which to get more characters. */
        CompareCursor *c = st->cursora->owns_buffer ? st->cursorb : st->cursora;

        if (*c->gaps) {
            (*c->gaps)--;
            if (c->isa) { st->cursora = c->parent; }
            else { st->cursorb = c->parent; }
            return;
        }

        /* get some characters, comparing as they're found */
        switch(STR_FLAGS(c->string)) {
            case MVM_STRING_TYPE_INT32: {
                descend_sized(int32s, uint8s, MVMCodepoint32, MVMCodepoint8, cp32, cp8)
            }
            case MVM_STRING_TYPE_UINT8: {
                descend_sized(uint8s, int32s, MVMCodepoint8, MVMCodepoint32, cp8, cp32)
            }
            case MVM_STRING_TYPE_ROPE: {
                MVMStrand *strand;
                MVMStringIndex next_compare_offset, child_length, child_idx, child_end_idx;
                if (c->owns_buffer) { /* first time we've seen this rope */
                    c->strand_idx = find_strand_index(c->string, c->string_idx);
                    c->owns_buffer = 0;
                }
                if (c->string_idx == c->end_idx) {
                    if (c->isa) { st->cursora = c->parent; }
                    else { st->cursorb = c->parent; }
                    if (orig == c)
                        return;
                    (*c->gaps)++;
                    continue;
                }
                strand = c->string->body.strands + c->strand_idx;
                child_idx = c->string_idx - strand->compare_offset;
                next_compare_offset = (strand + 1)->compare_offset;
                child_length = next_compare_offset - strand->compare_offset;
                child_length = child_length > c->end_idx - c->string_idx
                    ? c->end_idx - c->string_idx : child_length;
                child_end_idx = child_idx + child_length;
                c->string_idx += child_length;
                c->strand_idx++;
                {
                    CompareCursor child = { strand->string, c, c->gaps, child_idx,
                        child_end_idx, 0, IS_ROPE(strand->string), c->isa };
                    if (c->isa) { st->cursora = &child; } else { st->cursorb = &child; }
                    compare_descend(tc, st, result, &child);
                }
                if (*result) return;
                continue;
            }
            default:
                MVM_exception_throw_adhoc(tc, "internal string corruption");
        }
    }
    *result = 2;
}

/* returns nonzero if two substrings are equal, doesn't check bounds */
MVMint64 MVM_string_substrings_equal_nocheck(MVMThreadContext *tc, MVMString *a,
        MVMint64 starta, MVMint64 length, MVMString *b, MVMint64 startb) {
    MVMint16 gapsa = 0, gapsb = 0;
    MVMuint8 result = 0;
    CompareCursor
        cursora = { a, NULL, &gapsa, starta, starta + length, 0, IS_ROPE(a), 1 },
        cursorb = { b, NULL, &gapsb, startb, startb + length, 0, IS_ROPE(b), 0 };
    CompareDescentState st = { &cursora, &cursorb, NULL, NULL, 0, length };

    compare_descend(tc, &st, &result, &cursora);
    return result ? result - 1 : 1;
}

/* returns the codepoint without doing checks, for internal VM use only. */
MVMCodepoint32 MVM_string_get_codepoint_at_nocheck(MVMThreadContext *tc, MVMString *a, MVMint64 index) {
    MVMStringIndex idx = (MVMStringIndex)index;

    switch(STR_FLAGS(a)) {
        case MVM_STRING_TYPE_INT32:
            return a->body.int32s[idx];
        case MVM_STRING_TYPE_UINT8:
            return (MVMCodepoint32)a->body.uint8s[idx];
        case MVM_STRING_TYPE_ROPE: {
            MVMStrand *strand = a->body.strands + find_strand_index(a, idx);
            return MVM_string_get_codepoint_at_nocheck(tc,
                strand->string, idx - strand->compare_offset + strand->string_offset);
        }
    }
    MVM_exception_throw_adhoc(tc, "internal string corruption");
    return 0;
}

/* Returns the location of one string in another or -1  */
MVMint64 MVM_string_index(MVMThreadContext *tc, MVMString *haystack, MVMString *needle, MVMint64 start) {
    MVMint64 result = -1;
    size_t index    = (size_t)start;
    MVMStringIndex hgraphs = NUM_GRAPHS(haystack), ngraphs = NUM_GRAPHS(needle);

    if (!IS_CONCRETE((MVMObject *)haystack)) {
        MVM_exception_throw_adhoc(tc, "index needs a concrete search target");
    }

    if (!IS_CONCRETE((MVMObject *)needle)) {
        MVM_exception_throw_adhoc(tc, "index needs a concrete search term");
    }

    if (!ngraphs && !hgraphs)
        return 0; /* the empty strings are equal and start at zero */

    if (!hgraphs)
        return -1;

    if (start < 0 || start >= hgraphs)
        return -1;

    if (ngraphs > hgraphs || ngraphs < 1)
        return -1;
    /* brute force for now. horrible, yes. halp. */
    while (index <= hgraphs - ngraphs) {
        if (MVM_string_substrings_equal_nocheck(tc, needle, 0, ngraphs, haystack, index)) {
            result = (MVMint64)index;
            break;
        }
        index++;
    }
    return result;
}

/* Returns the location of one string in another or -1  */
MVMint64 MVM_string_index_from_end(MVMThreadContext *tc, MVMString *haystack, MVMString *needle, MVMint64 start) {
    MVMint64 result = -1;
    size_t index;
    MVMStringIndex hgraphs = NUM_GRAPHS(haystack), ngraphs = NUM_GRAPHS(needle);

    if (!IS_CONCRETE((MVMObject *)haystack)) {
        MVM_exception_throw_adhoc(tc, "index needs a concrete search target");
    }

    if (!IS_CONCRETE((MVMObject *)needle)) {
        MVM_exception_throw_adhoc(tc, "index needs a concrete search term");
    }

    if (!ngraphs && !hgraphs)
        return 0; /* the empty strings are equal and start at zero */

    if (!hgraphs)
        return -1;

    if (start == -1)
        start = hgraphs - ngraphs;

    if (start < 0 || start >= hgraphs)
        /* maybe return -1 instead? */
        MVM_exception_throw_adhoc(tc, "index start offset out of range");

    if (ngraphs > hgraphs || ngraphs < 1)
        return -1;

    index = start;

    /* brute force for now. horrible, yes. halp. */
    do {
        if (MVM_string_substrings_equal_nocheck(tc, needle, 0, ngraphs, haystack, index)) {
            result = (MVMint64)index;
            break;
        }
    } while (index-- > 0);
    return result;
}

/* Returns a substring of the given string */
MVMString * MVM_string_substring(MVMThreadContext *tc, MVMString *a, MVMint64 offset, MVMint64 length) {
    MVMString *result;
    MVMStrand *strands;
    MVMint64   start_pos, end_pos;

    /* convert to signed to avoid implicit arithmetic conversions */
    MVMint64 agraphs = (MVMint64)NUM_GRAPHS(a);

    if (!IS_CONCRETE((MVMObject *)a)) {
        MVM_exception_throw_adhoc(tc, "Substring needs a concrete string");
    }

    /* -1 signifies go to the end of the string
     * anything less is a bug
     */
    if (length < -1)
        MVM_exception_throw_adhoc(tc, "Substring length (%lld) cannot be negative", length);

    /* negative offsets count from the end */
    start_pos = offset < 0 ? offset + agraphs : offset;
    end_pos   = length == -1 ? agraphs : start_pos + length;

    /* return an empty string if start_pos is out of bounds but positive */
    if (start_pos > agraphs) {
        start_pos = 0;
        end_pos   = 0;
    }

    if (end_pos < 0)
        MVM_exception_throw_adhoc(tc, "Substring end (%lld) cannot be less than 0", end_pos);

    if (start_pos < 0)
        start_pos = 0;

    if (end_pos > agraphs)
        end_pos = agraphs;

    if (start_pos == end_pos)
        return tc->instance->str_consts.empty;

    MVM_gc_root_temp_push(tc, (MVMCollectable **)&a);
    result = (MVMString *)REPR(a)->allocate(tc, STABLE(a));
    MVM_gc_root_temp_pop(tc);

    strands = result->body.strands = calloc(sizeof(MVMStrand), 2);
    /* if we're substringing a substring, substring the same one */
    if (IS_ONE_STRING_ROPE(a)) {
        strands->string_offset = (MVMStringIndex)start_pos + a->body.strands->string_offset;
        strands->string = a->body.strands->string;
    }
    else {
        strands->string_offset = (MVMStringIndex)start_pos;
        strands->string = a;
    }
    /* result->body.codes  = 0; /* Populate this lazily. */
    result->body.flags = MVM_STRING_TYPE_ROPE;
    result->body.num_strands = 1;
    strands[1].graphs = end_pos - start_pos;
    _STRAND_DEPTH(result) = STRAND_DEPTH(strands->string) + 1;

    MVM_string_flatten(tc, result);

    return result;
}

MVMString * MVM_string_replace(MVMThreadContext *tc, MVMString *original, MVMint64 start, MVMint64 count, MVMString *replacement) {
    /* XXX this could probably be done more efficiently directly. */
    MVMString *first_part;
    MVMString *rest_part;
    MVMString *result;

    MVM_gc_root_temp_push(tc, (MVMCollectable **)&replacement);
    MVM_gc_root_temp_push(tc, (MVMCollectable **)&original);
    first_part = MVM_string_substring(tc, original, 0, start);
    MVM_gc_root_temp_push(tc, (MVMCollectable **)&first_part);

    rest_part  = MVM_string_substring(tc, original, start + count, -1);
    rest_part  = MVM_string_concatenate(tc, replacement, rest_part);
    result     = MVM_string_concatenate(tc, first_part, rest_part);

    MVM_gc_root_temp_pop_n(tc, 3);

    MVM_string_flatten(tc, result);

    return result;
}

/* Append one string to another. */
/* XXX inline parent's strands if it's a rope too? */
MVMString * MVM_string_concatenate(MVMThreadContext *tc, MVMString *a, MVMString *b) {
    MVMString *result;
    MVMStrandIndex strand_count = 0;
    MVMStrand *strands;
    MVMStringIndex index = 0;
    MVMStrandIndex max_strand_depth = 0;
    MVMStringIndex agraphs = NUM_GRAPHS(a), bgraphs = NUM_GRAPHS(b), rgraphs;

    if (!IS_CONCRETE((MVMObject *)a) || !IS_CONCRETE((MVMObject *)b)) {
        MVM_exception_throw_adhoc(tc, "Concatenate needs concrete strings");
    }

    MVM_gc_root_temp_push(tc, (MVMCollectable **)&a);
    MVM_gc_root_temp_push(tc, (MVMCollectable **)&b);
    result = (MVMString *)REPR(a)->allocate(tc, STABLE(a));
    MVM_gc_root_temp_pop_n(tc, 2);

    /* there could be unattached combining chars at the beginning of b,
       so, XXX TODO handle this */
    result->body.flags = MVM_STRING_TYPE_ROPE;
    rgraphs = agraphs + bgraphs;

    if (agraphs)
        strand_count = 1;
    if (bgraphs)
        ++strand_count;
    strands = result->body.strands = calloc(sizeof(MVMStrand), strand_count + 1);
    strand_count = 0;
    if (agraphs) {
        strands->string = a;
        strands->string_offset = 0;
        strands->compare_offset = index;
        index = agraphs;
        strand_count = 1;
        max_strand_depth = STRAND_DEPTH(a);
    }
    if (bgraphs) {
        strands[strand_count].string = b;
        strands[strand_count].string_offset = 0;
        strands[strand_count].compare_offset = index;
        index += bgraphs;
        ++strand_count;
        if (STRAND_DEPTH(b) > max_strand_depth)
            max_strand_depth = STRAND_DEPTH(b);
    }
    strands[strand_count].graphs = index;
    result->body.num_strands = strand_count;
    result->body.flags = MVM_STRING_TYPE_ROPE;
    _STRAND_DEPTH(result) = max_strand_depth + 1;

    MVM_string_flatten(tc, result);

    return result;
}

MVMString * MVM_string_repeat(MVMThreadContext *tc, MVMString *a, MVMint64 count) {
    MVMString *result;
    MVMint64 bkup_count = count;
    MVMStringIndex string_offset = 0, graphs = 0, rgraphs;

    if (!IS_CONCRETE((MVMObject *)a)) {
        MVM_exception_throw_adhoc(tc, "repeat needs a concrete string");
    }

    if (count < 0)
        MVM_exception_throw_adhoc(tc, "repeat count (%lld) cannot be negative", count);

    if (count > (1<<30))
        MVM_exception_throw_adhoc(tc, "repeat count > %lld arbitrarily unsupported...", (1<<30));

    MVM_gc_root_temp_push(tc, (MVMCollectable **)&a);
    result = (MVMString *)REPR(a)->allocate(tc, STABLE(a));
    MVM_gc_root_temp_pop(tc);

    if (IS_ONE_STRING_ROPE(a)) {
        string_offset = a->body.strands->string_offset;
        graphs = a->body.strands[1].graphs;
        a = a->body.strands->string;
    }
    else {
        graphs = NUM_GRAPHS(a);
    }

    rgraphs = graphs * count;

    /* XXX compute tradeoff here of space usage of the strands table vs real string */
    if (rgraphs) {
        MVMStrand *strands = result->body.strands = calloc(sizeof(MVMStrand), count + 1);
        result->body.flags = MVM_STRING_TYPE_ROPE;

        while (count--) {
            strands[count].compare_offset = count * graphs;
            strands[count].string = a;
            strands[count].string_offset = string_offset;
        }
        strands[bkup_count].graphs = rgraphs;
        result->body.num_strands = bkup_count;
        _STRAND_DEPTH(result) = STRAND_DEPTH(a) + 1;
    }
    else {
        result->body.flags = MVM_STRING_TYPE_UINT8;
        /* leave graphs 0 and storage null */
    }

    MVM_string_flatten(tc, result);

    return result;
}

void MVM_string_say(MVMThreadContext *tc, MVMString *a) {
    MVMuint8 *utf8_encoded;
    MVMuint64 utf8_encoded_length;

    if (!IS_CONCRETE((MVMObject *)a)) {
        MVM_exception_throw_adhoc(tc, "say needs a concrete string");
    }

    /* XXX send a buffer of substrings of size 100 or something? */
    utf8_encoded = MVM_string_utf8_encode(tc, a, &utf8_encoded_length);
    utf8_encoded[utf8_encoded_length] = '\n';

    fwrite(utf8_encoded, 1, utf8_encoded_length + 1, stdout);

    free(utf8_encoded);
}

void MVM_string_print(MVMThreadContext *tc, MVMString *a) {
    MVMuint8 *utf8_encoded;
    MVMuint64 utf8_encoded_length;

    if (!IS_CONCRETE((MVMObject *)a)) {
        MVM_exception_throw_adhoc(tc, "print needs a concrete string");
    }

    /* XXX send a buffer of substrings of size 100 or something? */
    utf8_encoded = MVM_string_utf8_encode(tc, a, &utf8_encoded_length);

    fwrite(utf8_encoded, 1, utf8_encoded_length, stdout);

    free(utf8_encoded);
}

/* Tests whether one string a has the other string b as a substring at that index */
MVMint64 MVM_string_equal_at(MVMThreadContext *tc, MVMString *a, MVMString *b, MVMint64 offset) {

    MVMStringIndex agraphs, bgraphs;

    if (!IS_CONCRETE((MVMObject *)a) || !IS_CONCRETE((MVMObject *)b)) {
        MVM_exception_throw_adhoc(tc, "equal_at needs concrete strings");
    }

    agraphs = NUM_GRAPHS(a);
    bgraphs = NUM_GRAPHS(b);

    if (offset < 0) {
        offset += agraphs;
        if (offset < 0)
            offset = 0; /* XXX I think this is the right behavior here */
    }
    if (agraphs - offset < bgraphs)
        return 0;
    return MVM_string_substrings_equal_nocheck(tc, a, offset, bgraphs, b, 0);
}

MVMint64 MVM_string_equal_at_ignore_case(MVMThreadContext *tc, MVMString *a, MVMString *b, MVMint64 offset) {
    MVMString *lca = MVM_string_lc(tc, a);
    MVMString *lcb;

    MVMROOT(tc, lca, {
        lcb = MVM_string_lc(tc, b);
    });

    return MVM_string_equal_at(tc, lca, lcb, offset);
}

/* Compares two strings for equality. */
MVMint64 MVM_string_equal(MVMThreadContext *tc, MVMString *a, MVMString *b) {
    if (a == b)
        return 1;
    if (NUM_GRAPHS(a) != NUM_GRAPHS(b))
        return 0;
    return MVM_string_equal_at(tc, a, b, 0);
}

/* more general form of has_at; compares two substrings for equality */
MVMint64 MVM_string_have_at(MVMThreadContext *tc, MVMString *a,
        MVMint64 starta, MVMint64 length, MVMString *b, MVMint64 startb) {

    if (!IS_CONCRETE((MVMObject *)a) || !IS_CONCRETE((MVMObject *)b)) {
        MVM_exception_throw_adhoc(tc, "have_at needs concrete strings");
    }

    if (starta < 0 || startb < 0)
        return 0;
    if (length == 0)
        return 1;
    if (starta + length > NUM_GRAPHS(a) || startb + length > NUM_GRAPHS(b))
        return 0;
    return MVM_string_substrings_equal_nocheck(tc, a, starta, length, b, startb);
}

/* returns the codepoint (could be a negative synthetic) at a given index of the string */
MVMint64 MVM_string_get_codepoint_at(MVMThreadContext *tc, MVMString *a, MVMint64 index) {
    MVMStringIndex agraphs;

    if (!IS_CONCRETE((MVMObject *)a)) {
        MVM_exception_throw_adhoc(tc, "codepoint_at needs a concrete string");
    }

    agraphs = NUM_GRAPHS(a);

    if (index < 0 || index >= agraphs)
        MVM_exception_throw_adhoc(tc, "Invalid string index: max %lld, got %lld",
            agraphs - 1, index);
    return (MVMint64)MVM_string_get_codepoint_at_nocheck(tc, a, index);
}

/* finds the location of a codepoint in a string.  Useful for small character class lookup */
MVMint64 MVM_string_index_of_codepoint(MVMThreadContext *tc, MVMString *a, MVMint64 codepoint) {
    size_t index = -1;
    while (++index < NUM_GRAPHS(a))
        /* XXX make this use the traversal function */
        if (MVM_string_get_codepoint_at_nocheck(tc, a, index) == codepoint)
            return index;
    return -1;
}

typedef struct MVMCaseChangeState {
    MVMString *dest;
    MVMStringIndex size;
    MVMint32 case_change_type;
} MVMCaseChangeState;

#define change_case_iterate(member, dest_member, dest_size) \
for (i = string->body.member + start; i < string->body.member + start + length; ) { \
    if (dest->body.graphs == state->size) { \
        if (!state->size) state->size = 16; \
        else state->size *= 2; \
        dest->body.dest_member = realloc(dest->body.dest_member, \
            state->size * sizeof(dest_size)); \
    } \
    dest->body.dest_member[dest->body.graphs++] = \
        MVM_unicode_get_case_change(tc, (MVMCodepoint32) *i++, state->case_change_type); \
}

/* XXX make this handle case changes that change the number of characters.
    Will require some form of buffering/lookahead, or rollback of a placed
    char if a following char forms a composite sequence that case changes
    together. */
MVM_SUBSTRING_CONSUMER(MVM_string_case_change_consumer) {
    MVMCaseChangeState *state = (MVMCaseChangeState *)data;
    MVMString *dest = state->dest;
    switch (STR_FLAGS(string)) {
        case MVM_STRING_TYPE_INT32: {
            MVMCodepoint32 *i;
            if (!IS_WIDE(dest)) {
                MVM_string_flatten(tc, dest);
            }
            change_case_iterate(int32s, int32s, MVMCodepoint32)
            break;
        }
        case MVM_STRING_TYPE_UINT8: {
            MVMCodepoint8 *i;
            if (IS_WIDE(dest)) {
                change_case_iterate(uint8s, int32s, MVMCodepoint32)
            }
            else { /* hopefully most common/fast case of ascii->ascii */
                change_case_iterate(uint8s, uint8s, MVMCodepoint8)
            }
            break;
        }
        default:
            MVM_exception_throw_adhoc(tc, "internal string corruption");
    }
    return 0;
}

/* Uppercases a string. */
#define case_change_func(funcname, type, error) \
MVMString * funcname(MVMThreadContext *tc, MVMString *s) { \
    MVMString *result; \
    MVMStringIndex i; \
    MVMCaseChangeState state = { NULL, 0, type }; \
    \
    if (!IS_CONCRETE((MVMObject *)s)) { \
        MVM_exception_throw_adhoc(tc, error); \
    } \
    \
    MVM_gc_root_temp_push(tc, (MVMCollectable **)&s); \
    state.dest = result = (MVMString *)REPR(s)->allocate(tc, STABLE(s)); \
    MVM_gc_root_temp_pop(tc); \
     \
    MVM_string_traverse_substring(tc, s, 0, NUM_GRAPHS(s), 0, \
        MVM_string_case_change_consumer, &state); \
     \
    return result; \
}

case_change_func(MVM_string_uc, MVM_unicode_case_change_type_upper, "uc needs a concrete string")
case_change_func(MVM_string_lc, MVM_unicode_case_change_type_lower, "lc needs a concrete string")
case_change_func(MVM_string_tc, MVM_unicode_case_change_type_title, "tc needs a concrete string")

/* decodes a C buffer to an MVMString, dependent on the encoding type flag */
MVMString * MVM_string_decode(MVMThreadContext *tc,
        MVMObject *type_object, char *Cbuf, MVMint64 byte_length, MVMint64 encoding_flag) {

    /* someday make 0 mean "try really hard to detect the encoding */

    switch(encoding_flag) {
        case MVM_encoding_type_utf8:
            return MVM_string_utf8_decode(tc, type_object, Cbuf, byte_length);
        case MVM_encoding_type_ascii:
            return MVM_string_ascii_decode(tc, type_object, Cbuf, byte_length);
        case MVM_encoding_type_latin1:
            return MVM_string_latin1_decode(tc, type_object, Cbuf, byte_length);
        case MVM_encoding_type_utf16:
            return MVM_string_utf16_decode(tc, type_object, Cbuf, byte_length);
        case MVM_encoding_type_windows1252:
            return MVM_string_windows1252_decode(tc, type_object, Cbuf, byte_length);
        default:
            MVM_exception_throw_adhoc(tc, "invalid encoding type flag: %d", encoding_flag);
    }
    return NULL;
}

/* encodes an MVMString to a C buffer, dependent on the encoding type flag */
MVMuint8 * MVM_string_encode(MVMThreadContext *tc, MVMString *s, MVMint64 start, MVMint64 length, MVMuint64 *output_size, MVMint64 encoding_flag) {
    switch(encoding_flag) {
        case MVM_encoding_type_utf8:
            return MVM_string_utf8_encode_substr(tc, s, output_size, start, length);
        case MVM_encoding_type_ascii:
            return MVM_string_ascii_encode_substr(tc, s, output_size, start, length);
        case MVM_encoding_type_latin1:
            return MVM_string_latin1_encode_substr(tc, s, output_size, start, length);
        case MVM_encoding_type_utf16:
            return MVM_string_utf16_encode_substr(tc, s, output_size, start, length);
        case MVM_encoding_type_windows1252:
            return MVM_string_windows1252_encode_substr(tc, s, output_size, start, length);
        default:
            MVM_exception_throw_adhoc(tc, "invalid encoding type flag: %d", encoding_flag);
    }
    return NULL;
}

/* Encodes a string, and writes the encoding string into the supplied Buf
 * instance, which should be an integer array with MVMArray REPR. */
void MVM_string_encode_to_buf(MVMThreadContext *tc, MVMString *s, MVMString *enc_name, MVMObject *buf) {
    MVMuint64 output_size;
    MVMuint8 *encoded;
    MVMArrayREPRData *buf_rd;
    MVMuint8 elem_size = 0;

    /* Ensure the target is in the correct form. */
    if (!IS_CONCRETE(buf) || REPR(buf)->ID != MVM_REPR_ID_MVMArray)
        MVM_exception_throw_adhoc(tc, "encode requires a native array to write into");
    buf_rd = (MVMArrayREPRData *)STABLE(buf)->REPR_data;
    if (buf_rd) {
        switch (buf_rd->slot_type) {
            case MVM_ARRAY_I64: elem_size = 8; break;
            case MVM_ARRAY_I32: elem_size = 4; break;
            case MVM_ARRAY_I16: elem_size = 2; break;
            case MVM_ARRAY_I8:  elem_size = 1; break;
            case MVM_ARRAY_U64: elem_size = 8; break;
            case MVM_ARRAY_U32: elem_size = 4; break;
            case MVM_ARRAY_U16: elem_size = 2; break;
            case MVM_ARRAY_U8:  elem_size = 1; break;
        }
    }
    if (!elem_size)
        MVM_exception_throw_adhoc(tc, "encode requires a native int array");
    if (((MVMArray *)buf)->body.slots.any)
        MVM_exception_throw_adhoc(tc, "encode requires an empty array");

    /* At least find_encoding may allocate on first call, so root just
     * in case. */
    MVMROOT(tc, buf, {
    MVMROOT(tc, s, {
        const MVMuint8 encoding_flag = MVM_string_find_encoding(tc, enc_name);
        encoded = MVM_string_encode(tc, s, 0, NUM_GRAPHS(s), &output_size,
            encoding_flag);
    });
    });

    /* Stash the encoded data in the VMArray. */
    ((MVMArray *)buf)->body.slots.i8 = (MVMint8 *)encoded;
    ((MVMArray *)buf)->body.start    = 0;
    ((MVMArray *)buf)->body.ssize    = output_size / elem_size;
    ((MVMArray *)buf)->body.elems    = output_size / elem_size;
}

/* Decodes a string using the data from the specified Buf. */
MVMString * MVM_string_decode_from_buf(MVMThreadContext *tc, MVMObject *buf, MVMString *enc_name) {
    MVMArrayREPRData *buf_rd;
    MVMuint8 encoding_flag;
    MVMuint8 elem_size = 0;

    /* Ensure the source is in the correct form. */
    if (!IS_CONCRETE(buf) || REPR(buf)->ID != MVM_REPR_ID_MVMArray)
        MVM_exception_throw_adhoc(tc, "decode requires a native array to read from");
    buf_rd = (MVMArrayREPRData *)STABLE(buf)->REPR_data;
    if (buf_rd) {
        switch (buf_rd->slot_type) {
            case MVM_ARRAY_I64: elem_size = 8; break;
            case MVM_ARRAY_I32: elem_size = 4; break;
            case MVM_ARRAY_I16: elem_size = 2; break;
            case MVM_ARRAY_I8:  elem_size = 1; break;
            case MVM_ARRAY_U64: elem_size = 8; break;
            case MVM_ARRAY_U32: elem_size = 4; break;
            case MVM_ARRAY_U16: elem_size = 2; break;
            case MVM_ARRAY_U8:  elem_size = 1; break;
        }
    }
    if (!elem_size)
        MVM_exception_throw_adhoc(tc, "encode requires a native int array");

    /* Decode. */
    MVMROOT(tc, buf, {
        encoding_flag = MVM_string_find_encoding(tc, enc_name);
    });
    return MVM_string_decode(tc, tc->instance->VMString,
        ((MVMArray *)buf)->body.slots.i8 + ((MVMArray *)buf)->body.start,
        ((MVMArray *)buf)->body.elems * elem_size,
        encoding_flag);
}

MVMObject * MVM_string_split(MVMThreadContext *tc, MVMString *separator, MVMString *input) {
    MVMObject *result;
    MVMStringIndex start, end, sep_length;
    MVMHLLConfig *hll = MVM_hll_current(tc);

    if (!IS_CONCRETE((MVMObject *)separator)) {
        MVM_exception_throw_adhoc(tc, "split needs a concrete string separator");
    }

    MVMROOT(tc, input, {
    MVMROOT(tc, separator, {
        result = MVM_repr_alloc_init(tc, hll->slurpy_array_type);
        MVMROOT(tc, result, {
            start = 0;
            end = NUM_GRAPHS(input);
            sep_length = NUM_GRAPHS(separator);

            while (start < end) {
                MVMString *portion;
                MVMStringIndex index;
                MVMStringIndex length;

                /* XXX make this use the dual-traverse iterator, but such that it
                    can reset the index of what it's comparing... <!> */
                index = MVM_string_index(tc, input, separator, start);
                length = sep_length ? (index == -1 ? end : index) - start : 1;
                if (length > 0 || (sep_length && length == 0)) {
                    portion = MVM_string_substring(tc, input, start, length);
                    MVMROOT(tc, portion, {
                        MVMObject *pobj = MVM_repr_alloc_init(tc, hll->str_box_type);
                        MVM_repr_set_str(tc, pobj, portion);
                        MVM_repr_push_o(tc, result, pobj);
                    });
                }
                start += length + sep_length;
                /* Gather an empty string if the delimiter is found at the end. */
                if (sep_length && start == end) {
                    MVMObject *pobj = MVM_repr_alloc_init(tc, hll->str_box_type);
                    MVM_repr_set_str(tc, pobj, tc->instance->str_consts.empty);
                    MVM_repr_push_o(tc, result, pobj);
                }
            }
        });
    });
    });

    return result;
}

MVMString * MVM_string_join(MVMThreadContext *tc, MVMString *separator, MVMObject *input) {
    MVMint64 elems, length = 0, index = -1, position = 0;
    MVMString *portion, *result;
    MVMuint32 codes = 0;
    MVMStrandIndex portion_index = 0, max_strand_depth;
    MVMStringIndex sgraphs, rgraphs;
    MVMStrand *strands;
    MVMint64 is_str_array;

    if (!IS_CONCRETE(input)) {
        MVM_exception_throw_adhoc(tc, "join needs a concrete array to join");
    }

    if (!IS_CONCRETE((MVMObject *)separator)) {
        MVM_exception_throw_adhoc(tc, "join needs a concrete separator");
    }

    MVMROOT(tc, separator, {
    MVMROOT(tc, input, {
        result = (MVMString *)MVM_repr_alloc_init(tc, (MVMObject *)separator);
        MVMROOT(tc, result, {
            elems = REPR(input)->elems(tc, STABLE(input),
                input, OBJECT_BODY(input));
            is_str_array = REPR(input)->pos_funcs.get_elem_storage_spec(tc,
                STABLE(input)).boxed_primitive == MVM_STORAGE_SPEC_BP_STR;

            sgraphs = NUM_GRAPHS(separator);
            max_strand_depth = STRAND_DEPTH(separator);

            while (++index < elems) {
                MVMStringIndex pgraphs;

                if (is_str_array) {
                    portion = MVM_repr_at_pos_s(tc, input, index);
                    if (!portion)
                        continue;
                }
                else {
                    /* allow null or type object items in the array, I guess.. */
                    MVMObject *item = MVM_repr_at_pos_o(tc, input, index);
                    if (!item || !IS_CONCRETE(item))
                        continue;
                    portion = MVM_repr_get_str(tc, item);
                }

                pgraphs = NUM_GRAPHS(portion);
                if (pgraphs)
                    ++portion_index;
                if (index && sgraphs)
                    ++portion_index;
                length += pgraphs + (index ? sgraphs : 0);
                /* XXX codes += portion->body.codes + (index ? separator->body.codes : 0); */
                if (STRAND_DEPTH(portion) > max_strand_depth)
                    max_strand_depth = STRAND_DEPTH(portion);
            }

            rgraphs = length;
            /* XXX consider whether to coalesce combining characters
            if they cause new combining sequences to appear */
            /* XXX result->body.codes = codes; */

            if (portion_index > (1<<30)) {
                MVM_exception_throw_adhoc(tc, "join array items > %lld arbitrarily unsupported...", (1<<30));
            }

            if (portion_index) {
                index = -1;
                position = 0;
                strands = result->body.strands = calloc(sizeof(MVMStrand), portion_index + 1);

                portion_index = 0;
                while (++index < elems) {
                    if (is_str_array) {
                        portion = MVM_repr_at_pos_s(tc, input, index);
                        if (!portion)
                            continue;
                    }
                    else {
                        MVMObject *item = MVM_repr_at_pos_o(tc, input, index);
                        if (!item || !IS_CONCRETE(item))
                            continue;
                        portion = MVM_repr_get_str(tc, item);
                    }

                    /* Note: this allows the separator to precede the empty string. */
                    if (index && sgraphs) {
                        strands[portion_index].compare_offset = position;
                        strands[portion_index].string = separator;
                        position += sgraphs;
                        ++portion_index;
                    }

                    length = NUM_GRAPHS(portion);
                    if (length) {
                        strands[portion_index].compare_offset = position;
                        strands[portion_index].string = portion;
                        position += length;
                        ++portion_index;
                    }
                }
                strands[portion_index].graphs = position;
                strands[portion_index].strand_depth = max_strand_depth + 1;
                result->body.flags = MVM_STRING_TYPE_ROPE;
                result->body.num_strands = portion_index;
            }
            else {
                /* leave type default of int32 and graphs 0 */
            }
        });
    });
    });

    /* assertion/check */
    if (NUM_GRAPHS(result) != position)
        MVM_exception_throw_adhoc(tc, "join had an internal error");

    MVM_string_flatten(tc, result);

    return result;
}

typedef struct MVMCharAtState {
    MVMCodepoint32 search;
    MVMStringIndex result;
} MVMCharAtState;

#define substring_consumer_iterate(member, size) \
size *i; \
for (i = string->body.member + start; i < string->body.member + start + length; ) { \
    if (*i++ == state->search) { \
        state->result = top_index + (MVMStringIndex)(i - (string->body.member + start) - 1); \
        return 1; \
    } \
} \
break; \


MVM_SUBSTRING_CONSUMER(MVM_string_char_at_consumer) {
    MVMCharAtState *state = (MVMCharAtState *)data;
    switch (STR_FLAGS(string)) {
        case MVM_STRING_TYPE_INT32: {
            substring_consumer_iterate(int32s, MVMCodepoint32);
        }
        case MVM_STRING_TYPE_UINT8: {
            substring_consumer_iterate(uint8s, MVMCodepoint8);
        }
        default:
            MVM_exception_throw_adhoc(tc, "internal string corruption");
    }
    return 0;
}

/* returning nonzero means it found the char at the position specified in 'a' in 'b'.
    For character enumerations in regexes.  */
MVMint64 MVM_string_char_at_in_string(MVMThreadContext *tc, MVMString *a, MVMint64 offset, MVMString *b) {
    MVMuint32 index;
    MVMCharAtState state;

    /* We return -2 here only to be able to distinguish between "out of bounds" and "not in string". */
    if (offset < 0 || offset >= NUM_GRAPHS(a))
        return -2;

    state.search = MVM_string_get_codepoint_at_nocheck(tc, a, offset);
    state.result = -1;

    MVM_string_traverse_substring(tc, b, 0, NUM_GRAPHS(b), 0,
        MVM_string_char_at_consumer, &state);
    return state.result;
}

MVMint64 MVM_string_offset_has_unicode_property_value(MVMThreadContext *tc, MVMString *s, MVMint64 offset, MVMint64 property_code, MVMint64 property_value_code) {

    if (!IS_CONCRETE((MVMObject *)s)) {
        MVM_exception_throw_adhoc(tc, "uniprop lookup needs a concrete string");
    }

    if (offset < 0 || offset >= NUM_GRAPHS(s))
        return 0;

    return MVM_unicode_codepoint_has_property_value(tc,
        MVM_string_get_codepoint_at_nocheck(tc, s, offset), property_code, property_value_code);
}

/* internal function so hashes can easily compute hashes of hash keys */
void MVM_string_flatten(MVMThreadContext *tc, MVMString *s) {
    /* XXX This is temporary until we can get the hashing mechanism
        to compute the hash (and test for equivalence!) using the
        codepoint iterator interface.  It's not thread-safe. */
    MVMStringIndex position = 0, sgraphs = NUM_GRAPHS(s);
    void *storage = s->body.storage;
    MVMCodepoint32 *buffer;
    if (IS_WIDE(s))
        return;
    if (!sgraphs) {
        if (storage) free(storage);
        s->body.int32s = malloc(1); /* just in case a hash tries to hash */
        s->body.flags = MVM_STRING_TYPE_INT32;
        return;
    }
    buffer = malloc(sizeof(MVMCodepoint32) * sgraphs);
    if (IS_ROPE(s) && STRAND_DEPTH(s) == 1) {
        MVMStrandIndex strand;
        MVMStrandIndex last  = s->body.num_strands;
        for (strand = 0; strand < last; strand++) {
            MVMStrand *snd = &s->body.strands[strand];
            MVMStringIndex end = (s->body.strands[strand + 1].compare_offset - snd->compare_offset);
            if (IS_WIDE(snd->string)) {
                memcpy(buffer + snd->compare_offset,
                       snd->string->body.int32s + snd->string_offset,
                       end * 4);
            } else if (IS_ASCII(snd->string)) {
                MVMStringIndex copy_idx;
                // we have to turn uint8 into int32 here.
                for (copy_idx = 0; copy_idx < end; copy_idx++) {
                    buffer[snd->compare_offset + copy_idx] = snd->string->body.uint8s[copy_idx + snd->string_offset];
                }
            } else {
                MVM_exception_throw_adhoc(tc, "Unexpected rope in string_flatten.");
            }
        }
    } else {
        for (; position < sgraphs; position++) {
                /* XXX make this use the iterator */
            buffer[position] = MVM_string_get_codepoint_at_nocheck(tc, s, position);
        }
    }
    s->body.flags = MVM_STRING_TYPE_INT32;
    s->body.graphs = sgraphs;
    s->body.int32s = buffer;
    if (storage) free(storage); /* not thread-safe */
}

/* Escapes a string, replacing various chars like \n with \\n. Can no doubt be
 * further optimized. */
MVMString * MVM_string_escape(MVMThreadContext *tc, MVMString *s) {
    MVMString      *res     = NULL;
    MVMStringIndex  sgraphs = NUM_GRAPHS(s);
    MVMStringIndex  spos    = 0;
    MVMStringIndex  balloc  = sgraphs;
    MVMCodepoint32 *buffer  = malloc(sizeof(MVMCodepoint32) * balloc);
    MVMStringIndex  bpos    = 0;

    for (; spos < sgraphs; spos++) {
        MVMCodepoint32 cp = MVM_string_get_codepoint_at_nocheck(tc, s, spos);
        MVMCodepoint32 esc = 0;
        switch (cp) {
            case '\\': esc = '\\'; break;
            case 7:    esc = 'a';  break;
            case '\b': esc = 'b';  break;
            case '\n': esc = 'n';  break;
            case '\r': esc = 'r';  break;
            case '\t': esc = 't';  break;
            case '\f': esc = 'f';  break;
            case '"':  esc = '"';  break;
            case 27:   esc = 'e';  break;
        }
        if (esc) {
            if (bpos + 2 > balloc) {
                balloc += 32;
                buffer = realloc(buffer, sizeof(MVMCodepoint32) * balloc);
            }
            buffer[bpos++] = '\\';
            buffer[bpos++] = esc;
        }
        else {
            if (bpos + 1 > balloc) {
                balloc += 32;
                buffer = realloc(buffer, sizeof(MVMCodepoint32) * balloc);
            }
            buffer[bpos++] = cp;
        }
    }

    res = (MVMString *)MVM_repr_alloc_init(tc, tc->instance->VMString);
    res->body.flags = MVM_STRING_TYPE_INT32;
    res->body.graphs = bpos;
    res->body.int32s = buffer;

    return res;
}

/* Takes a string and reverses its characters. */
MVMString * MVM_string_flip(MVMThreadContext *tc, MVMString *s) {
    MVMString      *res     = NULL;
    MVMStringIndex  sgraphs = NUM_GRAPHS(s);
    MVMStringIndex  spos    = 0;
    MVMCodepoint32 *rbuffer = malloc(sizeof(MVMCodepoint32) * sgraphs);
    MVMStringIndex  rpos    = sgraphs;

    for (; spos < sgraphs; spos++)
        rbuffer[--rpos] = MVM_string_get_codepoint_at_nocheck(tc, s, spos);

    res = (MVMString *)MVM_repr_alloc_init(tc, tc->instance->VMString);
    res->body.flags = MVM_STRING_TYPE_INT32;
    res->body.graphs = sgraphs;
    res->body.int32s = rbuffer;

    return res;
}

/* Compares two strings, returning -1, 0 or 1 to indicate less than,
 * equal or greater than. */
MVMint64 MVM_string_compare(MVMThreadContext *tc, MVMString *a, MVMString *b) {
    MVMStringIndex alen = NUM_GRAPHS(a);
    MVMStringIndex blen = NUM_GRAPHS(b);
    MVMStringIndex i, scanlen;

    /* Simple cases when one or both are zero length. */
    if (alen == 0)
        return blen == 0 ? 0 : -1;
    if (blen == 0)
        return 1;

    /* Otherwise, need to scan them. */
    scanlen = alen > blen ? blen : alen;
    for (i = 0; i < scanlen; i++) {
        MVMCodepoint32 ai = MVM_string_get_codepoint_at_nocheck(tc, a, i);
        MVMCodepoint32 bi = MVM_string_get_codepoint_at_nocheck(tc, b, i);
        if (ai != bi)
            return ai < bi ? -1 : 1;
    }

    /* All shared chars equal, so go on length. */
    return alen < blen ? -1 :
           alen > blen ?  1 :
                          0 ;
}

/* Takes two strings and AND's their charaters. */
MVMString * MVM_string_bitand(MVMThreadContext *tc, MVMString *a, MVMString *b) {
    MVMString         *res = NULL;
    MVMStringIndex    alen = NUM_GRAPHS(a);
    MVMStringIndex    blen = NUM_GRAPHS(b);
    MVMStringIndex sgraphs = alen < blen ? alen : blen;
    MVMCodepoint32 *buffer = malloc(sizeof(MVMCodepoint32) * sgraphs);
    MVMStringIndex i, scanlen;

    /* Binary-and up to the length of the shortest string. */
    for (i = 0; i < sgraphs; i++)
        buffer[i] = (MVM_string_get_codepoint_at_nocheck(tc, a, i)
                   & MVM_string_get_codepoint_at_nocheck(tc, b, i));

    res = (MVMString *)MVM_repr_alloc_init(tc, tc->instance->VMString);
    res->body.flags = MVM_STRING_TYPE_INT32;
    res->body.graphs = sgraphs;
    res->body.int32s = buffer;

    return res;
}

/* Takes two strings and OR's their charaters. */
MVMString * MVM_string_bitor(MVMThreadContext *tc, MVMString *a, MVMString *b) {
    MVMString         *res = NULL;
    MVMStringIndex    alen = NUM_GRAPHS(a);
    MVMStringIndex    blen = NUM_GRAPHS(b);
    MVMStringIndex sgraphs = (alen > blen ? alen : blen);
    MVMCodepoint32 *buffer = malloc(sizeof(MVMCodepoint32) * sgraphs);
    MVMStringIndex i, scanlen;

    /* First, binary-or up to the length of the shortest string. */
    scanlen = alen > blen ? blen : alen;
    for (i = 0; i < scanlen; i++)
        buffer[i] = (MVM_string_get_codepoint_at_nocheck(tc, a, i)
                   | MVM_string_get_codepoint_at_nocheck(tc, b, i));

    /* Second pass, fill with characters of the longest string. */
    if (alen > blen)
        for (; i < sgraphs; i++)
            buffer[i] = MVM_string_get_codepoint_at_nocheck(tc, a, i);
    else
        for (; i < sgraphs; i++)
            buffer[i] = MVM_string_get_codepoint_at_nocheck(tc, b, i);

    res = (MVMString *)MVM_repr_alloc_init(tc, tc->instance->VMString);
    res->body.flags = MVM_STRING_TYPE_INT32;
    res->body.graphs = sgraphs;
    res->body.int32s = buffer;

    return res;
}

/* Takes two strings and XOR's their charaters. */
MVMString * MVM_string_bitxor(MVMThreadContext *tc, MVMString *a, MVMString *b) {
    MVMString         *res = NULL;
    MVMStringIndex    alen = NUM_GRAPHS(a);
    MVMStringIndex    blen = NUM_GRAPHS(b);
    MVMStringIndex sgraphs = (alen > blen ? alen : blen);
    MVMCodepoint32 *buffer = malloc(sizeof(MVMCodepoint32) * sgraphs);
    MVMStringIndex i, scanlen;

    /* First, binary-xor up to the length of the shorter string. */
    scanlen = alen > blen ? blen : alen;
    for (i = 0; i < scanlen; i++)
        buffer[i] = (MVM_string_get_codepoint_at_nocheck(tc, a, i)
                   ^ MVM_string_get_codepoint_at_nocheck(tc, b, i));

    /* Second pass, fill with characters of the longest string. */
    if (alen > blen)
        for (; i < sgraphs; i++)
            buffer[i] = MVM_string_get_codepoint_at_nocheck(tc, a, i);
    else
        for (; i < sgraphs; i++)
            buffer[i] = MVM_string_get_codepoint_at_nocheck(tc, b, i);

    res = (MVMString *)MVM_repr_alloc_init(tc, tc->instance->VMString);
    res->body.flags = MVM_STRING_TYPE_INT32;
    res->body.graphs = sgraphs;
    res->body.int32s = buffer;

    return res;
}

/* The following statics hold on to various unicode property values we will
 * resolve once so we don't have to do it repeatedly. */
static MVMint64 UPV_Nd = 0;
static MVMint64 UPV_Lu = 0;
static MVMint64 UPV_Ll = 0;
static MVMint64 UPV_Lt = 0;
static MVMint64 UPV_Lm = 0;
static MVMint64 UPV_Lo = 0;
static MVMint64 UPV_Zs = 0;
static MVMint64 UPV_Zl = 0;
static MVMint64 UPV_Pc = 0;
static MVMint64 UPV_Pd = 0;
static MVMint64 UPV_Ps = 0;
static MVMint64 UPV_Pe = 0;
static MVMint64 UPV_Pi = 0;
static MVMint64 UPV_Pf = 0;
static MVMint64 UPV_Po = 0;

/* Resolves various unicode property values that we'll need. */
void MVM_string_cclass_init(MVMThreadContext *tc) {
    UPV_Nd = MVM_unicode_name_to_property_value_code(tc,
        MVM_UNICODE_PROPERTY_GENERAL_CATEGORY,
        MVM_string_ascii_decode_nt(tc, tc->instance->VMString, "Nd"));
    UPV_Lu = MVM_unicode_name_to_property_value_code(tc,
        MVM_UNICODE_PROPERTY_GENERAL_CATEGORY,
        MVM_string_ascii_decode_nt(tc, tc->instance->VMString, "Lu"));
    UPV_Ll = MVM_unicode_name_to_property_value_code(tc,
        MVM_UNICODE_PROPERTY_GENERAL_CATEGORY,
        MVM_string_ascii_decode_nt(tc, tc->instance->VMString, "Ll"));
    UPV_Lt = MVM_unicode_name_to_property_value_code(tc,
        MVM_UNICODE_PROPERTY_GENERAL_CATEGORY,
        MVM_string_ascii_decode_nt(tc, tc->instance->VMString, "Lt"));
    UPV_Lm = MVM_unicode_name_to_property_value_code(tc,
        MVM_UNICODE_PROPERTY_GENERAL_CATEGORY,
        MVM_string_ascii_decode_nt(tc, tc->instance->VMString, "Lm"));
    UPV_Lo = MVM_unicode_name_to_property_value_code(tc,
        MVM_UNICODE_PROPERTY_GENERAL_CATEGORY,
        MVM_string_ascii_decode_nt(tc, tc->instance->VMString, "Lo"));
    UPV_Zs = MVM_unicode_name_to_property_value_code(tc,
        MVM_UNICODE_PROPERTY_GENERAL_CATEGORY,
        MVM_string_ascii_decode_nt(tc, tc->instance->VMString, "Zs"));
    UPV_Zl = MVM_unicode_name_to_property_value_code(tc,
        MVM_UNICODE_PROPERTY_GENERAL_CATEGORY,
        MVM_string_ascii_decode_nt(tc, tc->instance->VMString, "Zl"));
    UPV_Pc = MVM_unicode_name_to_property_value_code(tc,
        MVM_UNICODE_PROPERTY_GENERAL_CATEGORY,
        MVM_string_ascii_decode_nt(tc, tc->instance->VMString, "Pc"));
    UPV_Pd = MVM_unicode_name_to_property_value_code(tc,
        MVM_UNICODE_PROPERTY_GENERAL_CATEGORY,
        MVM_string_ascii_decode_nt(tc, tc->instance->VMString, "Pd"));
    UPV_Ps = MVM_unicode_name_to_property_value_code(tc,
        MVM_UNICODE_PROPERTY_GENERAL_CATEGORY,
        MVM_string_ascii_decode_nt(tc, tc->instance->VMString, "Ps"));
    UPV_Pe = MVM_unicode_name_to_property_value_code(tc,
        MVM_UNICODE_PROPERTY_GENERAL_CATEGORY,
        MVM_string_ascii_decode_nt(tc, tc->instance->VMString, "Pe"));
    UPV_Pi = MVM_unicode_name_to_property_value_code(tc,
        MVM_UNICODE_PROPERTY_GENERAL_CATEGORY,
        MVM_string_ascii_decode_nt(tc, tc->instance->VMString, "Pi"));
    UPV_Pf = MVM_unicode_name_to_property_value_code(tc,
        MVM_UNICODE_PROPERTY_GENERAL_CATEGORY,
        MVM_string_ascii_decode_nt(tc, tc->instance->VMString, "Pf"));
    UPV_Po = MVM_unicode_name_to_property_value_code(tc,
        MVM_UNICODE_PROPERTY_GENERAL_CATEGORY,
        MVM_string_ascii_decode_nt(tc, tc->instance->VMString, "Po"));
}

/* Checks if the character at the specified offset is a member of the
 * indicated character class. */
MVMint64 MVM_string_is_cclass(MVMThreadContext *tc, MVMint64 cclass, MVMString *s, MVMint64 offset) {
    if (offset < 0 || offset >= NUM_GRAPHS(s))
        return 0;

    switch (cclass) {
        case MVM_CCLASS_ANY:
            return 1;

        case MVM_CCLASS_UPPERCASE:
            return MVM_string_offset_has_unicode_property_value(tc, s, offset,
                MVM_UNICODE_PROPERTY_GENERAL_CATEGORY, UPV_Lu);

        case MVM_CCLASS_LOWERCASE:
            return MVM_string_offset_has_unicode_property_value(tc, s, offset,
                MVM_UNICODE_PROPERTY_GENERAL_CATEGORY, UPV_Ll);

        case MVM_CCLASS_WORD:
            if (MVM_string_get_codepoint_at(tc, s, offset) == '_')
                return 1;
            /* Deliberate fall-through; word is _ or digit or alphabetic. */

        case MVM_CCLASS_ALPHANUMERIC:
            if (MVM_string_offset_has_unicode_property_value(tc, s, offset,
                    MVM_UNICODE_PROPERTY_GENERAL_CATEGORY, UPV_Nd))
                return 1;
            /* Deliberate fall-through; alphanumeric is digit or alphabetic. */

        case MVM_CCLASS_ALPHABETIC:
            return
                MVM_string_offset_has_unicode_property_value(tc, s, offset,
                    MVM_UNICODE_PROPERTY_GENERAL_CATEGORY, UPV_Ll)
             || MVM_string_offset_has_unicode_property_value(tc, s, offset,
                    MVM_UNICODE_PROPERTY_GENERAL_CATEGORY, UPV_Lu)
             || MVM_string_offset_has_unicode_property_value(tc, s, offset,
                    MVM_UNICODE_PROPERTY_GENERAL_CATEGORY, UPV_Lt)
             || MVM_string_offset_has_unicode_property_value(tc, s, offset,
                    MVM_UNICODE_PROPERTY_GENERAL_CATEGORY, UPV_Lm)
             || MVM_string_offset_has_unicode_property_value(tc, s, offset,
                    MVM_UNICODE_PROPERTY_GENERAL_CATEGORY, UPV_Lo);

        case MVM_CCLASS_NUMERIC:
            return MVM_string_offset_has_unicode_property_value(tc, s, offset,
                MVM_UNICODE_PROPERTY_GENERAL_CATEGORY, UPV_Nd);

        case MVM_CCLASS_HEXADECIMAL:
            return MVM_string_offset_has_unicode_property_value(tc, s, offset,
                MVM_UNICODE_PROPERTY_ASCII_HEX_DIGIT, 1);

        case MVM_CCLASS_WHITESPACE:
            return MVM_string_offset_has_unicode_property_value(tc, s, offset,
                MVM_UNICODE_PROPERTY_WHITE_SPACE, 1);

        case MVM_CCLASS_BLANK:
            if (MVM_string_get_codepoint_at(tc, s, offset) == '\t')
                return 1;
            return MVM_string_offset_has_unicode_property_value(tc, s, offset,
                MVM_UNICODE_PROPERTY_GENERAL_CATEGORY, UPV_Zs);

        case MVM_CCLASS_CONTROL: {
            MVMCodepoint32 cp = MVM_string_get_codepoint_at(tc, s, offset);
            return (cp >= 0 && cp < 32) || (cp >= 127 && cp < 160);
        }

        case MVM_CCLASS_PRINTING: {
            MVMCodepoint32 cp = MVM_string_get_codepoint_at(tc, s, offset);
            return !(cp >= 0 && cp < 32) || (cp >= 127 && cp < 160);
        }

        case MVM_CCLASS_PUNCTUATION:
            return
                MVM_string_offset_has_unicode_property_value(tc, s, offset,
                    MVM_UNICODE_PROPERTY_GENERAL_CATEGORY, UPV_Pc)
             || MVM_string_offset_has_unicode_property_value(tc, s, offset,
                    MVM_UNICODE_PROPERTY_GENERAL_CATEGORY, UPV_Pd)
             || MVM_string_offset_has_unicode_property_value(tc, s, offset,
                    MVM_UNICODE_PROPERTY_GENERAL_CATEGORY, UPV_Ps)
             || MVM_string_offset_has_unicode_property_value(tc, s, offset,
                    MVM_UNICODE_PROPERTY_GENERAL_CATEGORY, UPV_Pe)
             || MVM_string_offset_has_unicode_property_value(tc, s, offset,
                    MVM_UNICODE_PROPERTY_GENERAL_CATEGORY, UPV_Pi)
             || MVM_string_offset_has_unicode_property_value(tc, s, offset,
                    MVM_UNICODE_PROPERTY_GENERAL_CATEGORY, UPV_Pf)
             || MVM_string_offset_has_unicode_property_value(tc, s, offset,
                    MVM_UNICODE_PROPERTY_GENERAL_CATEGORY, UPV_Po);

        case MVM_CCLASS_NEWLINE: {
            MVMCodepoint32 cp = MVM_string_get_codepoint_at(tc, s, offset);
            if (cp == '\n' || cp == '\r' || cp == 0x85)
                return 1;
            return MVM_string_offset_has_unicode_property_value(tc, s, offset,
                MVM_UNICODE_PROPERTY_GENERAL_CATEGORY, UPV_Zl);
        }

        default:
            return 0;
    }
}

/* Searches for the next char that is in the specified character class. */
MVMint64 MVM_string_find_cclass(MVMThreadContext *tc, MVMint64 cclass, MVMString *s, MVMint64 offset, MVMint64 count) {
    MVMint64 length = NUM_GRAPHS(s);
    MVMint64 end    = offset + count;
    MVMint64 pos;

    end = length < end ? length : end;

    for (pos = offset; pos < end; pos++)
        if (MVM_string_is_cclass(tc, cclass, s, pos) > 0)
            return pos;

    return end;
}

/* Searches for the next char that is not in the specified character class. */
MVMint64 MVM_string_find_not_cclass(MVMThreadContext *tc, MVMint64 cclass, MVMString *s, MVMint64 offset, MVMint64 count) {
    MVMint64 length = NUM_GRAPHS(s);
    MVMint64 end    = offset + count;
    MVMint64 pos;

    end = length < end ? length : end;

    for (pos = offset; pos < end; pos++)
        if (MVM_string_is_cclass(tc, cclass, s, pos) == 0)
            return pos;

    return end;
}

static MVMint16   encoding_name_init         = 0;
static MVMString *encoding_utf8_name         = NULL;
static MVMString *encoding_ascii_name        = NULL;
static MVMString *encoding_latin1_name       = NULL;
static MVMString *encoding_utf16_name        = NULL;
static MVMString *encoding_windows1252_name  = NULL;
MVMuint8 MVM_string_find_encoding(MVMThreadContext *tc, MVMString *name) {
    if (!encoding_name_init) {
        MVM_gc_allocate_gen2_default_set(tc);
        encoding_utf8_name        = MVM_string_ascii_decode_nt(tc, tc->instance->VMString, "utf8");
        MVM_gc_root_add_permanent(tc, (MVMCollectable **)&encoding_utf8_name);
        encoding_ascii_name       = MVM_string_ascii_decode_nt(tc, tc->instance->VMString, "ascii");
        MVM_gc_root_add_permanent(tc, (MVMCollectable **)&encoding_ascii_name);
        encoding_latin1_name      = MVM_string_ascii_decode_nt(tc, tc->instance->VMString, "iso-8859-1");
        MVM_gc_root_add_permanent(tc, (MVMCollectable **)&encoding_latin1_name);
        encoding_utf16_name       = MVM_string_ascii_decode_nt(tc, tc->instance->VMString, "utf16");
        MVM_gc_root_add_permanent(tc, (MVMCollectable **)&encoding_utf16_name);
        encoding_windows1252_name = MVM_string_ascii_decode_nt(tc, tc->instance->VMString, "windows-1252");
        MVM_gc_root_add_permanent(tc, (MVMCollectable **)&encoding_windows1252_name);
        encoding_name_init   = 1;
        MVM_gc_allocate_gen2_default_clear(tc);
    }

    if (MVM_string_equal(tc, name, encoding_utf8_name)) {
        return MVM_encoding_type_utf8;
    }
    else if (MVM_string_equal(tc, name, encoding_ascii_name)) {
        return MVM_encoding_type_ascii;
    }
    else if (MVM_string_equal(tc, name, encoding_latin1_name)) {
        return MVM_encoding_type_latin1;
    }
    else if (MVM_string_equal(tc, name, encoding_utf16_name)) {
        return MVM_encoding_type_utf16;
    }
    else {
        MVM_exception_throw_adhoc(tc, "Unknown string encoding: '%s'",
            MVM_string_utf8_encode_C_string(tc, name));
    }
}
