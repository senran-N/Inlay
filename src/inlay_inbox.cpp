#include "inlay_internal.h"
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_mutex.h>
#include <cstdio>

namespace {
constexpr uint32_t kMaxMessages = 4096u;
constexpr uint32_t kMaxBytes = 16u * 1024u * 1024u;
constexpr uint32_t kDefaultDrainBudget = 64u;

struct Entry {
 uint32_t kind, id, size;
};

bool is_valid_utf8(const char* s, uint32_t len) noexcept {
 uint32_t i = 0u;
 while (i < len) {
  const uint8_t b0 = static_cast<uint8_t>(s[i]);
  if (b0 < 0x80u) {
   ++i;
   continue;
  }
  uint32_t n = 0u;
  uint32_t cp = 0u;
  uint32_t min_cp = 0u;
  if (b0 >= 0xC2u && b0 <= 0xDFu) {
   n = 2u; cp = b0 & 0x1Fu; min_cp = 0x80u;
  } else if (b0 >= 0xE0u && b0 <= 0xEFu) {
   n = 3u; cp = b0 & 0x0Fu; min_cp = 0x800u;
  } else if (b0 >= 0xF0u && b0 <= 0xF4u) {
   n = 4u; cp = b0 & 0x07u; min_cp = 0x10000u;
  } else {
   return false;
  }
  if (i + n > len) return false;
  for (uint32_t k = 1u; k < n; ++k) {
   const uint8_t bx = static_cast<uint8_t>(s[i + k]);
   if ((bx & 0xC0u) != 0x80u) return false;
   cp = (cp << 6) | static_cast<uint32_t>(bx & 0x3Fu);
  }
  if (cp < min_cp || cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) return false;
  i += n;
 }
 return true;
}

uint32_t ring_copy(char* dest, const char* src, uint32_t capacity, uint32_t offset,
                   uint32_t size, bool into_ring) noexcept {
 const uint32_t first = size < capacity - offset ? size : capacity - offset;
 if (into_ring) {
  std::memcpy(dest + offset, src, first);
  std::memcpy(dest, src + first, size - first);
 } else {
  std::memcpy(dest, src + offset, first);
  std::memcpy(dest + first, src, size - first);
 }
 return (offset + size) % capacity;
}
} // namespace

struct InlayInbox {
 InlayInboxDesc desc;
 SDL_Mutex* mutex;
 Entry* entries;
 char* bytes;
 uint32_t entry_read, count, read, write, used;
 bool closed;
};

