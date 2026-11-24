#include "inlay_internal.h"
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_mutex.h>
#include <SDL3/SDL_thread.h>
#include <cstdlib>
#include <cstring>

namespace {
struct ImageJob {
 uint64_t key_hash;
 uint64_t generation;
 uint32_t slot_index;
 char* path;
 uint8_t* raw_bytes;
 size_t raw_size;
 EtchDecodedImage decoded;
 bool is_file;
 bool srgb;
 bool generate_mipmaps;
 bool ok;
 ImageJob* next;
};

struct ImageEntry {
 bool occupied;
 uint64_t key_hash;
 char* key_str;
 uint32_t key_len;
 InlayImageLoadStatus status;
 uint64_t generation;
 uint64_t lru_clock;
 EtchLoadedTexture tex;
 size_t vram_bytes;
 bool srgb;
};

void free_job(ImageJob* j) noexcept {
 if (!j) return;
 SDL_free(j->path);
 SDL_free(j->raw_bytes);
 if (j->decoded.rgba) etch_free_decoded_image(&j->decoded);
 SDL_free(j);
}

bool bytes_look_like_svg(const uint8_t* data, size_t size) noexcept {
 if (!data || size < 4u) return false;
 size_t p = 0u;
 if (size >= 3u && data[0] == 0xEFu && data[1] == 0xBBu && data[2] == 0xBFu) p = 3u;
 for (int pass = 0; pass < 8 && p < size; ++pass) {
  while (p < size && (data[p] == ' ' || data[p] == '\t' || data[p] == '\r' || data[p] == '\n')) ++p;
  if (p + 2u <= size && data[p] == '<' && data[p + 1u] == '?') {
   p += 2u;
   while (p + 1u < size && !(data[p] == '?' && data[p + 1u] == '>')) ++p;
   if (p + 1u < size) p += 2u;
   else return false;
   continue;
  }
  if (p + 4u <= size && data[p] == '<' && data[p + 1u] == '!' && data[p + 2u] == '-' && data[p + 3u] == '-') {
   p += 4u;
   while (p + 2u < size && !(data[p] == '-' && data[p + 1u] == '-' && data[p + 2u] == '>')) ++p;
   if (p + 2u < size) p += 3u;
   else return false;
   continue;
  }
  if (p + 2u <= size && data[p] == '<' && data[p + 1u] == '!') {
   p += 2u;
   while (p < size && data[p] != '>') ++p;
   if (p < size) ++p;
   else return false;
   continue;
  }
  break;
 }
 if (p + 4u > size || data[p] != '<') return false;
 const auto lo = [](uint8_t c) noexcept -> uint8_t {
  return (c >= 'A' && c <= 'Z') ? uint8_t(c + ('a' - 'A')) : c;
 };
 if (lo(data[p + 1u]) != 's' || lo(data[p + 2u]) != 'v' || lo(data[p + 3u]) != 'g') return false;
 if (p + 4u == size) return true;
 const uint8_t nx = data[p + 4u];
 return nx == ' ' || nx == '\t' || nx == '\r' || nx == '\n' || nx == '>' || nx == '/';
}
} // namespace

struct InlayImageLoader {
 EtchContext* ctx;
 uint32_t max_entries;
 size_t max_vram_bytes;
 size_t current_vram_bytes;
 uint32_t epoch;
 uint64_t lru_clock;
 uint64_t next_gen;
 ImageEntry* entries;
 SDL_Mutex* mutex;
 SDL_Condition* work_cond;
 ImageJob* pending_head;
 ImageJob* pending_tail;
 ImageJob* done_head;
 ImageJob* done_tail;
 bool stop;
 SDL_Thread* worker;
};

