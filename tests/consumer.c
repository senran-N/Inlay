#include <inlay/inlay_c.h>
#include <string.h>
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
static void render_diagram_header(uint32_t block_index,uint32_t row_id,EtchStr lang,EtchStr code,void* user) {
 (void)block_index;(void)row_id;(void)code;
 if(lang.length==7 && memcmp(lang.chars,"mermaid",7)==0)*(bool*)user=true;
}
static void render_c99_slot(InlaySurface* surface,uint32_t element_id,float width_px,float height_px,EtchStr props_json,void* user) {
 (void)surface;(void)element_id;
 if(width_px>0.0f && height_px>0.0f && props_json.chars && props_json.length>0u)*(bool*)user=true;
}
int main(void) {
 if(inlay_abi_version()!=INLAY_ABI_VERSION)return 1;
#if INLAY_VERSION_MAJOR != 0 || INLAY_VERSION_MINOR != 13
#error "Unexpected Inlay version"
#endif
 /* Verify all v0.13.0 InlayUiKind enum values compile & remain distinct even with INLAY_ENABLE_SCRIPT=OFF */
 {
  const InlayUiKind v013_kinds[12]={
   INLAY_UI_DOC, INLAY_UI_TERMINAL, INLAY_UI_VIRTUAL_LIST, INLAY_UI_TABLE,
   INLAY_UI_TREE, INLAY_UI_TREE_ITEM, INLAY_UI_MENU, INLAY_UI_CONTEXT_MENU,
   INLAY_UI_AVATAR, INLAY_UI_TOAST, INLAY_UI_ACCORDION, INLAY_UI_SKELETON
  };
  for(size_t i=0;i<12u;++i) {
   if(v013_kinds[i]<=INLAY_UI_SLOT)return 1;
   for(size_t j=i+1u;j<12u;++j)if(v013_kinds[i]==v013_kinds[j])return 1;
  }
 }
 EtchContextDesc c=etch_context_desc_default();c.headless=true;
 EtchContext* ctx=etch_create_context(&c);if(!ctx)return 1;
 /* Versioned C99 ABI struct_size checks: default, zero-init, partial prefix, corrupt tiny, oversized */
 const InlayDocConfig def_cfg=inlay_doc_config_default();
 if(def_cfg.struct_size!=(uint32_t)sizeof(InlayDocConfig))return 1;
 InlayDocConfig zero_cfg={0};
 InlayDoc* zero_doc=inlay_doc_create(ctx,&zero_cfg);
 if(!zero_doc)return 1;
 inlay_doc_destroy(zero_doc);
 InlayDocConfig partial_cfg={0};
 partial_cfg.struct_size=(uint32_t)offsetof(InlayDocConfig,initial_blocks);
 partial_cfg.initial_source_bytes=1024u;
 InlayDoc* partial_doc=inlay_doc_create(ctx,&partial_cfg);
 if(!partial_doc)return 1;
 inlay_doc_destroy(partial_doc);
 InlayDocConfig bad_tiny_cfg=def_cfg;
 bad_tiny_cfg.struct_size=2u;
 if(inlay_doc_create(ctx,&bad_tiny_cfg)!=NULL)return 1;
 InlayDocConfig bad_big_cfg=def_cfg;
 bad_big_cfg.struct_size=(uint32_t)sizeof(InlayDocConfig)+8u;
 if(inlay_doc_create(ctx,&bad_big_cfg)!=NULL)return 1;

 InlayDoc* doc=inlay_doc_create(ctx,NULL);if(!doc)return 1;
 const InlaySurfaceDesc surface=inlay_surface_desc_default();
 if(surface.struct_size!=(uint32_t)sizeof(InlaySurfaceDesc) ||
    surface.disable_jsx ||
    surface.max_tick_ms<=0 || inlay_surface_stats(NULL).js_ticks || inlay_surface_next_timer_ms(NULL)!=-1 ||
    inlay_surface_set_script(NULL,ETCH_STR_LIT("")) || inlay_surface_bind_slot(NULL,ETCH_STR_LIT("slot"),render_c99_slot,NULL))return 1;
 InlaySurfaceDesc bad_surface=surface;bad_surface.struct_size=1u;
 if(inlay_surface_create(ctx,&bad_surface)!=NULL)return 1;
 /* Legacy v0.12 struct_size prefix without disable_jsx remains ABI-compatible */
 {
  InlaySurfaceDesc legacy_surface=surface;
  legacy_surface.struct_size=(uint32_t)offsetof(InlaySurfaceDesc,disable_jsx);
  InlaySurface* legacy_s=inlay_surface_create(ctx,&legacy_surface);
  if(legacy_s)inlay_surface_destroy(legacy_s);
 }
 InlaySurface* scripted=inlay_surface_create(ctx,NULL);
 if(scripted){
  if(!inlay_surface_set_state_json(scripted,ETCH_STR_LIT("{\"value\":3}")) || !inlay_surface_set_script(scripted,ETCH_STR_LIT("function render(s){return ui.scroll({},ui.input({value:String(s.value)}),ui.toggle({label:'Live',checked:true}),ui.progress({value:0.5}),ui.slider({value:0.2,min:0,max:1}),ui.badge('v" INLAY_VERSION_STRING "'),ui.divider({}),ui.image({alt:'pic'}));}")))return 1;
  uint32_t count=0;
  const InlayUiNode* nodes=inlay_surface_nodes(scripted,&count);
  if(!nodes || count!=8 || nodes[0].kind!=INLAY_UI_SCROLL || nodes[1].kind!=INLAY_UI_INPUT ||
     nodes[2].kind!=INLAY_UI_TOGGLE || nodes[3].kind!=INLAY_UI_PROGRESS ||
     nodes[4].kind!=INLAY_UI_SLIDER || nodes[5].kind!=INLAY_UI_BADGE ||
     nodes[6].kind!=INLAY_UI_DIVIDER || nodes[7].kind!=INLAY_UI_IMAGE)return 1;
  if(inlay_surface_bind_slot(scripted,ETCH_STR_LIT(""),render_c99_slot,NULL) ||
     inlay_surface_bind_slot(scripted,ETCH_STR_LIT("c99"),NULL,NULL))return 1;
  bool slot_called=false;
  if(!inlay_surface_bind_slot(scripted,ETCH_STR_LIT("c99_slot"),render_c99_slot,&slot_called))return 1;
  if(!inlay_surface_set_script(scripted,ETCH_STR_LIT("function render(){return <Card cls='p-2 gap-2'><Tabs items={['A','B']} active={0} /><Math tex='x^2' /><Avatar name='C99' size='sm' /><Table columns={['K']} rows={[['V']]} /><Tree><TreeItem label='N' leaf /></Tree><Accordion title='A'><Skeleton variant='rect' width={40} height={10} /></Accordion><Slot name='c99_slot' width={120} height={32} props={{ok:1}} /></Card>;}")))return 1;
  nodes=inlay_surface_nodes(scripted,&count);
  if(!nodes || !count || nodes[0].kind!=INLAY_UI_CARD)return 1;
  bool has_card=false,has_tabs=false,has_math=false,has_slot=false,has_avatar=false,has_table=false,has_tree=false,has_acc=false,has_skel=false;
  for(uint32_t i=0;i<count;++i){
   if(nodes[i].kind==INLAY_UI_CARD)has_card=true;
   else if(nodes[i].kind==INLAY_UI_TABS)has_tabs=true;
   else if(nodes[i].kind==INLAY_UI_MATH)has_math=true;
   else if(nodes[i].kind==INLAY_UI_SLOT)has_slot=true;
   else if(nodes[i].kind==INLAY_UI_AVATAR)has_avatar=true;
   else if(nodes[i].kind==INLAY_UI_TABLE)has_table=true;
   else if(nodes[i].kind==INLAY_UI_TREE)has_tree=true;
   else if(nodes[i].kind==INLAY_UI_ACCORDION)has_acc=true;
   else if(nodes[i].kind==INLAY_UI_SKELETON)has_skel=true;
  }
  if(!has_card || !has_tabs || !has_math || !has_slot || !has_avatar || !has_table || !has_tree || !has_acc || !has_skel)return 1;
  {
   EtchFrameDesc sf=etch_frame_desc_default();
   sf.viewport_w=320;sf.viewport_h=240;
   InlaySurfaceViewDesc sv=inlay_surface_view_desc_default();
   sv.id=etch_id("c99-slot-view");sv.width_px=0.0f;sv.height_px=0.0f;
   etch_begin_frame(ctx,&sf);
   inlay_surface_view(scripted,&sv);
   etch_end_frame(ctx);
  }
  if(!slot_called)return 1;
  /* Verify disable_jsx flag on InlaySurfaceDesc */
  {
   InlaySurfaceDesc no_jsx_desc=inlay_surface_desc_default();
   no_jsx_desc.disable_jsx=true;
   InlaySurface* no_jsx=inlay_surface_create(ctx,&no_jsx_desc);
   if(!no_jsx)return 1;
   if(inlay_surface_set_script(no_jsx,ETCH_STR_LIT("function render(){return <Text>x</Text>;}")))return 1;
   if(!inlay_surface_set_script(no_jsx,ETCH_STR_LIT("function render(){return ui.text('ok');}")))return 1;
   inlay_surface_destroy(no_jsx);
  }
  inlay_surface_destroy(scripted);
 }
 InlayPortalDesc portal_desc=inlay_portal_desc_default();
 if(portal_desc.struct_size!=(uint32_t)sizeof(InlayPortalDesc) ||
    inlay_portal_view_desc_default().struct_size!=(uint32_t)sizeof(InlayPortalViewDesc) ||
    inlay_portal_webview2_options_default().struct_size!=(uint32_t)sizeof(InlayPortalWebView2Options))return 1;
 portal_desc.mode=INLAY_PORTAL_MODE_UNDERLAY;
 InlayPortal* portal=inlay_portal_create(ctx,&portal_desc);
 inlay_portal_set_input_enabled(portal,false);
 if(inlay_portal_has_focus(portal) || !inlay_portal_mock_advance(portal,1))return 1;
 InlayPortalDesc bad_portal=portal_desc;bad_portal.struct_size=3u;
 if(inlay_portal_create(ctx,&bad_portal)!=NULL)return 1;
 const InlayPortalBackendVTable* web_backend=inlay_portal_webview2_backend();
 if(web_backend && web_backend->create(NULL,320,240))return 1;
 if(!portal || !inlay_portal_load_html(portal,ETCH_STR_LIT("C99 portal")) || !inlay_portal_poll(portal) || !inlay_portal_destroy(portal))return 1;
 const InlayMathDesc math=inlay_math_desc_default();
 const InlayDiagramDesc diagram=inlay_diagram_desc_default();
 if(math.struct_size!=(uint32_t)sizeof(InlayMathDesc) || diagram.struct_size!=(uint32_t)sizeof(InlayDiagramDesc) ||
    math.font_size_px<=0 || diagram.node_gap_px<=0)return 1;
 EtchFrameDesc frame=etch_frame_desc_default();
 frame.viewport_w=320;frame.viewport_h=240;
 InlayDocStyle style=inlay_doc_style_default();
 if(style.struct_size!=(uint32_t)sizeof(InlayDocStyle) ||
    style.font_id!=ETCH_INVALID_FONT || style.bold_font_id!=ETCH_INVALID_FONT ||
    style.mono_font_id!=ETCH_INVALID_FONT || style.has_background ||
    style.inline_code_no_border || style.inline_code_padding_x!=0.0f ||
    style.inline_code_padding_y!=0.0f || style.inline_code_radius_px!=0.0f ||
    style.alert_bg_alpha!=0.0f)return 1;
 style.has_background=true;
 style.highlight_bg=etch_rgba8(255,220,80,120);
 style.inline_code_no_border=true;
 style.inline_code_radius_px=4.0f;
 style.alert_colors[0]=etch_rgba8(64,156,255,255);
 InlayDocViewDesc view=inlay_doc_view_desc_default();
 if(view.struct_size!=(uint32_t)sizeof(InlayDocViewDesc) ||
    view.block_padding_px!=12 || view.block_gap_px!=8 || view.table_cell_padding_x!=12 ||
    view.table_cell_padding_y!=8 || view.table_radius_px!=8 || view.inline_decorator_fn ||
    view.inline_decorator_user_data || view.hide_code_copy_button ||
    view.style || view.image_resolver_fn || view.task_toggle_fn ||
    view.stick_to_bottom || view.custom_fence_estimate_fn || view.custom_fence_render_fn ||
    view.code_header_fn || view.diagram_header_fn || view.diagram_header_user_data)return 1;
 bool called=false,img_called=false,custom_called=false,hdr_called=false,diag_hdr_called=false;
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
 view.diagram_header_fn=render_diagram_header;
 view.diagram_header_user_data=&diag_hdr_called;
 if(!inlay_doc_set_utf8(doc,ETCH_STR_LIT("# C99 consumer\n\n`Native` ==API== [^1].\n\n![pic](a.png)\n\n[^1]: Note.\n\n```custom\nhello\n```\n\n```rust\nlet x = 1;\n```\n\n```mermaid\nflowchart LR\nA[In] --> B[Out]\n```\n")))return 1;
 const float estimate=inlay_doc_measure_height(doc,view.id,view.width_px,view.font_size_px);
 inlay_doc_view_scroll_to_bottom(view.id,true);
 etch_begin_frame(ctx,&frame);
 /* Invalid struct_size view descriptor must safely no-op */
 InlayDocViewDesc bad_view=view;bad_view.struct_size=2u;
 const InlayDocResult bad_res=inlay_doc_view(doc,&bad_view);
 /* Partial legacy struct_size ABI backward compatibility for Style, ViewDesc, and DiagramDesc */
 InlayDocStyle legacy_style=inlay_doc_style_default();
 legacy_style.struct_size=(uint32_t)offsetof(InlayDocStyle,inline_code_bg);
 InlayDocViewDesc legacy_view=inlay_doc_view_desc_default();
 legacy_view.struct_size=(uint32_t)offsetof(InlayDocViewDesc,diagram_header_fn);
 legacy_view.id=etch_id("consumer-legacy");legacy_view.width_px=260;legacy_view.height_px=0;
 legacy_view.style=&legacy_style;
 const InlayDocResult legacy_res=inlay_doc_view(doc,&legacy_view);
 InlayDiagramDesc legacy_diag=inlay_diagram_desc_default();
 legacy_diag.struct_size=(uint32_t)offsetof(InlayDiagramDesc,max_width_px);
 const InlayDiagramResult legacy_graph=inlay_diagram_layout(ETCH_STR_LIT("flowchart LR\nA --> B"),&legacy_diag);
 InlayDiagramDesc fit_diag=inlay_diagram_desc_default();
 fit_diag.max_width_px=220.0f;
 fit_diag.min_scale=0.5f;
 const InlayDiagramResult fit_graph=inlay_diagram_layout(ETCH_STR_LIT("flowchart LR\nA[One] --> B[Two] --> C[Three] --> D[Four]"),&fit_diag);
 const InlayDocResult result=inlay_doc_view(doc,&view);
 const InlayDiagramResult graph=inlay_diagram_layout(ETCH_STR_LIT("flowchart TD\nA -. retry .-> B"),&diagram);
 const bool dashed=graph.valid && graph.edge_count==1 && graph.edges[0].dashed && graph.edges[0].label.length==5 &&
                   graph.scale==1.0f && graph.natural_width==graph.width;
 etch_end_frame(ctx);
 if(bad_res.total_rows!=0 || !dashed || legacy_res.total_rows!=7 ||
    !legacy_graph.valid || legacy_graph.scale!=1.0f ||
    !fit_graph.valid || fit_graph.scale>=1.0f || fit_graph.width>220.5f ||
    fit_graph.natural_width<=220.0f)return 1;
 /* Selection + plain text / markdown copy API smoke */
 inlay_doc_view_select_all(doc,view.id);
 const InlayDocSelection sel=inlay_doc_view_selection(doc,view.id);
 char plain_buf[256];
 const uint32_t plain_len=inlay_doc_view_copy_plain_text(doc,view.id,plain_buf,(uint32_t)sizeof(plain_buf));
 char md_buf[256];
 const uint32_t md_len=inlay_doc_view_copy_markdown(doc,view.id,md_buf,(uint32_t)sizeof(md_buf));
 inlay_doc_view_clear_selection(doc,view.id);
 const InlayBlock* img_blk=inlay_doc_block(doc,2);
 const InlayBlock* fn_blk=inlay_doc_block(doc,3);
 const bool ok=estimate>0 && called && img_called && custom_called && hdr_called && diag_hdr_called &&
               result.clicked_kind==INLAY_INLINE_TEXT && result.clicked_mods==0 &&
               sel.active && sel.focus==sel.total_bytes && plain_len>0 && md_len>0 &&
               strstr(plain_buf,"C99 consumer")!=NULL && strstr(md_buf,"# C99 consumer")!=NULL &&
               !inlay_doc_view_selection(doc,view.id).active &&
               result.at_bottom && inlay_doc_view_is_at_bottom(view.id) &&
               result.measured_height_px>0 &&
               result.visible_end==7 && inlay_doc_block_count(doc)==7 &&
               img_blk && img_blk->kind==INLAY_IMAGE &&
               fn_blk && fn_blk->kind==INLAY_FOOTNOTE;
 inlay_doc_destroy(doc);etch_destroy_context(ctx);return ok ? 0 : 1;
}