extern "C" {
INLAY_API InlayInboxDesc inlay_inbox_desc_default(void) {
 return {sizeof(InlayInboxDesc), 64u, 256u * 1024u, 0u};
}

INLAY_API InlayInbox* inlay_inbox_create(const InlayInboxDesc* input) {
 InlayInboxDesc desc = inlay_inbox_desc_default();
 if (input && !inlay_read_abi_struct(input, &desc)) return nullptr;
 if (!desc.max_messages || desc.max_messages > kMaxMessages || !desc.max_bytes || desc.max_bytes > kMaxBytes ||
     (desc.wake_event_type && (desc.wake_event_type < SDL_EVENT_USER || desc.wake_event_type >= SDL_EVENT_LAST))) return nullptr;
 auto* inbox = static_cast<InlayInbox*>(SDL_calloc(1u, sizeof(InlayInbox)));
 if (!inbox) return nullptr;
 inbox->desc = desc;
 inbox->mutex = SDL_CreateMutex();
 inbox->entries = static_cast<Entry*>(SDL_malloc(desc.max_messages * sizeof(Entry)));
 if (!inbox->mutex || !inbox->entries) {
  inlay_inbox_destroy(inbox);
  return nullptr;
 }
 return inbox;
}

INLAY_API InlayInboxStatus inlay_inbox_post(InlayInbox* inbox, uint32_t kind, uint32_t id,
                                         const void* payload, uint32_t size) {
 if (!inbox || (!payload && size)) return INLAY_INBOX_INVALID;
 SDL_LockMutex(inbox->mutex);
 if (inbox->closed) {
  SDL_UnlockMutex(inbox->mutex);
  return INLAY_INBOX_CLOSED;
 }
 const uint32_t limit = inbox->desc.max_bytes;
 if (inbox->count == inbox->desc.max_messages || size > limit - inbox->used) {
  SDL_UnlockMutex(inbox->mutex);
  return INLAY_INBOX_FULL;
 }
 if (size && !inbox->bytes) {
  inbox->bytes = static_cast<char*>(SDL_malloc(limit));
  if (!inbox->bytes) {
   SDL_UnlockMutex(inbox->mutex);
   return INLAY_INBOX_NO_MEMORY;
  }
 }
 if (size) {
  inbox->write = ring_copy(inbox->bytes, static_cast<const char*>(payload), limit, inbox->write, size, true);
  inbox->used += size;
 }
 const uint32_t slot = (inbox->entry_read + inbox->count) % inbox->desc.max_messages;
 inbox->entries[slot] = {kind, id, size};
 const uint32_t wake = inbox->count++ == 0u ? inbox->desc.wake_event_type : 0u;
 SDL_UnlockMutex(inbox->mutex);
 if (wake) {
  SDL_Event event = {};
  event.type = wake;
  SDL_PushEvent(&event);
 }
 return INLAY_INBOX_OK;
}

INLAY_API InlayInboxStatus inlay_inbox_take(InlayInbox* inbox, InlayInboxMessage* message,
                                         void* payload, uint32_t capacity) {
 if (!inbox || !message || (!payload && capacity)) return INLAY_INBOX_INVALID;
 *message = {.struct_size = sizeof(InlayInboxMessage), .kind = 0u, .id = 0u, .payload_size = 0u};
 SDL_LockMutex(inbox->mutex);
 if (inbox->count == 0u) {
  const auto result = inbox->closed ? INLAY_INBOX_CLOSED : INLAY_INBOX_EMPTY;
  SDL_UnlockMutex(inbox->mutex);
  return result;
 }
 const Entry& entry = inbox->entries[inbox->entry_read];
 *message = {sizeof(InlayInboxMessage), entry.kind, entry.id, entry.size};
 if (capacity < entry.size) {
  SDL_UnlockMutex(inbox->mutex);
  return INLAY_INBOX_BUFFER_TOO_SMALL;
 }
 if (entry.size) {
  inbox->read = ring_copy(static_cast<char*>(payload), inbox->bytes, inbox->desc.max_bytes, inbox->read, entry.size, false);
  inbox->used -= entry.size;
 }
 inbox->entry_read = (inbox->entry_read + 1u) % inbox->desc.max_messages;
 --inbox->count;
 SDL_UnlockMutex(inbox->mutex);
 return INLAY_INBOX_OK;
}

INLAY_API uint32_t inlay_inbox_pending(InlayInbox* inbox) {
 if (!inbox) return 0u;
 SDL_LockMutex(inbox->mutex);
 const uint32_t count = inbox->count;
 SDL_UnlockMutex(inbox->mutex);
 return count;
}

INLAY_API void inlay_inbox_close(InlayInbox* inbox) {
 if (!inbox) return;
 SDL_LockMutex(inbox->mutex);
 inbox->closed = true;
 SDL_UnlockMutex(inbox->mutex);
}

INLAY_API void inlay_inbox_destroy(InlayInbox* inbox) {
 if (!inbox) return;
 SDL_DestroyMutex(inbox->mutex);
 SDL_free(inbox->bytes);
 SDL_free(inbox->entries);
 SDL_free(inbox);
}

INLAY_API uint32_t inlay_inbox_drain_to_surface(InlayInbox* inbox, InlaySurface* surface,
                                                uint32_t max_messages) {
 if (!inbox || !surface) return 0u;
 const uint32_t budget = max_messages ? max_messages : kDefaultDrainBudget;
 char stack_buf[4096];
 char* heap_buf = nullptr;
 uint32_t heap_cap = 0u;
 uint32_t drained = 0u;

 while (drained < budget) {
  InlayInboxMessage msg = {};
  char* payload_ptr = stack_buf;
  InlayInboxStatus st = inlay_inbox_take(inbox, &msg, stack_buf, static_cast<uint32_t>(sizeof(stack_buf)));
  if (st == INLAY_INBOX_BUFFER_TOO_SMALL) {
   if (msg.payload_size > heap_cap) {
    auto* mem = static_cast<char*>(SDL_realloc(heap_buf, msg.payload_size));
    if (!mem) break;
    heap_buf = mem;
    heap_cap = msg.payload_size;
   }
   payload_ptr = heap_buf;
   st = inlay_inbox_take(inbox, &msg, heap_buf, heap_cap);
  }
  if (st != INLAY_INBOX_OK) break;
  ++drained;

  if (msg.payload_size == 0u || is_valid_utf8(payload_ptr, msg.payload_size)) {
   char name_buf[32] = {};
   EtchStr event_name = ETCH_STR_LIT("inbox");
   if (msg.kind != 0u) {
    const int n = std::snprintf(name_buf, sizeof(name_buf), "inbox:%u", static_cast<unsigned int>(msg.kind));
    if (n > 0) {
     event_name = {name_buf, static_cast<uint32_t>(n), false};
    }
   }
   const EtchStr payload = msg.payload_size > 0u
    ? EtchStr{payload_ptr, msg.payload_size, false}
    : ETCH_STR_LIT("{}");
   inlay_surface_dispatch(surface, event_name, payload);
  }
 }

 SDL_free(heap_buf);
 return drained;
}
}
