#include <inlay/inlay.h>

namespace {
bool decorate(inlay::InlineKind kind,EtchStr,EtchStr url,EtchTextSpan* span,void* user) {
 *static_cast<bool*>(user)=kind==INLAY_INLINE_LINK && url.length==5;
 span->padding_x=4;
 return true;
}
}

int main() {
 const inlay::PortalDesc portal=inlay_portal_desc_default();
 const inlay::SurfaceDesc surface=inlay_surface_desc_default();
 if(!portal.width || !inlay_portal_mock_backend() || !inlay_portal_destroy(nullptr) ||
    !surface.max_nodes || !inlay_doc_config_default().initial_blocks ||
    inlay_math_desc_default().font_size_px<=0 || inlay_diagram_desc_default().rank_gap_px<=0)return 1;
 auto config=etch_context_desc_default();config.headless=true;
 auto* ctx=etch_create_context(&config);
 inlay::Doc* doc=inlay_doc_create(ctx,nullptr);
 bool called=false;
 const inlay::InlineDecoratorFn decorator=decorate;
 auto view=inlay_doc_view_desc_default();
 view.id=etch_id("cpp-flow");view.height_px=0;view.width_px=240;
 view.inline_decorator_fn=decorator;view.inline_decorator_user_data=&called;
 bool ok=doc && inlay_doc_set_utf8(doc,ETCH_STR_LIT("*Italic* and [link](local)"));
 if(ok) {
  auto frame=etch_frame_desc_default();frame.viewport_w=320;frame.viewport_h=240;
  etch_begin_frame(ctx,&frame);
  const inlay::DocResult result=inlay_doc_view(doc,&view);
  etch_end_frame(ctx);
  ok=called && result.visible_end==1 && result.measured_height_px>0 &&
     inlay_doc_measure_height(doc,view.id,view.width_px,view.font_size_px)>0;
 }
 inlay_doc_destroy(doc);
 etch_destroy_context(ctx);
 return ok ? 0 : 1;
}