namespace {
uint32_t find_entry_locked(const InlayImageLoader* loader, uint64_t key_hash, EtchStr key) noexcept {
 for (uint32_t i = 0u; i < loader->max_entries; ++i) {
  const auto& e = loader->entries[i];
  if (e.occupied && e.key_hash == key_hash && e.key_len == key.length &&
      std::memcmp(e.key_str, key.chars, key.length) == 0) {
   return i;
  }
 }
 return UINT32_MAX;
}

uint32_t alloc_slot_locked(InlayImageLoader* loader, EtchLoadedTexture* out_evicted_tex) noexcept {
 *out_evicted_tex = {};
 for (uint32_t i = 0u; i < loader->max_entries; ++i) {
  if (!loader->entries[i].occupied) return i;
 }
 // Prefer evicting IDLE or ERROR slots first, then oldest LRU READY slot (never evict LOADING).
 uint32_t victim = UINT32_MAX;
 uint64_t oldest_ready = UINT64_MAX;
 for (uint32_t i = 0u; i < loader->max_entries; ++i) {
  const auto& e = loader->entries[i];
  if (e.status == INLAY_IMAGE_STATUS_IDLE || e.status == INLAY_IMAGE_STATUS_ERROR) {
   victim = i;
   break;
  }
  if (e.status == INLAY_IMAGE_STATUS_READY && e.lru_clock < oldest_ready) {
   oldest_ready = e.lru_clock;
   victim = i;
  }
 }
 if (victim == UINT32_MAX) return UINT32_MAX;
 auto& ve = loader->entries[victim];
 if (ve.tex.texture) {
  *out_evicted_tex = ve.tex;
  if (loader->current_vram_bytes >= ve.vram_bytes) loader->current_vram_bytes -= ve.vram_bytes;
  else loader->current_vram_bytes = 0u;
 }
 SDL_free(ve.key_str);
 ve = {};
 return victim;
}

int SDLCALL image_loader_worker(void* user) noexcept {
 auto* loader = static_cast<InlayImageLoader*>(user);
 for (;;) {
  SDL_LockMutex(loader->mutex);
  while (!loader->stop && !loader->pending_head) {
   SDL_WaitCondition(loader->work_cond, loader->mutex);
  }
  if (loader->stop) {
   SDL_UnlockMutex(loader->mutex);
   return 0;
  }
  ImageJob* job = loader->pending_head;
  loader->pending_head = job->next;
  if (!loader->pending_head) loader->pending_tail = nullptr;
  job->next = nullptr;
  bool active = false;
  if (job->slot_index < loader->max_entries) {
   const auto& e = loader->entries[job->slot_index];
   active = e.occupied && e.key_hash == job->key_hash &&
            e.generation == job->generation && e.status == INLAY_IMAGE_STATUS_LOADING;
  }
  SDL_UnlockMutex(loader->mutex);

  if (!active) {
   free_job(job);
   continue;
  }

  uint8_t* raw = job->raw_bytes;
  size_t raw_size = job->raw_size;
  job->raw_bytes = nullptr;
  job->raw_size = 0u;
  if (job->is_file) {
   size_t fsize = 0u;
   void* fdata = job->path ? SDL_LoadFile(job->path, &fsize) : nullptr;
   if (fdata && fsize > 0u) {
    raw = static_cast<uint8_t*>(fdata);
    raw_size = fsize;
   } else {
    SDL_free(fdata);
   }
  }
  SDL_free(job->path);
  job->path = nullptr;

  job->ok = false;
  job->decoded = {};
  job->generate_mipmaps = true;
  if (raw && raw_size > 0u) {
   if (bytes_look_like_svg(raw, raw_size)) {
    EtchSvgRasterDesc sdesc = etch_svg_raster_desc_default();
    sdesc.srgb = job->srgb;
    job->generate_mipmaps = sdesc.generate_mipmaps;
    job->ok = etch_rasterize_svg_memory(raw, raw_size, &sdesc, &job->decoded);
   } else {
    job->ok = etch_decode_image_memory(raw, raw_size, &job->decoded);
   }
   SDL_free(raw);
   raw = nullptr;
   if (job->ok && (!job->decoded.rgba || job->decoded.width == 0u || job->decoded.height == 0u)) {
    etch_free_decoded_image(&job->decoded);
    job->ok = false;
   }
  }

  SDL_LockMutex(loader->mutex);
  if (loader->stop) {
   SDL_UnlockMutex(loader->mutex);
   free_job(job);
   return 0;
  }
  if (loader->done_tail) loader->done_tail->next = job;
  else loader->done_head = job;
  loader->done_tail = job;
  SDL_UnlockMutex(loader->mutex);
  etch_wake_up(loader->ctx);
 }
}

bool enqueue_request(InlayImageLoader* loader, EtchStr key, const char* path,
                     const void* mem_data, size_t mem_size, bool is_file, bool srgb) noexcept {
 if (!loader || !key.chars || key.length == 0u) return false;
 if (is_file && (!path || !path[0])) return false;
 if (!is_file && (!mem_data || mem_size == 0u)) return false;

 const uint64_t kh = inlay_fnv1a_64(key.chars, key.length);
 EtchLoadedTexture evicted = {};

 SDL_LockMutex(loader->mutex);
 uint32_t idx = find_entry_locked(loader, kh, key);
 if (idx != UINT32_MAX) {
  auto& e = loader->entries[idx];
  if (e.status == INLAY_IMAGE_STATUS_READY || e.status == INLAY_IMAGE_STATUS_LOADING) {
   e.lru_clock = ++loader->lru_clock;
   SDL_UnlockMutex(loader->mutex);
   return true;
  }
 }

 auto* job = static_cast<ImageJob*>(SDL_calloc(1u, sizeof(ImageJob)));
 if (!job) {
  SDL_UnlockMutex(loader->mutex);
  return false;
 }
 job->is_file = is_file;
 job->srgb = srgb;
 if (is_file) {
  const size_t plen = std::strlen(path);
  job->path = static_cast<char*>(SDL_malloc(plen + 1u));
  if (!job->path) {
   SDL_UnlockMutex(loader->mutex);
   free_job(job);
   return false;
  }
  std::memcpy(job->path, path, plen + 1u);
 } else {
  job->raw_bytes = static_cast<uint8_t*>(SDL_malloc(mem_size));
  if (!job->raw_bytes) {
   SDL_UnlockMutex(loader->mutex);
   free_job(job);
   return false;
  }
  std::memcpy(job->raw_bytes, mem_data, mem_size);
  job->raw_size = mem_size;
 }

 if (idx == UINT32_MAX) {
  idx = alloc_slot_locked(loader, &evicted);
  if (idx == UINT32_MAX) {
   SDL_UnlockMutex(loader->mutex);
   free_job(job);
   return false;
  }
  auto& e = loader->entries[idx];
  e.key_str = static_cast<char*>(SDL_malloc(key.length));
  if (!e.key_str) {
   SDL_UnlockMutex(loader->mutex);
   free_job(job);
   if (evicted.texture) etch_destroy_loaded_texture(loader->ctx, &evicted);
   return false;
  }
  std::memcpy(e.key_str, key.chars, key.length);
  e.key_len = key.length;
  e.key_hash = kh;
  e.occupied = true;
 }

 auto& entry = loader->entries[idx];
 entry.status = INLAY_IMAGE_STATUS_LOADING;
 entry.generation = ++loader->next_gen;
 entry.lru_clock = ++loader->lru_clock;
 entry.srgb = srgb;

 job->key_hash = kh;
 job->generation = entry.generation;
 job->slot_index = idx;
 if (loader->pending_tail) loader->pending_tail->next = job;
 else loader->pending_head = job;
 loader->pending_tail = job;
 SDL_SignalCondition(loader->work_cond);
 SDL_UnlockMutex(loader->mutex);

 if (evicted.texture) etch_destroy_loaded_texture(loader->ctx, &evicted);
 return true;
}
} // namespace

