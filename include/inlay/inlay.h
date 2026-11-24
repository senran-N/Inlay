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
using AsyncActionEvent=InlayAsyncActionEvent;
using Inbox=InlayInbox;
using InboxDesc=InlayInboxDesc;
using InboxMessage=InlayInboxMessage;
using InboxStatus=InlayInboxStatus;
using SlotRenderFn=InlaySlotRenderFn;
using DiagramDesc=InlayDiagramDesc;
using DiagramNode=InlayDiagramNode;
using DiagramEdge=InlayDiagramEdge;
using DiagramResult=InlayDiagramResult;
using DiagramDiagnosticKind=InlayDiagramDiagnosticKind;
using DiagramDiagnosticFn=InlayDiagramDiagnosticFn;
using MathDesc=InlayMathDesc;
using MathResult=InlayMathResult;
using MathError=InlayMathError;
using Doc=InlayDoc;
using DocAnchor=InlayDocAnchor;
using DocStreamStatus=InlayDocStreamStatus;
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
using SlotMeasureFn=InlaySlotMeasureFn;
using ActionPermission=InlayActionPermission;
using ActionPolicy=InlayActionPolicy;
using ActionAuditFn=InlayActionAuditFn;
using Model=InlayModel;
using ModelRow=InlayModelRow;
using ModelReadFn=InlayModelReadFn;
using Stream=InlayStream;
using StreamRole=InlayStreamRole;
using StreamItemKind=InlayStreamItemKind;
using StreamStatus=InlayStreamStatus;
using StreamItem=InlayStreamItem;
using GenUiValidation=InlayGenUiValidation;
using ImageLoader=InlayImageLoader;
using ImageLoadStatus=InlayImageLoadStatus;
using ImageLoaderDesc=InlayImageLoaderDesc;
using PortalNavDecision=InlayPortalNavDecision;
using PortalNavFn=InlayPortalNavFn;
using PortalMessageFn=InlayPortalMessageFn;
using PortalPermissionFn=InlayPortalPermissionFn;
using PortalPolicy=InlayPortalPolicy;

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
inline InboxDesc inbox_desc_default() noexcept { return inlay_inbox_desc_default(); }
inline Inbox* create_inbox(const InboxDesc* desc=nullptr) noexcept { return inlay_inbox_create(desc); }
inline InboxStatus inbox_post(Inbox* inbox,uint32_t kind,uint32_t id,const void* payload,uint32_t size) noexcept {
 return inlay_inbox_post(inbox,kind,id,payload,size);
}
inline InboxStatus inbox_take(Inbox* inbox,InboxMessage* message,void* payload,uint32_t capacity) noexcept {
 return inlay_inbox_take(inbox,message,payload,capacity);
}
inline uint32_t inbox_pending(Inbox* inbox) noexcept { return inlay_inbox_pending(inbox); }
inline void inbox_close(Inbox* inbox) noexcept { inlay_inbox_close(inbox); }
inline void destroy_inbox(Inbox* inbox) noexcept { inlay_inbox_destroy(inbox); }
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
inline DocStreamStatus doc_stream_status(const Doc* doc) noexcept {
 return inlay_doc_stream_status(doc);
}
inline bool doc_replace_utf8(Doc* doc,uint32_t begin,uint32_t end,EtchStr text) noexcept {
 return inlay_doc_replace_utf8(doc,begin,end,text);
}
inline uint32_t doc_revision(const Doc* doc) noexcept {
 return inlay_doc_revision(doc);
}
inline bool doc_set_anchor(Doc* doc,DocAnchor anchor) noexcept { return inlay_doc_set_anchor(doc,anchor); }
inline bool doc_get_anchor(const Doc* doc,uint64_t id,DocAnchor* out) noexcept { return inlay_doc_get_anchor(doc,id,out); }
inline bool doc_remove_anchor(Doc* doc,uint64_t id) noexcept { return inlay_doc_remove_anchor(doc,id); }
inline uint32_t doc_anchor_count(const Doc* doc) noexcept { return inlay_doc_anchor_count(doc); }
inline bool doc_anchor_at(const Doc* doc,uint32_t i,DocAnchor* out) noexcept { return inlay_doc_anchor_at(doc,i,out); }
inline bool doc_view_is_at_bottom(uint32_t view_id) noexcept {
 return inlay_doc_view_is_at_bottom(view_id);
}
inline void doc_view_scroll_to_bottom(uint32_t view_id,bool stop_momentum=true) noexcept {
 inlay_doc_view_scroll_to_bottom(view_id,stop_momentum);
}
inline DocSelection doc_view_selection(const Doc* doc,uint32_t view_id) noexcept {
 return inlay_doc_view_selection(doc,view_id);
}
inline DocSelection doc_view_find_text(const Doc* doc,uint32_t view_id,EtchStr query,uint32_t from_byte=0u) noexcept {
 return inlay_doc_view_find_text(doc,view_id,query,from_byte);
}
inline bool doc_view_reveal_text(Doc* doc,uint32_t view_id,uint32_t byte_offset,EtchAlignY align=ETCH_ALIGN_Y_TOP) noexcept {
 return inlay_doc_view_reveal_text(doc,view_id,byte_offset,align);
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
inline bool surface_allow_async_action(Surface* s, EtchStr name) noexcept {
 return inlay_surface_allow_async_action(s, name);
}
inline AsyncActionEvent surface_poll_async_action(Surface* s) noexcept {
 return inlay_surface_poll_async_action(s);
}
inline bool surface_complete_async_action(Surface* s, uint32_t request_id, bool success, EtchStr result_json) noexcept {
 return inlay_surface_complete_async_action(s, request_id, success, result_json);
}
inline void surface_close_async_actions(Surface* s) noexcept {
 inlay_surface_close_async_actions(s);
}
inline bool surface_bind_slot(Surface* s, EtchStr name, SlotRenderFn fn, void* user_data=nullptr) noexcept {
 return inlay_surface_bind_slot(s, name, fn, user_data);
}
inline bool surface_bind_document(Surface* s, EtchStr name, Doc* doc) noexcept {
 return inlay_surface_bind_document(s, name, doc);
}
inline bool surface_bind_text_buffer(Surface* s, EtchStr name, EtchTextBuffer* buffer) noexcept {
 return inlay_surface_bind_text_buffer(s, name, buffer);
}
inline bool surface_set_image_resolver(Surface* s, ImageResolverFn fn, void* user_data=nullptr) noexcept {
 return inlay_surface_set_image_resolver(s, fn, user_data);
}
/* width_px <= 0 or height_px <= 0 grows to fill the parent container (etch_sizing_grow). */
inline void surface_view(Surface* s, const SurfaceViewDesc* desc) noexcept {
 inlay_surface_view(s, desc);
}
inline ActionPolicy action_policy_default() noexcept { return inlay_action_policy_default(); }
inline PortalPolicy portal_policy_default() noexcept { return inlay_portal_policy_default(); }
inline ImageLoaderDesc image_loader_desc_default() noexcept { return inlay_image_loader_desc_default(); }
inline ImageLoader* create_image_loader(etch::Context* ctx, const ImageLoaderDesc* desc=nullptr) noexcept {
 return inlay_image_loader_create(to_c_context(ctx), desc);
}
}
