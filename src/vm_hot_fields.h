// Hints live only inside an allocation-free interpreter invocation. Validate
// receiver/layout before using an entry; no cache survives a GC or host call.
typedef struct {
    size_t pc;
    LhatFieldFold fold;
    LhatTable *table;
    uint32_t version;
    LhatTableEntry *entry, *right_entry;
    size_t capacity, index, right_index;
} LhatHotFieldCache;

static void vm_hot_fields_init(const LhatChunk *chunk, size_t pc,
                                LhatHotFieldCache cache[2])
{
    for (size_t i = 0; i < 2; ++i) {
        memset(&cache[i], 0, sizeof cache[i]);
        cache[i].pc = SIZE_MAX;
    }
    size_t count = 0;
    for (size_t i = pc; i < chunk->count && count < 2; ++i) {
        if (lhat_chunk_field_fold(chunk, i, &cache[count].fold)) {
            cache[count].pc = i;
            count++;
        }
    }
}

#if defined(_MSC_VER)
__declspec(noinline)
#else
__attribute__((noinline))
#endif
static bool vm_hot_field_step(Machine *m, LhatSlots *slots,
                              LhatHotFieldCache *hit)
{
    const LhatFieldFold *fold = &hit->fold;
    LhatValue owner = lhat_slots_get(*slots, fold->source);
    if (!lhat_is_object_kind(owner, LHAT_OBJECT_TABLE)) return false;
    LhatTable *table = (LhatTable *)lhat_as_object(owner);
    if (table->sealed || table->shared) return false;
    if (hit->table != table || hit->version != table->version) {
        // Different records often share the same key layout. Reuse entry
        // indices only after checking capacity and the actual keys in this
        // receiver; a collision, deletion or different layout takes a probe.
        LhatTableEntry *entry = NULL, *right_entry = NULL;
        if (hit->entry != NULL && hit->capacity == table->entry_capacity) {
            entry = &table->entries[hit->index];
            if (entry->key.tag != fold->key_value->tag ||
                entry->key.as.object != fold->key_value->as.object)
                entry = NULL;
            if (entry != NULL && fold->span == 7) {
                right_entry = &table->entries[hit->right_index];
                if (hit->right_entry == NULL || right_entry->key.tag != fold->right->tag ||
                    right_entry->key.as.object != fold->right->as.object)
                    entry = NULL;
            }
        }
        if (entry == NULL) {
            entry = vm_own_string_entry(table, *fold->key_value);
            right_entry = fold->span == 7 ? vm_own_string_entry(table, *fold->right) : NULL;
        }
        hit->entry = entry;
        hit->right_entry = right_entry;
        hit->capacity = table->entry_capacity;
        hit->index = entry == NULL ? 0 : (size_t)(entry - table->entries);
        hit->right_index = right_entry == NULL ? 0 : (size_t)(right_entry - table->entries);
        hit->table = table;
        hit->version = table->version;
    }
    if (hit->entry == NULL || (fold->span == 7 && hit->right_entry == NULL)) return false;
    LhatValue left = hit->entry->value;
    LhatValue right = fold->span == 7 ? hit->right_entry->value : *fold->right;
    if (!lhat_is_integer(left) || !lhat_is_integer(right)) return false;
    LhatInteger result;
#if defined(__clang__) || defined(__GNUC__)
    if (__builtin_add_overflow(lhat_as_integer(left), lhat_as_integer(right), &result)) return false;
#else
    if (!add_exact(lhat_as_integer(left), lhat_as_integer(right), &result)) return false;
#endif
    LhatValue answer = lhat_integer(result);
    // Preserve every register write made by the original sequence, including
    // scratch roots. The guarded sequence cannot allocate or lend a receiver.
    lhat_slots_set(*slots, fold->owner, owner);
    lhat_slots_set(*slots, fold->key, *fold->key_value);
    lhat_slots_set(*slots, fold->value, answer);
    if (fold->span == 7) {
        lhat_slots_set(*slots, fold->rhs_owner, owner);
        lhat_slots_set(*slots, fold->rhs_value, right);
    }
    hit->entry->key = *fold->key_value;
    hit->entry->value = answer;
    vm_table_write_barrier(m, table, *fold->key_value, answer);
    return true;
}