extern "C" {
INLAY_API InlayImageLoader* inlay_image_loader_create(EtchContext* ctx, const InlayImageLoaderDesc* desc_in) {
 if (!ctx) return nullptr;
 InlayImageLoaderDesc desc = inlay_image_loader_desc_default();
 if (desc_in && !inlay_read_abi_struct(desc_in, &desc)) return nullptr;
 const uint32_t max_entries = desc.max_entries == 0u ? 128u : (desc.max_entries > 16384u ? 16384u : desc.max_entries);
 const size_t max_vram = desc.max_vram_bytes == 0u ? (64u * 1024u * 1024u) : desc.max_vram_bytes;

 auto* loader = static_cast<InlayImageLoader*>(SDL_calloc(1u, sizeof(InlayImageLoader)));
 if (!loader) return nullptr;
 loader->ctx = ctx;
 loader->max_entries = max_entries;
 loader->max_vram_bytes = max_vram;
 loader->entries = static_cast<ImageEntry*>(SDL_calloc(max_entries, sizeof(ImageEntry)));
 loader->mutex = SDL_CreateMutex();
 loader->work_cond = SDL_CreateCondition();
 if (!loader->entries || !loader->mutex || !loader->work_cond) {
  if (loader->work_cond) SDL_DestroyCondition(loader->work_cond);
  if (loader->mutex) SDL_DestroyMutex(loader->mutex);
  SDL_free(loader->entries);
  SDL_free(loader);
  return nullptr;
 }
 loader->worker = SDL_CreateThread(image_loader_worker, "InlayImageLoader", loader);
 if (!loader->worker) {
  SDL_DestroyCondition(loader->work_cond);
  SDL_DestroyMutex(loader->mutex);
  SDL_free(loader->entries);
  SDL_free(loader);
  return nullptr;
 }
 return loader;
}

INLAY_API void inlay_image_loader_destroy(InlayImageLoader* loader) {
 if (!loader) return;
 SDL_LockMutex(loader->mutex);
 loader->stop = true;
 SDL_BroadcastCondition(loader->work_cond);
 SDL_UnlockMutex(loader->mutex);

 if (loader->worker) {
  SDL_WaitThread(loader->worker, nullptr);
  loader->worker = nullptr;
 }
 while (loader->pending_head) {
  ImageJob* nx = loader->pending_head->next;
  free_job(loader->pending_head);
  loader->pending_head = nx;
 }
 while (loader->done_head) {
  ImageJob* nx = loader->done_head->next;
  free_job(loader->done_head);
  loader->done_head = nx;
 }
 for (uint32_t i = 0u; i < loader->max_entries; ++i) {
  auto& e = loader->entries[i];
  if (e.tex.texture) etch_destroy_loaded_texture(loader->ctx, &e.tex);
  SDL_free(e.key_str);
 }
 SDL_DestroyCondition(loader->work_cond);
 SDL_DestroyMutex(loader->mutex);
 SDL_free(loader->entries);
 SDL_free(loader);
}

INLAY_API bool inlay_image_loader_request_file(InlayImageLoader* loader, EtchStr key, const char* path, bool srgb) {
 return enqueue_request(loader, key, path, nullptr, 0u, true, srgb);
}

INLAY_API bool inlay_image_loader_request_memory(InlayImageLoader* loader, EtchStr key, const void* data, size_t size_bytes, bool srgb) {
 return enqueue_request(loader, key, nullptr, data, size_bytes, false, srgb);
}

INLAY_API void inlay_image_loader_cancel(InlayImageLoader* loader, EtchStr key) {
 if (!loader || !key.chars || key.length == 0u) return;
 const uint64_t kh = inlay_fnv1a_64(key.chars, key.length);
 SDL_LockMutex(loader->mutex);
 const uint32_t idx = find_entry_locked(loader, kh, key);
 if (idx != UINT32_MAX && loader->entries[idx].status == INLAY_IMAGE_STATUS_LOADING) {
  loader->entries[idx].generation = ++loader->next_gen;
  loader->entries[idx].status = INLAY_IMAGE_STATUS_IDLE;
 }
 SDL_UnlockMutex(loader->mutex);
}

INLAY_API uint32_t inlay_image_loader_poll(InlayImageLoader* loader, uint32_t max_uploads) {
 if (!loader) return 0u;
 const uint32_t budget = max_uploads ? max_uploads : 4u;
 uint32_t uploaded = 0u;

 while (uploaded < budget) {
  SDL_LockMutex(loader->mutex);
  ImageJob* job = loader->done_head;
  if (!job) {
   SDL_UnlockMutex(loader->mutex);
   break;
  }
  loader->done_head = job->next;
  if (!loader->done_head) loader->done_tail = nullptr;
  job->next = nullptr;

  bool active = false;
  if (job->slot_index < loader->max_entries) {
   const auto& e = loader->entries[job->slot_index];
   active = e.occupied && e.key_hash == job->key_hash &&
            e.generation == job->generation && e.status == INLAY_IMAGE_STATUS_LOADING;
  }
  SDL_UnlockMutex(loader->mutex);

  if (!active) {
   free_job(job);
   continue;
  }

  EtchLoadedTexture tex = {};
  if (job->ok && job->decoded.rgba && job->decoded.width > 0u && job->decoded.height > 0u) {
   EtchTextureDesc tdesc = etch_texture_desc_default();
   tdesc.rgba_pixels = job->decoded.rgba;
   tdesc.width = job->decoded.width;
   tdesc.height = job->decoded.height;
   tdesc.row_pitch_bytes = job->decoded.width * 4u;
   tdesc.srgb = job->srgb;
   tdesc.generate_mipmaps = job->generate_mipmaps;
   tex = etch_create_texture_rgba(loader->ctx, &tdesc);
  }
  etch_free_decoded_image(&job->decoded);
  const uint32_t slot_index = job->slot_index;
  const uint64_t job_hash = job->key_hash;
  const uint64_t job_gen = job->generation;
  free_job(job);

  bool discard_new = false;
  bool need_vram_evict = false;

  SDL_LockMutex(loader->mutex);
  if (slot_index < loader->max_entries) {
   auto& e = loader->entries[slot_index];
   if (e.occupied && e.key_hash == job_hash && e.generation == job_gen && e.status == INLAY_IMAGE_STATUS_LOADING) {
    if (tex.texture != nullptr) {
     const size_t bytes = (tex.width > 0u && tex.height > 0u)
      ? (size_t(tex.width) * size_t(tex.height) * 4u) : 4u;
     e.tex = tex;
     e.vram_bytes = bytes;
     e.status = INLAY_IMAGE_STATUS_READY;
     e.lru_clock = ++loader->lru_clock;
     loader->current_vram_bytes += bytes;
     ++loader->epoch;
     ++uploaded;
     need_vram_evict = loader->current_vram_bytes > loader->max_vram_bytes;
    } else {
     e.status = INLAY_IMAGE_STATUS_ERROR;
     ++loader->epoch;
    }
   } else {
    discard_new = true;
   }
  } else {
   discard_new = true;
  }
  SDL_UnlockMutex(loader->mutex);

  if (discard_new && tex.texture) etch_destroy_loaded_texture(loader->ctx, &tex);

  while (need_vram_evict) {
   EtchLoadedTexture evict_stack[32] = {};
   uint32_t evict_count = 0u;
   SDL_LockMutex(loader->mutex);
   while (loader->current_vram_bytes > loader->max_vram_bytes && evict_count < 32u) {
    uint32_t victim = UINT32_MAX;
    uint64_t oldest = UINT64_MAX;
    for (uint32_t i = 0u; i < loader->max_entries; ++i) {
     if (i == slot_index) continue;
     const auto& cand = loader->entries[i];
     if (cand.occupied && cand.status == INLAY_IMAGE_STATUS_READY && cand.tex.texture && cand.lru_clock < oldest) {
      oldest = cand.lru_clock;
      victim = i;
     }
    }
    if (victim == UINT32_MAX) break;
    auto& ve = loader->entries[victim];
    evict_stack[evict_count++] = ve.tex;
    if (loader->current_vram_bytes >= ve.vram_bytes) loader->current_vram_bytes -= ve.vram_bytes;
    else loader->current_vram_bytes = 0u;
    SDL_free(ve.key_str);
    ve = {};
   }
   need_vram_evict = (evict_count == 32u && loader->current_vram_bytes > loader->max_vram_bytes);
   SDL_UnlockMutex(loader->mutex);
   for (uint32_t i = 0u; i < evict_count; ++i) {
    etch_destroy_loaded_texture(loader->ctx, &evict_stack[i]);
   }
  }
 }

 return uploaded;
}

INLAY_API InlayImageLoadStatus inlay_image_loader_status(const InlayImageLoader* loader, EtchStr key) {
 if (!loader || !key.chars || key.length == 0u) return INLAY_IMAGE_STATUS_IDLE;
 const uint64_t kh = inlay_fnv1a_64(key.chars, key.length);
 SDL_LockMutex(loader->mutex);
 const uint32_t idx = find_entry_locked(loader, kh, key);
 const InlayImageLoadStatus st = (idx != UINT32_MAX) ? loader->entries[idx].status : INLAY_IMAGE_STATUS_IDLE;
 SDL_UnlockMutex(loader->mutex);
 return st;
}

INLAY_API uint32_t inlay_image_loader_epoch(const InlayImageLoader* loader) {
 if (!loader) return 0u;
 SDL_LockMutex(loader->mutex);
 const uint32_t ep = loader->epoch;
 SDL_UnlockMutex(loader->mutex);
 return ep;
}

INLAY_API bool inlay_image_loader_resolve(EtchStr src, InlayImageResult* out, void* loader_user_data) {
 if (!src.chars || src.length == 0u || !out || !loader_user_data) return false;
 auto* loader = static_cast<InlayImageLoader*>(loader_user_data);
 const uint64_t kh = inlay_fnv1a_64(src.chars, src.length);
 SDL_LockMutex(loader->mutex);
 const uint32_t idx = find_entry_locked(loader, kh, src);
 if (idx != UINT32_MAX) {
  auto& e = loader->entries[idx];
  if (e.status == INLAY_IMAGE_STATUS_READY && e.tex.texture) {
   out->texture = e.tex.texture;
   out->width_px = static_cast<float>(e.tex.width);
   out->height_px = static_cast<float>(e.tex.height);
   out->radius_px = 0.0f;
   e.lru_clock = ++loader->lru_clock;
   SDL_UnlockMutex(loader->mutex);
   return true;
  }
 }
 SDL_UnlockMutex(loader->mutex);
 return false;
}
} // extern "C"
