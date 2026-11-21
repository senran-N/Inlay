#include <inlay/inlay_c.h>
static bool decorate(InlayInlineKind kind,EtchStr text,EtchStr url,EtchTextSpan* span,void* user) {
 bool* called=(bool*)user;
 *called=kind==INLAY_INLINE_CODE && text.length==6 && url.length==0;
 span->padding_y=2;
 return *called;
}
static bool resolve_img(EtchStr alt,EtchStr src,InlayImageResult* out,void* user) {
 (void)alt;(void)src;(void)out;
 *(bool*)user=true;
 return false;
}
static void on_task_toggle(uint32_t block_index,uint32_t item_index,bool checked,void* user) {
 (void)block_index;(void)item_index;(void)checked;(void)user;
}
static float est_custom_fence(EtchStr lang,EtchStr body,float avail_w,float font_px,void* user) {
 (void)body;(void)avail_w;(void)font_px;(void)user;
 return (lang.length==6 && lang.chars[0]=='c' && lang.chars[1]=='u' && lang.chars[2]=='s') ? 40.0f : 0.0f;
}
static void render_custom_fence(uint32_t block_index,uint32_t row_id,EtchStr lang,EtchStr body,float avail_w,float font_px,void* user) {
 (void)block_index;(void)row_id;(void)lang;(void)body;(void)avail_w;(void)font_px;
 *(bool*)user=true;
}
static void render_code_header(uint32_t block_index,uint32_t row_id,EtchStr lang,EtchStr code,void* user) {
 (void)block_index;(void)row_id;(void)lang;(void)code;
 *(bool*)user=true;
}
int main(void) {
 EtchContextDesc c=etch_context_desc_default();c.headless=true;
 EtchContext* ctx=etch_create_context(&c);if(!ctx)return 1;
 InlayDoc* doc=inlay_doc_create(ctx,NULL);if(!doc)return 1;
 const InlaySurfaceDesc surface=inlay_surface_desc_default();
 if(surface.max_tick_ms<=0 || inlay_surface_stats(NULL).js_ticks || inlay_surface_next_timer_ms(NULL)!=-1 || inlay_surface_set_script(NULL,ETCH_STR_LIT("")))return 1;
 InlaySurface* scripted=inlay_surface_create(ctx,NULL);
 if(scripted){
  if(!inlay_surface_set_state_json(scripted,ETCH_STR_LIT("{\"value\":3}")) || !inlay_surface_set_script(scripted,ETCH_STR_LIT("function render(s){return ui.scroll({},ui.input({value:String(s.value)}),ui.toggle({label:'Live',checked:true}),ui.progress({value:0.5}),ui.slider({value:0.2,min:0,max:1}),ui.badge('v0.9'),ui.divider({}),ui.image({alt:'pic'}));}")))return 1;
  uint32_t count=0;
  const InlayUiNode* nodes=inlay_surface_nodes(scripted,&count);
  if(!nodes || count!=8 || nodes[0].kind!=INLAY_UI_SCROLL || nodes[1].kind!=INLAY_UI_INPUT ||
     nodes[2].kind!=INLAY_UI_TOGGLE || nodes[3].kind!=INLAY_UI_PROGRESS ||
     nodes[4].kind!=INLAY_UI_SLIDER || nodes[5].kind!=INLAY_UI_BADGE ||
     nodes[6].kind!=INLAY_UI_DIVIDER || nodes[7].kind!=INLAY_UI_IMAGE)return 1;
  inlay_surface_destroy(scripted);
 }
 InlayPortalDesc portal_desc=inlay_portal_desc_default();portal_desc.mode=INLAY_PORTAL_MODE_UNDERLAY;
 InlayPortal* portal=inlay_portal_create(ctx,&portal_desc);
 inlay_portal_set_input_enabled(portal,false);
 if(inlay_portal_has_focus(portal) || !inlay_portal_mock_advance(portal,1))return 1;
 const InlayPortalBackendVTable* web_backend=inlay_portal_webview2_backend();
 if(web_backend && web_backend->create(NULL,320,240))return 1;
 if(!portal || !inlay_portal_load_html(portal,ETCH_STR_LIT("C99 portal")) || !inlay_portal_poll(portal) || !inlay_portal_destroy(portal))return 1;
 const InlayMathDesc math=inlay_math_desc_default();
 const InlayDiagramDesc diagram=inlay_diagram_desc_default();
 if(math.font_size_px<=0 || diagram.node_gap_px<=0)return 1;
 EtchFrameDesc frame=etch_frame_desc_default();
 frame.viewport_w=320;frame.viewport_h=240;
 InlayDocStyle style=inlay_doc_style_default();
 if(style.font_id!=ETCH_INVALID_FONT || style.bold_font_id!=ETCH_INVALID_FONT ||
    style.mono_font_id!=ETCH_INVALID_FONT || style.has_background)return 1;
 style.has_background=true;
 style.highlight_bg=etch_rgba8(255,220,80,120);
 InlayDocViewDesc view=inlay_doc_view_desc_default();
 if(view.block_padding_px!=12 || view.block_gap_px!=8 || view.table_cell_padding_x!=12 ||
    view.table_cell_padding_y!=8 || view.table_radius_px!=8 || view.inline_decorator_fn ||
    view.inline_decorator_user_data || view.hide_code_copy_button ||
    view.style || view.image_resolver_fn || view.task_toggle_fn ||
    view.stick_to_bottom || view.custom_fence_estimate_fn || view.custom_fence_render_fn || view.code_header_fn)return 1;
 bool called=false,img_called=false,custom_called=false,hdr_called=false;
 view.id=etch_id("consumer-flow");view.height_px=0;view.width_px=300;
 view.inline_decorator_fn=decorate;view.inline_decorator_user_data=&called;
 view.style=&style;
 view.image_resolver_fn=resolve_img;view.image_resolver_user_data=&img_called;
 view.task_toggle_fn=on_task_toggle;
 view.stick_to_bottom=true;
 view.custom_fence_estimate_fn=est_custom_fence;
 view.custom_fence_render_fn=render_custom_fence;
 view.custom_fence_user_data=&custom_called;
 view.code_header_fn=render_code_header;
 view.code_header_user_data=&hdr_called;
 if(!inlay_doc_set_utf8(doc,ETCH_STR_LIT("# C99 consumer\n\n`Native` ==API== [^1].\n\n![pic](a.png)\n\n[^1]: Note.\n\n```custom\nhello\n```\n\n```rust\nlet x = 1;\n```\n")))return 1;
 const float estimate=inlay_doc_measure_height(doc,view.id,view.width_px,view.font_size_px);
 inlay_doc_view_scroll_to_bottom(view.id,true);
 etch_begin_frame(ctx,&frame);
 const InlayDocResult result=inlay_doc_view(doc,&view);
 const InlayDiagramResult graph=inlay_diagram_layout(ETCH_STR_LIT("flowchart TD\nA -. retry .-> B"),&diagram);
 const bool dashed=graph.valid && graph.edge_count==1 && graph.edges[0].dashed && graph.edges[0].label.length==5;
 etch_end_frame(ctx);
 if(!dashed)return 1;
 const InlayBlock* img_blk=inlay_doc_block(doc,2);
 const InlayBlock* fn_blk=inlay_doc_block(doc,3);
 const bool ok=estimate>0 && called && img_called && custom_called && hdr_called &&
               result.at_bottom && inlay_doc_view_is_at_bottom(view.id) &&
               result.measured_height_px>0 &&
               result.visible_end==6 && inlay_doc_block_count(doc)==6 &&
               img_blk && img_blk->kind==INLAY_IMAGE &&
               fn_blk && fn_blk->kind==INLAY_FOOTNOTE;
 inlay_doc_destroy(doc);etch_destroy_context(ctx);return ok ? 0 : 1;
}
