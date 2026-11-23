#pragma once
#ifdef __cplusplus
#include <etch/etch.h>
#endif
#include "inlay_c.h"
namespace inlay {
using Portal=InlayPortal;
using PortalDesc=InlayPortalDesc;
using PortalViewDesc=InlayPortalViewDesc;
using PortalMode=InlayPortalMode;
using PortalDirtyRect=InlayPortalDirtyRect;
using PortalBackendVTable=InlayPortalBackendVTable;
using PortalStats=InlayPortalStats;
using PortalWebView2Options=InlayPortalWebView2Options;
using Surface=InlaySurface;
using SurfaceDesc=InlaySurfaceDesc;
using SurfaceViewDesc=InlaySurfaceViewDesc;
using SurfaceStats=InlaySurfaceStats;
using UiKind=InlayUiKind;
using UiNode=InlayUiNode;
using ActionFn=InlayActionFn;
using SlotRenderFn=InlaySlotRenderFn;
using DiagramDesc=InlayDiagramDesc;
using DiagramNode=InlayDiagramNode;
using DiagramEdge=InlayDiagramEdge;
using DiagramResult=InlayDiagramResult;
using MathDesc=InlayMathDesc;
using MathResult=InlayMathResult;
using Doc=InlayDoc;
using BlockKind=InlayBlockKind;
using AlertKind=InlayAlertKind;
using BlockFlags=InlayBlockFlags;
using Block=InlayBlock;
using DocConfig=InlayDocConfig;
using DocStyle=InlayDocStyle;
using DocViewDesc=InlayDocViewDesc;
using DocResult=InlayDocResult;
using DocSelection=InlayDocSelection;
using InlineKind=InlayInlineKind;
using InlineDecoratorFn=InlayInlineDecoratorFn;
using ImageResult=InlayImageResult;
using ImageResolverFn=InlayImageResolverFn;
using TaskToggleFn=InlayTaskToggleFn;
using CustomFenceEstimateFn=InlayCustomFenceEstimateFn;
using CustomFenceRenderFn=InlayCustomFenceRenderFn;
using CodeHeaderFn=InlayCodeHeaderFn;

inline uint32_t abi_version() noexcept { return inlay_abi_version(); }
inline EtchContext* to_c_context(etch::Context* ctx) noexcept {
 return reinterpret_cast<EtchContext*>(ctx);
}
inline const EtchContext* to_c_context(const etch::Context* ctx) noexcept {
 return reinterpret_cast<const EtchContext*>(ctx);
}
inline DocConfig doc_config_default() noexcept { return inlay_doc_config_default(); }
inline DocStyle doc_style_default() noexcept { return inlay_doc_style_default(); }
inline DocViewDesc doc_view_desc_default() noexcept { return inlay_doc_view_desc_default(); }
inline SurfaceDesc surface_desc_default() noexcept { return inlay_surface_desc_default(); }
inline SurfaceViewDesc surface_view_desc_default() noexcept { return inlay_surface_view_desc_default(); }
inline PortalDesc portal_desc_default() noexcept { return inlay_portal_desc_default(); }
inline PortalViewDesc portal_view_desc_default() noexcept { return inlay_portal_view_desc_default(); }
inline PortalWebView2Options portal_webview2_options_default() noexcept { return inlay_portal_webview2_options_default(); }
inline MathDesc math_desc_default() noexcept { return inlay_math_desc_default(); }
inline DiagramDesc diagram_desc_default() noexcept { return inlay_diagram_desc_default(); }
inline DiagramResult diagram_layout(EtchStr source,const DiagramDesc* desc) noexcept {
 return inlay_diagram_layout(source,desc);
}
inline DiagramResult diagram_view(EtchStr source,const DiagramDesc* desc) noexcept {
 return inlay_diagram_view(source,desc);
}
inline DocResult doc_view(Doc* doc,const DocViewDesc* desc) noexcept {
 return inlay_doc_view(doc,desc);
}
inline Doc* create_doc(etch::Context* ctx,const DocConfig* cfg=nullptr) noexcept {
 return inlay_doc_create(to_c_context(ctx),cfg);
}
inline Surface* create_surface(etch::Context* ctx,const SurfaceDesc* desc=nullptr) noexcept {
 return inlay_surface_create(to_c_context(ctx),desc);
}
inline Portal* create_portal(etch::Context* ctx,const PortalDesc* desc=nullptr) noexcept {
 return inlay_portal_create(to_c_context(ctx),desc);
}
inline bool doc_view_is_at_bottom(uint32_t view_id) noexcept {
 return inlay_doc_view_is_at_bottom(view_id);
}
inline void doc_view_scroll_to_bottom(uint32_t view_id,bool stop_momentum=true) noexcept {
 inlay_doc_view_scroll_to_bottom(view_id,stop_momentum);
}
inline DocSelection doc_view_selection(const Doc* doc,uint32_t view_id) noexcept {
 return inlay_doc_view_selection(doc,view_id);
}
inline void doc_view_set_selection(Doc* doc,uint32_t view_id,uint32_t anchor,uint32_t focus) noexcept {
 inlay_doc_view_set_selection(doc,view_id,anchor,focus);
}
inline void doc_view_select_all(Doc* doc,uint32_t view_id) noexcept {
 inlay_doc_view_select_all(doc,view_id);
}
inline void doc_view_clear_selection(Doc* doc,uint32_t view_id) noexcept {
 inlay_doc_view_clear_selection(doc,view_id);
}
inline uint32_t doc_view_copy_plain_text(const Doc* doc,uint32_t view_id,char* out_buf,uint32_t cap) noexcept {
 return inlay_doc_view_copy_plain_text(doc,view_id,out_buf,cap);
}
inline uint32_t doc_view_copy_markdown(const Doc* doc,uint32_t view_id,char* out_buf,uint32_t cap) noexcept {
 return inlay_doc_view_copy_markdown(doc,view_id,out_buf,cap);
}
inline int32_t surface_next_timer_ms(const Surface* s) noexcept {
 return inlay_surface_next_timer_ms(s);
}
inline bool surface_bind_slot(Surface* s, EtchStr name, SlotRenderFn fn, void* user_data=nullptr) noexcept {
 return inlay_surface_bind_slot(s, name, fn, user_data);
}
/* width_px <= 0 or height_px <= 0 grows to fill the parent container (etch_sizing_grow). */
inline void surface_view(Surface* s, const SurfaceViewDesc* desc) noexcept {
 inlay_surface_view(s, desc);
}
}
