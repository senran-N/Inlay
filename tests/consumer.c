#include <inlay/inlay_c.h>
int main(void) {
 EtchContextDesc c=etch_context_desc_default();c.headless=true;
 EtchContext* ctx=etch_create_context(&c);if(!ctx)return 1;
 InlayDoc* doc=inlay_doc_create(ctx,NULL);if(!doc)return 1;
 const bool ok=inlay_doc_set_utf8(doc,ETCH_STR_LIT("# C99 consumer\n\nNative API.")) && inlay_doc_block_count(doc)==2;
 inlay_doc_destroy(doc);etch_destroy_context(ctx);return ok ? 0 : 1;
}
