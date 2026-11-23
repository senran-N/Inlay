#include "inlay_internal.h"
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_mutex.h>

namespace {
constexpr uint32_t kMaxMessages = 4096u;
constexpr uint32_t kMaxBytes = 16u * 1024u * 1024u;

struct Entry {
 uint32_t kind, id, size;
};

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
}
