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
using DiagramDesc=InlayDiagramDesc;
using DiagramResult=InlayDiagramResult;
using MathDesc=InlayMathDesc;
using MathResult=InlayMathResult;
using Doc=InlayDoc;
using BlockKind=InlayBlockKind;
using Block=InlayBlock;
using DocConfig=InlayDocConfig;
using DocStyle=InlayDocStyle;
using DocViewDesc=InlayDocViewDesc;
using DocResult=InlayDocResult;
using InlineKind=InlayInlineKind;
using InlineDecoratorFn=InlayInlineDecoratorFn;
using ImageResult=InlayImageResult;
using ImageResolverFn=InlayImageResolverFn;
using TaskToggleFn=InlayTaskToggleFn;
using CustomFenceEstimateFn=InlayCustomFenceEstimateFn;
using CustomFenceRenderFn=InlayCustomFenceRenderFn;
using CodeHeaderFn=InlayCodeHeaderFn;

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
inline MathDesc math_desc_default() noexcept { return inlay_math_desc_default(); }
inline DiagramDesc diagram_desc_default() noexcept { return inlay_diagram_desc_default(); }
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
inline int32_t surface_next_timer_ms(const Surface* s) noexcept {
 return inlay_surface_next_timer_ms(s);
}
}
