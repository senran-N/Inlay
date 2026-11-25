#include <inlay/inlay.h>
#include <cstring>

namespace {
bool decorate(inlay::InlineKind kind,EtchStr,EtchStr url,EtchTextSpan* span,void* user) {
 *static_cast<bool*>(user)=kind==INLAY_INLINE_LINK && url.length==5;
 span->padding_x=4;
 return true;
}
void render_cpp_slot(inlay::Surface*,uint32_t,float width_px,float height_px,EtchStr props_json,void* user) {
 if(width_px>0.0f && height_px>0.0f && props_json.chars && props_json.length>0u)
  *static_cast<bool*>(user)=true;
}
}

int main() {
 if(inlay::abi_version()!=INLAY_ABI_VERSION)return 1;
 static_assert(INLAY_VERSION_MAJOR==0 && INLAY_VERSION_MINOR==14);
 static_assert(INLAY_UI_DOC>INLAY_UI_SLOT && INLAY_UI_TERMINAL>INLAY_UI_DOC &&
               INLAY_UI_VIRTUAL_LIST>INLAY_UI_TERMINAL && INLAY_UI_TABLE>INLAY_UI_VIRTUAL_LIST &&
               INLAY_UI_TREE>INLAY_UI_TABLE && INLAY_UI_TREE_ITEM>INLAY_UI_TREE &&
               INLAY_UI_MENU>INLAY_UI_TREE_ITEM && INLAY_UI_CONTEXT_MENU>INLAY_UI_MENU &&
               INLAY_UI_AVATAR>INLAY_UI_CONTEXT_MENU && INLAY_UI_TOAST>INLAY_UI_AVATAR &&
               INLAY_UI_ACCORDION>INLAY_UI_TOAST && INLAY_UI_SKELETON>INLAY_UI_ACCORDION &&
               INLAY_UI_PORTAL>INLAY_UI_FORM && INLAY_UI_VIEW_ZONE>INLAY_UI_PORTAL &&
               INLAY_UI_DIFF_VIEW>INLAY_UI_VIEW_ZONE);
 const inlay::TerminalSnapshot tsnap=inlay::terminal_snapshot_default();
 if(tsnap.struct_size!=sizeof(inlay::TerminalSnapshot) || !tsnap.cursor_visible)return 1;
 const inlay::PortalState pstate=inlay::portal_state_default();
 if(pstate.struct_size!=sizeof(inlay::PortalState) || pstate.zoom_factor!=1.0f)return 1;
 const inlay::PortalFindResult pfind=inlay::portal_find_result_default();
 if(pfind.struct_size!=sizeof(inlay::PortalFindResult) || pfind.active ||
    inlay::portal_macos_options_default().struct_size!=sizeof(inlay::PortalMacOSOptions) ||
    inlay::portal_gtk_options_default().struct_size!=sizeof(inlay::PortalGtkOptions))return 1;
 (void)inlay::portal_native_backend();
 inlay::surface_set_console_callback(nullptr,nullptr);
 inlay::surface_set_code_tokenizer(nullptr,nullptr);
 inlay::surface_set_doc_hooks(nullptr,nullptr);
 (void)inlay::surface_bind_stream(nullptr,ETCH_STR_LIT("s"),nullptr);
 const inlay::PortalDesc portal=inlay_portal_desc_default();
 const inlay::SurfaceDesc surface=inlay_surface_desc_default();
 if(portal.struct_size!=sizeof(inlay::PortalDesc) || surface.struct_size!=sizeof(inlay::SurfaceDesc) ||
    surface.disable_jsx ||
    !portal.width || !inlay_portal_mock_backend() || !inlay_portal_destroy(nullptr) ||
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

 // Zero-cast C++20 wrapper verification with etch::Context* directly
 etch::Context* cpp_ctx=etch::create_context({.headless=true});
 if(!cpp_ctx)return 1;
 inlay::DocConfig zero_cfg={};
 inlay::Doc* cpp_doc=inlay::create_doc(cpp_ctx,&zero_cfg);
 inlay::Surface* cpp_surf=inlay::create_surface(cpp_ctx);
 inlay::Portal* cpp_portal=inlay::create_portal(cpp_ctx);
 inlay::DocStyle style=inlay::doc_style_default();
 style.has_background=true;
 style.highlight_bg=etch_rgba8(255,210,70,110);
 auto cpp_view=inlay::doc_view_desc_default();
 cpp_view.id=etch::id("cpp-zero-cast");cpp_view.width_px=240;cpp_view.height_px=0;
 cpp_view.style=&style;
 ok &= cpp_doc && cpp_portal && inlay::surface_next_timer_ms(nullptr)==-1 &&
       !inlay::surface_bind_slot(nullptr,ETCH_STR_LIT("slot"),render_cpp_slot) &&
       inlay::portal_eval_script(cpp_portal,ETCH_STR_LIT("1")) &&
       inlay::portal_add_init_script(cpp_portal,ETCH_STR_LIT("2")) &&
       inlay::portal_open_devtools(cpp_portal) &&
       inlay_doc_set_utf8(cpp_doc,ETCH_STR_LIT("Zero-cast ==C++20== wrapper"));
 bool cpp_slot_called=false;
 if(cpp_surf) {
  const inlay::SlotRenderFn slot_fn=render_cpp_slot;
  ok &= inlay::surface_next_timer_ms(cpp_surf)==-1 &&
        inlay::surface_bind_slot(cpp_surf,ETCH_STR_LIT("cpp_slot"),slot_fn,&cpp_slot_called) &&
        inlay_surface_set_script(cpp_surf,ETCH_STR_LIT("function render(){return <Card cls='p-2 gap-1'><Badge variant='success' dot>JSX</Badge><Slot name='cpp_slot' width={100} height={32} props={{n:7}} /></Card>;}"));
  inlay::SurfaceDesc no_jsx_desc=inlay::surface_desc_default();
  no_jsx_desc.disable_jsx=true;
  if(inlay::Surface* no_jsx=inlay::create_surface(cpp_ctx,&no_jsx_desc)) {
   ok &= !inlay_surface_set_script(no_jsx,ETCH_STR_LIT("function render(){return <Text>x</Text>;}")) &&
         inlay_surface_set_script(no_jsx,ETCH_STR_LIT("function render(){return ui.text('ok');}"));
   inlay_surface_destroy(no_jsx);
  } else {
   ok = false;
  }
 }
 if(ok) {
  auto surf_view=inlay::surface_view_desc_default();
  surf_view.id=etch::id("cpp-slot-view");surf_view.width_px=0.0f;surf_view.height_px=0.0f;
  inlay::doc_view_scroll_to_bottom(cpp_view.id,true);
  etch::begin_frame(cpp_ctx,{.viewport_w=320,.viewport_h=240});
  const inlay::DocResult r=inlay_doc_view(cpp_doc,&cpp_view);
  if(cpp_surf)inlay::surface_view(cpp_surf,&surf_view);
  etch::end_frame(cpp_ctx);
  inlay::doc_view_select_all(cpp_doc,cpp_view.id);
  char buf[128]={};
  const uint32_t n=inlay::doc_view_copy_plain_text(cpp_doc,cpp_view.id,buf,sizeof(buf));
  ok &= (!cpp_surf || cpp_slot_called) && r.measured_height_px>0 && r.visible_end==1 && r.at_bottom && inlay::doc_view_is_at_bottom(cpp_view.id) &&
        n>0 && std::strstr(buf,"Zero-cast C++20 wrapper")!=nullptr;
 }
 inlay_portal_destroy(cpp_portal);
 inlay_surface_destroy(cpp_surf);
 inlay_doc_destroy(cpp_doc);
 etch::destroy_context(cpp_ctx);
 return ok ? 0 : 1;
}
