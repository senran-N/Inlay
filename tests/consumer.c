#include <inlay/inlay_c.h>
int main(void) {
 EtchContextDesc c=etch_context_desc_default();c.headless=true;
 EtchContext* ctx=etch_create_context(&c);if(!ctx)return 1;
 InlayDoc* doc=inlay_doc_create(ctx,NULL);if(!doc)return 1;
 const InlaySurfaceDesc surface=inlay_surface_desc_default();
 if(surface.max_tick_ms<=0 || inlay_surface_stats(NULL).js_ticks || inlay_surface_set_script(NULL,ETCH_STR_LIT("")))return 1;
 InlaySurface* scripted=inlay_surface_create(ctx,NULL);
 if(scripted){
  if(!inlay_surface_set_state_json(scripted,ETCH_STR_LIT("{\"value\":3}")) || !inlay_surface_set_script(scripted,ETCH_STR_LIT("function render(s){return h('box',{},h('input',{value:String(s.value)}),h('toggle',{label:'Live',checked:true}))}")))return 1;
  uint32_t count=0;
  const InlayUiNode* nodes=inlay_surface_nodes(scripted,&count);
  if(!nodes || count!=3 || nodes[1].kind!=INLAY_UI_INPUT || nodes[2].kind!=INLAY_UI_TOGGLE)return 1;
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
 etch_begin_frame(ctx,&frame);
 const InlayDiagramResult graph=inlay_diagram_layout(ETCH_STR_LIT("flowchart TD\nA -. retry .-> B"),&diagram);
 const bool dashed=graph.valid && graph.edge_count==1 && graph.edges[0].dashed && graph.edges[0].label.length==5;
 etch_end_frame(ctx);
 if(!dashed)return 1;
 const bool ok=inlay_doc_set_utf8(doc,ETCH_STR_LIT("# C99 consumer\n\nNative API.")) && inlay_doc_block_count(doc)==2;
 inlay_doc_destroy(doc);etch_destroy_context(ctx);return ok ? 0 : 1;
}
