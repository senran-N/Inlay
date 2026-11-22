#include <inlay/inlay_c.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_iostream.h>
#include <cmath>
#include <cstdio>
#include <cstring>
namespace {
// ASan instruments the allocator teardown after the VM deadline fires.
// Keep the production wall-clock budget strict in uninstrumented builds.
#if defined(__SANITIZE_ADDRESS__)
constexpr double native_conversion_limit_ms = 400;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
constexpr double native_conversion_limit_ms = 400;
#else
constexpr double native_conversion_limit_ms = 40;
#endif
#else
constexpr double native_conversion_limit_ms = 40;
#endif
bool check(bool value, const char *message) {
    if (!value)
        std::printf("FAIL: %s\n", message);
    return value;
}
bool write_file(const char *path, const char *source) {
    FILE *file = nullptr;
#ifdef _WIN32
    if (fopen_s(&file, path, "wb"))
        return false;
#else
    file = std::fopen(path, "wb");
    if (!file)
        return false;
#endif
    const auto size = std::strlen(source);
    const bool ok = std::fwrite(source, 1, size, file) == size;
    return std::fclose(file) == 0 && ok;
}
void action(InlaySurface *surface, EtchStr payload, void *user) {
    ++*static_cast<int *>(user);
    if (payload.length)
        inlay_surface_set_state_json(surface, ETCH_STR_LIT("{\"count\":9}"));
}
bool reactive_controls(EtchContext *ctx, const InlaySurfaceDesc &desc) {
    auto *surface = inlay_surface_create(ctx, &desc);
    if (!surface)
        return false;
    bool ok = check(inlay_surface_set_script(surface, ETCH_STR_LIT(
        "function render(s){return h('box',{axis:'column',width:200,bg:'var(--etch-border-subtle)'},"
        "h('input',{value:s.query||'',placeholder:'Filter...',onChange:'setQuery',width:-1}),"
        "h('input',{value:'other'}),h('toggle',{label:'Live',checked:!!s.live,onChange:'setLive'}),"
        "h('text',{text:'status',color:'var(--etch-text-muted)'}));}"
        "function onEvent(n,v,s){if(n==='setQuery')s.query=v;if(n==='setLive')s.live=v;}")),
        "input and toggle script");
    uint32_t count = 0;
    const auto *nodes = inlay_surface_nodes(surface, &count);
    if (count != 5 || nodes[1].kind != INLAY_UI_INPUT || nodes[3].kind != INLAY_UI_TOGGLE) {
        inlay_surface_destroy(surface);
        return check(false, "input and toggle cached nodes");
    }
    InlaySurfaceViewDesc view = inlay_surface_view_desc_default();
    view.id = etch_id("reactive-controls");
    view.width_px = 300;
    view.height_px = 200;
    const uint32_t root = etch_id_index(view.id,nodes[0].id);
    const uint32_t input = etch_id_index(root,nodes[1].id);
    const uint32_t toggle = etch_id_index(root,nodes[3].id);
    auto frame = etch_frame_desc_default();
    frame.viewport_w = 300;frame.viewport_h = 200;
    const auto step = [&] {
        etch_begin_frame(ctx, &frame);
        inlay_surface_view(surface, &view);
        etch_end_frame(ctx);
    };
    const auto ticks = [&] { return inlay_surface_stats(surface).js_ticks; };
    const auto before = ticks();
    step();step();
    ok &= check(ticks() == before, "idle controls run zero JS ticks");
    ok &= check(etch_element_bounds(input).w >= 199, "input grow width");
    ok &= check(etch_perform_a11y_action(input, ETCH_A11Y_ACT_FOCUS, 0, {}, ctx, nullptr),
                "focus input");
    step();
    SDL_Event event = {};
    event.type = SDL_EVENT_TEXT_EDITING;
    event.edit.text = "拼";
    event.edit.start = 1;
    etch_process_event(ctx, &event);
    step();
    ok &= check(ticks() == before, "IME composition runs zero JS ticks");
    event = {};
    event.type = SDL_EVENT_TEXT_INPUT;
    event.text.text = "拼\"\\";
    etch_process_event(ctx, &event);
    step();
    ok &= check(ticks() == before+1, "committed edit dispatched once");
    auto json = inlay_surface_state_json(surface);
    ok &= check(json.chars && !std::strcmp(json.chars, "{\"query\":\"拼\\\"\\\\\"}"),
                "JSON quotes, backslash and UTF-8 survive input");
    event.text.text = "字";
    etch_process_event(ctx, &event);
    step();
    json = inlay_surface_state_json(surface);
    ok &= check(ticks() == before+2 && json.chars &&
                !std::strcmp(json.chars, "{\"query\":\"拼\\\"\\\\字\"}"),
                "successive controlled edits preserve caret and text");
    ok &= check(etch_perform_a11y_action(toggle, ETCH_A11Y_ACT_PRESS, 0, {}, ctx, nullptr),
                "activate toggle");
    step();step();
    json = inlay_surface_state_json(surface);
    ok &= check(ticks() == before+3 && json.chars && std::strstr(json.chars, "\"live\":true"),
                "toggle dispatches true once");
    ok &= check(etch_perform_a11y_action(toggle, ETCH_A11Y_ACT_PRESS, 0, {}, ctx, nullptr),
                "activate toggle again");
    step();step();
    json = inlay_surface_state_json(surface);
    ok &= check(ticks() == before+4 && json.chars && std::strstr(json.chars, "\"live\":false"),
                "toggle dispatches false once");
    for (uint32_t i = 0; i < 16; ++i)
        step();
    ok &= check(ticks() == before+4, "controls return to idle without JS ticks");
    inlay_surface_destroy(surface);
    return ok;
}
bool has_rect_prim(EtchContext *ctx, EtchRect bounds, uint32_t fill_rgba, uint32_t border_rgba, bool want_radius) {
    for (uint32_t i = 0; i < etch_frame_prim_count(ctx); ++i) {
        EtchPrim prim = {};
        if (!etch_get_frame_prim(ctx, i, &prim))
            continue;
        if (prim.bounds.x < bounds.x - 1.f || prim.bounds.y < bounds.y - 1.f ||
            prim.bounds.x + prim.bounds.z > bounds.x + bounds.w + 1.f ||
            prim.bounds.y + prim.bounds.w > bounds.y + bounds.h + 1.f)
            continue;
        const bool fill_ok = !fill_rgba || prim.style.y == fill_rgba;
        const bool border_ok = !border_rgba || prim.style.z == border_rgba;
        const bool rad_ok = !want_radius || prim.payload.z != 0;
        if (fill_ok && border_ok && rad_ok)
            return true;
    }
    return false;
}
bool has_text_prim(EtchContext *ctx, EtchRect bounds, uint32_t color_rgba) {
    for (uint32_t i = 0; i < etch_frame_prim_count(ctx); ++i) {
        EtchPrim prim = {};
        if (!etch_get_frame_prim(ctx, i, &prim))
            continue;
        if (prim.bounds.x >= bounds.x - 1.f && prim.bounds.y >= bounds.y - 1.f &&
            prim.bounds.x + prim.bounds.z <= bounds.x + bounds.w + 1.f &&
            prim.bounds.y + prim.bounds.w <= bounds.y + bounds.h + 1.f &&
            prim.style.y == color_rgba)
            return true;
    }
    return false;
}
bool styled_surface_controls(EtchContext *ctx, const InlaySurfaceDesc &desc) {
    auto *surface = inlay_surface_create(ctx, &desc);
    if (!surface)
        return false;
    bool ok = check(inlay_surface_set_script(surface, ETCH_STR_LIT(
        "function render(s){return h('box',{id:'root',axis:'column',width:320,height:240,"
        "bg:'#123456',borderColor:'#abcdef',borderBottom:3,radius:12,alignY:'bottom',padding:10,gap:8},"
        "h('button',{id:'btn',text:'Save',bg:'#224466',color:'#fefefe',borderColor:'#ff3366',borderWidth:2,"
        "radius:10,width:140,height:44,fontSize:16,padding:8,onClick:'save'}),"
        "h('button',{id:'flat',text:'Flat',bg:'#335577',color:'#11ffee',radius:6,width:110,height:36}),"
        "h('input',{id:'inp',value:'abc',bg:'#102030',color:'#eeeeee',borderColor:'#44cc88',borderWidth:2,"
        "radius:8,width:200,height:42,fontSize:14,padding:6}));}"
        "function onEvent(n,v,s){if(n==='save')s.saved=(s.saved||0)+1;}")),
        "styled surface script");
    InlaySurfaceViewDesc view = inlay_surface_view_desc_default();
    view.id = etch_id("styled-surface");
    view.width_px = 400;
    view.height_px = 300;
    uint32_t count = 0;
    const auto *nodes = inlay_surface_nodes(surface, &count);
    if (count != 4) {
        inlay_surface_destroy(surface);
        return check(false, "styled node count");
    }
    const uint32_t root_id = etch_id_index(view.id, nodes[0].id);
    const uint32_t btn_id = etch_id_index(root_id, nodes[1].id);
    const uint32_t flat_id = etch_id_index(root_id, nodes[2].id);
    const uint32_t inp_id = etch_id_index(root_id, nodes[3].id);
    auto frame = etch_frame_desc_default();
    frame.viewport_w = 400;
    frame.viewport_h = 300;
    etch_begin_frame(ctx, &frame);
    inlay_surface_view(surface, &view);
    etch_end_frame(ctx);
    const auto root_b = etch_element_bounds(root_id);
    const auto btn_b = etch_element_bounds(btn_id);
    const auto flat_b = etch_element_bounds(flat_id);
    const auto inp_b = etch_element_bounds(inp_id);
    ok &= check(root_b.w == 320 && root_b.h == 240, "styled box dimensions");
    ok &= check(btn_b.w == 140 && btn_b.h == 44, "styled bordered button dimensions");
    ok &= check(flat_b.w == 110 && flat_b.h == 36, "styled flat button dimensions");
    ok &= check(inp_b.w == 200 && inp_b.h == 42, "styled input dimensions");
    ok &= check(inp_b.y + inp_b.h >= root_b.y + root_b.h - 11.f, "box alignY bottom positions last child at bottom");
    const uint32_t box_bg = etch_color_pack_rgba8(etch_rgba8(0x12, 0x34, 0x56, 255));
    const uint32_t box_border = etch_color_pack_rgba8(etch_rgba8(0xab, 0xcd, 0xef, 255));
    const uint32_t btn_bg = etch_color_pack_rgba8(etch_rgba8(0x22, 0x44, 0x66, 255));
    const uint32_t btn_fg = etch_color_pack_rgba8(etch_rgba8(0xfe, 0xfe, 0xfe, 255));
    const uint32_t btn_border = etch_color_pack_rgba8(etch_rgba8(0xff, 0x33, 0x66, 255));
    const uint32_t flat_bg = etch_color_pack_rgba8(etch_rgba8(0x33, 0x55, 0x77, 255));
    const uint32_t flat_fg = etch_color_pack_rgba8(etch_rgba8(0x11, 0xff, 0xee, 255));
    const uint32_t inp_bg = etch_color_pack_rgba8(etch_rgba8(0x10, 0x20, 0x30, 255));
    const uint32_t inp_fg = etch_color_pack_rgba8(etch_rgba8(0xee, 0xee, 0xee, 255));
    const uint32_t inp_border = etch_color_pack_rgba8(etch_rgba8(0x44, 0xcc, 0x88, 255));
    ok &= check(has_rect_prim(ctx, root_b, box_bg, 0, true), "box bg and radius");
    ok &= check(has_rect_prim(ctx, root_b, box_border, 0, false), "box borderBottom color");
    ok &= check(has_rect_prim(ctx, btn_b, btn_bg, btn_border, true), "bordered button bg, border and radius");
    ok &= check(has_text_prim(ctx, btn_b, btn_fg), "bordered button text color");
    ok &= check(has_rect_prim(ctx, flat_b, flat_bg, 0, true), "flat button bg and radius");
    ok &= check(has_text_prim(ctx, flat_b, flat_fg), "flat button text color");
    ok &= check(has_rect_prim(ctx, inp_b, inp_bg, inp_border, true), "input bg, border and radius");
    ok &= check(has_text_prim(ctx, inp_b, inp_fg), "input text color");
    ok &= check(etch_perform_a11y_action(btn_id, ETCH_A11Y_ACT_PRESS, 0, {}, ctx, nullptr),
                "press styled bordered button");
    etch_begin_frame(ctx, &frame);
    inlay_surface_view(surface, &view);
    etch_end_frame(ctx);
    const auto json = inlay_surface_state_json(surface);
    ok &= check(json.chars && std::strstr(json.chars, "\"saved\":1"), "styled button onClick fires");
    inlay_surface_destroy(surface);
    return ok;
}
bool expanded_surface_primitives(EtchContext *ctx, const InlaySurfaceDesc &desc) {
    auto *surface = inlay_surface_create(ctx, &desc);
    if (!surface)
        return false;
    bool ok = check(inlay_surface_set_script(surface, ETCH_STR_LIT(
        "function render(s){return ui.scroll({id:'scr',width:280,height:160,padding:8,gap:6,scrollY:true},"
        "ui.badge({id:'bdg',text:'v0.9',bg:'#224488',color:'#eef4ff'}),"
        "ui.divider({id:'div',color:'#445566'}),"
        "ui.progress({id:'prg',value:s.gain||50,min:0,max:100,width:240,height:14,color:'#33cc88',bg:'#1a2230'}),"
        "ui.slider({id:'sld',label:'Gain',value:s.gain||50,min:0,max:100,step:10,width:240,action:'setGain'}),"
        "ui.image({id:'img',alt:'cover',width:64,height:48,opacity:0.75}));}"
        "function onEvent(n,v,s){if(n==='setGain')s.gain=v;}")),
        "expanded ui primitives script");
    uint32_t count = 0;
    const auto *nodes = inlay_surface_nodes(surface, &count);
    if (count != 6 || nodes[0].kind != INLAY_UI_SCROLL || nodes[1].kind != INLAY_UI_BADGE ||
        nodes[2].kind != INLAY_UI_DIVIDER || nodes[3].kind != INLAY_UI_PROGRESS ||
        nodes[4].kind != INLAY_UI_SLIDER || nodes[5].kind != INLAY_UI_IMAGE) {
        inlay_surface_destroy(surface);
        return check(false, "expanded ui node kinds");
    }
    InlaySurfaceViewDesc view = inlay_surface_view_desc_default();
    view.id = etch_id("expanded-ui");
    view.width_px = 320;
    view.height_px = 220;
    const uint32_t scr_id = etch_id_index(view.id, nodes[0].id);
    const uint32_t bdg_id = etch_id_index(scr_id, nodes[1].id);
    const uint32_t div_id = etch_id_index(scr_id, nodes[2].id);
    const uint32_t prg_id = etch_id_index(scr_id, nodes[3].id);
    const uint32_t sld_id = etch_id_index(scr_id, nodes[4].id);
    const uint32_t img_id = etch_id_index(scr_id, nodes[5].id);
    auto frame = etch_frame_desc_default();
    frame.viewport_w = 320;
    frame.viewport_h = 220;
    etch_begin_frame(ctx, &frame);
    inlay_surface_view(surface, &view);
    etch_end_frame(ctx);
    const auto scr_b = etch_element_bounds(scr_id);
    const auto bdg_b = etch_element_bounds(bdg_id);
    const auto div_b = etch_element_bounds(div_id);
    const auto prg_b = etch_element_bounds(prg_id);
    const auto sld_b = etch_element_bounds(sld_id);
    const auto img_b = etch_element_bounds(img_id);
    ok &= check(scr_b.w == 280 && scr_b.h == 160, "scroll dimensions");
    ok &= check(bdg_b.w > 16 && bdg_b.h > 12, "badge dimensions");
    ok &= check(div_b.h == 1 && div_b.w > 100, "divider dimensions");
    ok &= check(prg_b.w == 240 && prg_b.h == 14, "progress dimensions");
    ok &= check(sld_b.w == 240 && sld_b.h > 16, "slider dimensions");
    ok &= check(img_b.w == 64 && img_b.h == 48, "image placeholder dimensions");
    const uint64_t idle_before = inlay_surface_stats(surface).js_ticks;
    etch_begin_frame(ctx, &frame);
    inlay_surface_view(surface, &view);
    etch_end_frame(ctx);
    ok &= check(inlay_surface_stats(surface).js_ticks == idle_before, "expanded primitives idle zero ticks");
    ok &= check(etch_perform_a11y_action(sld_id, ETCH_A11Y_ACT_INCREMENT, 0, {}, ctx, nullptr),
                "increment slider via a11y");
    etch_begin_frame(ctx, &frame);
    inlay_surface_view(surface, &view);
    etch_end_frame(ctx);
    const auto json = inlay_surface_state_json(surface);
    ok &= check(json.chars && std::strstr(json.chars, "\"gain\":60") != nullptr,
                "slider increment updates state via action");
    ok &= check(inlay_surface_set_script(surface, ETCH_STR_LIT(
        "function render(){return ui.image({spriteId:4294967295,iconId:65534,width:32,height:32});}")),
        "max valid spriteId and iconId");
    ok &= check(!inlay_surface_set_script(surface, ETCH_STR_LIT(
        "function render(){return ui.image({spriteId:4294967296});}")),
        "out-of-range spriteId rejected");
    ok &= check(!inlay_surface_set_script(surface, ETCH_STR_LIT(
        "function render(){return ui.image({iconId:65535});}")),
        "invalid iconId rejected");
    ok &= check(!inlay_surface_set_script(surface, ETCH_STR_LIT(
        "function render(){return ui.image({spriteId:1.5});}")),
        "fractional spriteId rejected");
    ok &= check(!inlay_surface_set_script(surface, ETCH_STR_LIT(
        "function render(){return ui.slider({value:1e100});}")),
        "overflow float slider value rejected");
    inlay_surface_destroy(surface);
    return ok;
}
bool i2_reactive_reconciler_upgrades(EtchContext *ctx, const InlaySurfaceDesc &desc) {
    auto *s = inlay_surface_create(ctx, &desc);
    if (!s)
        return false;
    bool ok = check(inlay_surface_next_timer_ms(nullptr) == -1 && inlay_surface_next_timer_ms(s) == -1,
                    "no timers returns -1");
    auto frame = etch_frame_desc_default();
    frame.viewport_w = 400;
    frame.viewport_h = 300;
    InlaySurfaceViewDesc view = inlay_surface_view_desc_default();
    view.id = etch_id("i2-reconciler");
    view.width_px = 360;
    view.height_px = 260;
    const auto step = [&] {
        etch_begin_frame(ctx, &frame);
        inlay_surface_view(s, &view);
        etch_end_frame(ctx);
    };

    // 1. Zero-idle timer deadline: 5000ms timer reports remaining ms without spinning redraws.
    ok &= check(inlay_surface_set_script(s, ETCH_STR_LIT(
        "setTimeout(()=>{state.fired=true},5000);"
        "function render(){return ui.text('wait');}")),
        "future timer script");
    for (int i = 0; i < 6; ++i)
        step();
    const int32_t next_ms = inlay_surface_next_timer_ms(s);
    ok &= check(next_ms > 1000 && next_ms <= 5000, "next_timer_ms reports future deadline");
    ok &= check(!etch_needs_redraw(ctx), "future timer settles idle without busy-spin");

    // 2. Sibling-scoped & keyed node ID stability across conditional mount/unmount and list prepend.
    ok &= check(inlay_surface_set_script(s, ETCH_STR_LIT(
        "function render(st){"
        "const items=st.prepend?['z','a','b']:['a','b'];"
        "return ui.box({axis:'column'},"
        "  st.banner?ui.text('Banner'):null,"
        "  ui.input({value:st.q||'',onChange:v=>{st.q=v}}),"
        "  ...items.map(k=>ui.text({key:k,text:k})));}"
        "function onEvent(n,v,st){if(n==='banner')st.banner=v;if(n==='prepend')st.prepend=v;}")),
        "keyed/sibling script");
    uint32_t cnt = 0;
    const auto *n0 = inlay_surface_nodes(s, &cnt);
    ok &= check(cnt == 4 && n0[1].kind == INLAY_UI_INPUT, "initial sibling/keyed count");
    const uint32_t input_id_before = cnt >= 4 ? n0[1].id : 0u;
    const uint32_t key_a_before = cnt >= 4 ? n0[2].id : 0u;
    const uint32_t root_eid = etch_id_index(view.id, n0[0].id);
    const uint32_t inp_eid = etch_id_index(root_eid, input_id_before);
    step();
    ok &= check(etch_perform_a11y_action(inp_eid, ETCH_A11Y_ACT_FOCUS, 0, {}, ctx, nullptr),
                "focus sibling input");
    step();
    ok &= check(inlay_surface_dispatch(s, ETCH_STR_LIT("banner"), ETCH_STR_LIT("true")),
                "mount conditional sibling before input");
    ok &= check(inlay_surface_dispatch(s, ETCH_STR_LIT("prepend"), ETCH_STR_LIT("true")),
                "prepend keyed item before 'a'");
    const auto *n1 = inlay_surface_nodes(s, &cnt);
    ok &= check(cnt == 6 && n1[2].kind == INLAY_UI_INPUT && n1[2].id == input_id_before,
                "conditional mount keeps input node ID invariant");
    ok &= check(cnt == 6 && n1[4].id == key_a_before,
                "keyed list prepend keeps 'a' node ID invariant");
    step();
    // Typing into the still-focused input exercises both focus preservation AND closure onChange!
    SDL_Event ev = {};
    ev.type = SDL_EVENT_TEXT_INPUT;
    ev.text.text = "ok";
    etch_process_event(ctx, &ev);
    step();
    const auto st_json = inlay_surface_state_json(s);
    ok &= check(st_json.chars && std::strstr(st_json.chars, "\"q\":\"ok\"") != nullptr,
                "focus preserved across conditional mount + closure onChange fires");

    // 3. First-class JS closures + useState hook + event batching (2 widgets -> 1 JS tick).
    ok &= check(inlay_surface_set_state_json(s, ETCH_STR_LIT("{}")), "clear state for hooks");
    ok &= check(inlay_surface_set_script(s, ETCH_STR_LIT(
        "function render(){"
        "const [count,setCount]=useState(10);"
        "const [on,setOn]=useState(false);"
        "return ui.box({axis:'column'},"
        "  ui.text('C='+count+','+(on?'Y':'N')),"
        "  ui.button({text:'+1',onClick:()=>setCount(c=>c+1)}),"
        "  ui.toggle({label:'Flag',checked:on,onChange:v=>setOn(v)}));}")),
        "useState + closure script");
    const auto *hn = inlay_surface_nodes(s, &cnt);
    ok &= check(cnt == 4 && hn[1].text.length == 6 && !std::memcmp(hn[1].text.chars, "C=10,N", 6),
                "useState initial render");
    const uint32_t hroot = etch_id_index(view.id, hn[0].id);
    const uint32_t hbtn = etch_id_index(hroot, hn[2].id);
    const uint32_t htog = etch_id_index(hroot, hn[3].id);
    step();
    const uint64_t ticks_before_batch = inlay_surface_stats(s).js_ticks;
    // Trigger BOTH button press and toggle press in the same frame!
    ok &= check(etch_perform_a11y_action(hbtn, ETCH_A11Y_ACT_PRESS, 0, {}, ctx, nullptr),
                "queue hook button press");
    ok &= check(etch_perform_a11y_action(htog, ETCH_A11Y_ACT_PRESS, 0, {}, ctx, nullptr),
                "queue hook toggle press");
    step();
    ok &= check(inlay_surface_stats(s).js_ticks == ticks_before_batch + 1,
                "multiple widget events batch into 1 JS tick");
    hn = inlay_surface_nodes(s, &cnt);
    ok &= check(cnt == 4 && hn[1].text.length == 6 && !std::memcmp(hn[1].text.chars, "C=11,Y", 6),
                "useState closures updated both states");
    const auto clean_state = inlay_surface_state_json(s);
    ok &= check(clean_state.length == 2 && !std::memcmp(clean_state.chars, "{}", 2),
                "useState does not pollute state JSON");

    // 4. Lazy state JSON serialization: 3 dispatches do NOT invoke toJSON until state_json() is read.
    ok &= check(inlay_surface_set_script(s, ETCH_STR_LIT(
        "let ser=0;state.n=0;state.toJSON=()=>{ser++;return {n:state.n,ser};};"
        "function render(st){return ui.text(String(st.n));}"
        "function onEvent(name,v,st){if(name==='inc')st.n++;}")),
        "lazy state serialization script");
    for (int i = 0; i < 3; ++i)
        ok &= inlay_surface_dispatch(s, ETCH_STR_LIT("inc"), ETCH_STR_LIT("null"));
    const auto lazy_json = inlay_surface_state_json(s);
    ok &= check(lazy_json.chars && std::strstr(lazy_json.chars, "\"n\":3") != nullptr &&
                    std::strstr(lazy_json.chars, "\"ser\":2") != nullptr,
                "state JSON serialized lazily on read, not per dispatch tick");

    inlay_surface_destroy(s);
    return ok;
}
struct SlotProbe {
    InlaySurface *expected_surface;
    uint32_t calls;
    uint32_t last_eid;
    float last_w, last_h;
    char last_props[128];
    const char *deferred_state_json;
    bool deferred_set_ok;
    bool deferred_reenter_blocked;
};
void slot_probe_cb(InlaySurface *surface, uint32_t element_id, float width_px, float height_px,
                   EtchStr props_json, void *user) {
    auto *p = static_cast<SlotProbe *>(user);
    if (surface == p->expected_surface)
        ++p->calls;
    p->last_eid = element_id;
    p->last_w = width_px;
    p->last_h = height_px;
    const uint32_t n = props_json.length < sizeof(p->last_props) - 1u
                           ? props_json.length
                           : uint32_t(sizeof(p->last_props) - 1u);
    if (props_json.chars && n > 0u)
        std::memcpy(p->last_props, props_json.chars, n);
    p->last_props[n] = '\0';
    if (p->deferred_state_json) {
        const char *json = p->deferred_state_json;
        p->deferred_state_json = nullptr;
        p->deferred_set_ok = inlay_surface_set_state_json(
            surface, {json, uint32_t(std::strlen(json)), false});
        p->deferred_reenter_blocked =
            !inlay_surface_dispatch(surface, ETCH_STR_LIT("noop"), ETCH_STR_LIT("null"));
    }
    auto box = etch_box_desc_default();
    box.width = etch_sizing_grow(0, 0);
    box.height = etch_sizing_grow(0, 0);
    etch_begin_box(&box);
    etch_text(ETCH_STR_LIT("Native slot"), nullptr);
    etch_end_box();
}
uint32_t find_surface_eid_rec(const InlayUiNode *nodes, uint32_t count, uint32_t idx,
                              uint32_t parent_eid, uint32_t target_node_id) {
    if (idx >= count)
        return 0u;
    const uint32_t eid = etch_id_index(parent_eid, nodes[idx].id);
    if (nodes[idx].id == target_node_id)
        return eid;
    for (uint32_t c = nodes[idx].first_child; c < count; c = nodes[c].next_sibling) {
        if (const uint32_t found = find_surface_eid_rec(nodes, count, c, eid, target_node_id))
            return found;
    }
    return 0u;
}
uint32_t find_surface_eid(const InlayUiNode *nodes, uint32_t count, uint32_t view_id,
                          const char *name) {
    return count ? find_surface_eid_rec(nodes, count, 0u, view_id, etch_id(name)) : 0u;
}
bool has_node_kind(const InlayUiNode *nodes, uint32_t count, InlayUiKind kind) {
    for (uint32_t i = 0; i < count; ++i)
        if (nodes[i].kind == kind)
            return true;
    return false;
}
bool str_eq(EtchStr s, const char *lit) {
    const size_t n = std::strlen(lit);
    return s.chars && s.length == n && !std::memcmp(s.chars, lit, n);
}
bool v012_expressive_surface_and_bridges(EtchContext *ctx, const InlaySurfaceDesc &desc) {
    auto *s = inlay_surface_create(ctx, &desc);
    if (!s)
        return false;

    bool ok = true;
    // Slot binding boundary validation
    ok &= check(!inlay_surface_bind_slot(nullptr, ETCH_STR_LIT("x"), slot_probe_cb, nullptr),
                "bind_slot rejects null surface");
    ok &= check(!inlay_surface_bind_slot(s, ETCH_STR_LIT(""), slot_probe_cb, nullptr),
                "bind_slot rejects empty name");
    ok &= check(!inlay_surface_bind_slot(s, ETCH_STR_LIT("x"), nullptr, nullptr),
                "bind_slot rejects null callback");

    SlotProbe probe = {};
    probe.expected_surface = s;
    ok &= check(inlay_surface_bind_slot(s, ETCH_STR_LIT("native_box"), slot_probe_cb, &probe),
                "bind_slot registers native_box");

    auto frame = etch_frame_desc_default();
    frame.viewport_w = 640;
    frame.viewport_h = 800;
    const EtchVec2 default_ptr = frame.pointer_pos;
    InlaySurfaceViewDesc view = inlay_surface_view_desc_default();
    view.id = etch_id("v012-expressive");
    view.width_px = 600;
    view.height_px = 760;
    const auto step = [&] {
        etch_begin_frame(ctx, &frame);
        inlay_surface_view(s, &view);
        etch_end_frame(ctx);
    };

    // Phase 1: Visual props, layout engine props, floating popover, and native bridges.
    static const char visual_layout_script[] = R"JS(
function render(s) {
  return ui.col({ id: 'vroot', width: 560, gap: 8, padding: 8 },
    ui.card({
      id: 'fx_card',
      width: 520,
      padding: [12, 8],
      gap: 6,
      elevation: 'lg',
      continuousCorners: true,
      radius: [12, 12, 4, 4],
      topRimAlpha: 0.2,
      backdropBlur: 12,
      backdropSaturation: 1.3,
      backdropBrightness: 1.05,
      backdropNoise: 0.01,
      hoverBg: '#1e293b',
      hoverBorderColor: '#38bdf8',
      hoverScale: 1.02,
      pressScale: 0.98,
      gradient: {
        kind: 'linear',
        oklab: true,
        dir: [1, 0],
        stops: [
          { color: '#6366f1', pos: 0.0 },
          { color: '#06b6d4', pos: 0.5 },
          { color: '#10b981', pos: 1.0 }
        ]
      }
    },
      ui.text({ text: 'Expressive Card', bold: true, wrap: false, color: '#ffffff' }),
      ui.box({
        id: 'shadow_box',
        width: 240,
        height: 28,
        bg: '#161b24',
        shadows: [
          { x: 0, y: 6, blur: 16, spread: -2, color: '#00000055' },
          { x: 0, y: 2, blur: 4, spread: 0, color: '#00000033' }
        ],
        rotate: 8,
        scale: 0.95,
        translateX: 4,
        translateY: 2
      }, ui.text({ text: 'TRANSFORM', mono: true, fontWeight: 'semibold', fontSize: 11 })),
      ui.box({ id: 'inset_well', width: 220, height: 24, bg: '#0c1017', insetShadow: true, radius: 6 })
    ),
    ui.row({ id: 'sp_row', width: 300, height: 28 },
      ui.box({ id: 'sp_l', width: 40, height: 20, bg: '#334155' }),
      ui.spacer({ id: 'sp_mid' }),
      ui.box({ id: 'sp_r', width: 40, height: 20, bg: '#475569' })
    ),
    ui.row({ id: 'jrow', width: 260, height: 28, justify: 'between' },
      ui.box({ id: 'jl', width: 30, height: 20, bg: '#334155' }),
      ui.box({ id: 'jr', width: 30, height: 20, bg: '#475569' })
    ),
    ui.box({ id: 'grid', width: 240, gridCols: 2, gap: 8 },
      ui.box({ id: 'g_span', colSpan: 2, width: -1, height: 20, bg: '#1e293b' }),
      ui.box({ id: 'g_c1', width: -1, height: 20, bg: '#334155' }),
      ui.box({ id: 'g_c2', width: -1, height: 20, bg: '#475569' })
    ),
    ui.row({ id: 'pad_row', width: 420, gap: 10 },
      ui.box({ id: 'padbox', width: 200, padding: { left: 10, right: 10, top: 6, bottom: 6 }, bg: '#182030' },
        ui.box({ id: 'padchild', width: -1, height: 20, margin: 4, bg: '#2563eb' })
      ),
      ui.box({ id: 'csspad', width: 120, padding: [5, 10, 7, 16], bg: '#182030' },
        ui.box({ id: 'csspad_c', width: -1, height: 12, bg: '#38bdf8' })
      )
    ),
    ui.row({ id: 'bridges', width: 520, gap: 10, alignItems: 'center' },
      ui.math({ id: 'mth', tex: 'x^2+y^2', fontSize: 16 }),
      ui.diagram({ id: 'dgm', source: 'flowchart LR\nA-->B', width: 240, padding: 12, alignX: 'center' }),
      ui.slot({ id: 'slt', name: 'native_box', width: 180, height: 48, props: { k: 42 } })
    ),
    ui.row({ id: 'action_row', width: 420, gap: 12, marginTop: 48, alignItems: 'center' },
      ui.button({ id: 'pbtn', text: 'Deploy', variant: 'primary', elevation: 'sm', continuousCorners: true }),
      ui.box({ id: 'pop_host', width: 180, height: 28, bg: '#1e293b' },
        ui.text({ id: 'slot_st', text: 'slot=' + (s.slotUpdated || 0) }),
        ui.popover({ id: 'pop', open: true, attachTo: 'parent', anchor: 'bottomLeft', anchorParent: 'topLeft', offsetX: 8, offsetY: -4, zIndex: 50, width: 140, padding: 8 },
          ui.text('Floating popover')
        )
      )
    )
  );
}
)JS";
    ok &= check(inlay_surface_set_script(
                    s, {visual_layout_script, uint32_t(sizeof(visual_layout_script) - 1u), true}),
                "v0.12 visual/layout/bridge script loads");
    if (inlay_surface_stats(s).has_error) {
        const auto err = inlay_surface_error(s);
        std::printf("v0.12 phase 1 error: %.*s\n", int(err.length), err.chars ? err.chars : "");
    }
    ok &= check(!inlay_surface_stats(s).has_error, "v0.12 visual/layout script has no error");

    uint32_t cnt = 0;
    const auto *nodes = inlay_surface_nodes(s, &cnt);
    ok &= check(nodes && has_node_kind(nodes, cnt, INLAY_UI_CARD), "card node present");
    ok &= check(nodes && has_node_kind(nodes, cnt, INLAY_UI_SPACER), "spacer node present");
    ok &= check(nodes && has_node_kind(nodes, cnt, INLAY_UI_POPOVER), "popover node present");
    ok &= check(nodes && has_node_kind(nodes, cnt, INLAY_UI_MATH), "math node present");
    ok &= check(nodes && has_node_kind(nodes, cnt, INLAY_UI_DIAGRAM), "diagram node present");
    ok &= check(nodes && has_node_kind(nodes, cnt, INLAY_UI_SLOT), "slot node present");

    step();
    step();
    ok &= check(etch_frame_prim_count(ctx) > 0u, "v0.12 visual/layout emits primitives");

    const auto sp_row_b = etch_element_bounds(find_surface_eid(nodes, cnt, view.id, "sp_row"));
    const auto sp_l_b = etch_element_bounds(find_surface_eid(nodes, cnt, view.id, "sp_l"));
    const auto sp_r_b = etch_element_bounds(find_surface_eid(nodes, cnt, view.id, "sp_r"));
    ok &= check(sp_row_b.w >= 299.f && sp_r_b.x >= sp_row_b.x + 240.f &&
                    sp_r_b.x > sp_l_b.x + sp_l_b.w + 150.f,
                "ui.spacer pushes sibling to right edge");

    const auto jrow_b = etch_element_bounds(find_surface_eid(nodes, cnt, view.id, "jrow"));
    const auto jr_b = etch_element_bounds(find_surface_eid(nodes, cnt, view.id, "jr"));
    ok &= check(jrow_b.w >= 259.f && jr_b.x >= jrow_b.x + 210.f,
                "justify:between pushes trailing child to right edge");

    const auto g_span_b = etch_element_bounds(find_surface_eid(nodes, cnt, view.id, "g_span"));
    const auto g_c1_b = etch_element_bounds(find_surface_eid(nodes, cnt, view.id, "g_c1"));
    const auto g_c2_b = etch_element_bounds(find_surface_eid(nodes, cnt, view.id, "g_c2"));
    ok &= check(g_span_b.w > g_c1_b.w + 40.f && g_c2_b.x > g_c1_b.x + 80.f &&
                    std::fabs(g_c1_b.y - g_c2_b.y) <= 1.5f,
                "gridCols:2 and colSpan:2 layout accurately");

    const auto padbox_b = etch_element_bounds(find_surface_eid(nodes, cnt, view.id, "padbox"));
    const auto padchild_b = etch_element_bounds(find_surface_eid(nodes, cnt, view.id, "padchild"));
    ok &= check(padchild_b.x >= padbox_b.x + 13.f && padchild_b.y >= padbox_b.y + 9.f,
                "per-side padding and margin offset child");

    const auto csspad_b = etch_element_bounds(find_surface_eid(nodes, cnt, view.id, "csspad"));
    const auto csspad_c_b = etch_element_bounds(find_surface_eid(nodes, cnt, view.id, "csspad_c"));
    ok &= check(std::fabs(csspad_c_b.x - (csspad_b.x + 16.f)) <= 0.5f &&
                    std::fabs(csspad_c_b.y - (csspad_b.y + 5.f)) <= 0.5f &&
                    std::fabs(csspad_c_b.w - 94.f) <= 0.5f &&
                    std::fabs(csspad_b.h - 24.f) <= 0.5f,
                "CSS 4-element padding [top,right,bottom,left] order applied accurately");

    const auto pop_host_b = etch_element_bounds(find_surface_eid(nodes, cnt, view.id, "pop_host"));
    const auto pop_b = etch_element_bounds(find_surface_eid(nodes, cnt, view.id, "pop"));
    ok &= check(pop_b.w >= 139.f && pop_b.h > 0.f &&
                    pop_b.y + pop_b.h <= pop_host_b.y - 3.f &&
                    std::fabs(pop_b.x - (pop_host_b.x + 8.f)) <= 1.5f,
                "floating popover camelCase bottomLeft/topLeft anchor places above parent top-left");

    const auto theme = etch_get_theme(ctx);
    const EtchColor primary_bg = theme.accent_primary.a != 0u ? theme.accent_primary : theme.accent;
    const auto pbtn_b = etch_element_bounds(find_surface_eid(nodes, cnt, view.id, "pbtn"));
    ok &= check(pbtn_b.w > 0.f && pbtn_b.h > 0.f &&
                    has_rect_prim(ctx, pbtn_b, etch_color_pack_rgba8(primary_bg), 0, true) &&
                    !has_rect_prim(ctx, pbtn_b, etch_color_pack_rgba8(theme.surface_control), 0, false) &&
                    has_text_prim(ctx, pbtn_b, etch_color_pack_rgba8(theme.on_primary)),
                "rich primary button emits primary accent color instead of surface_control");

    const auto mth_b = etch_element_bounds(find_surface_eid(nodes, cnt, view.id, "mth"));
    ok &= check(mth_b.w > 0.f && mth_b.h > 0.f, "ui.math bridge renders with positive bounds");

    const auto dgm_b = etch_element_bounds(find_surface_eid(nodes, cnt, view.id, "dgm"));
    const EtchRect dgm_inner = {dgm_b.x + 12.f, dgm_b.y + 12.f, dgm_b.w - 24.f, dgm_b.h - 24.f};
    ok &= check(dgm_b.w >= 239.f && dgm_b.h > 24.f &&
                    has_rect_prim(ctx, dgm_inner, 0, 0, false),
                "ui.diagram bridge with padding and alignX renders cleanly inside padded bounds");

    ok &= check(probe.calls >= 2u && probe.last_eid != 0u && probe.last_w > 0.f &&
                    probe.last_h > 0.f && !std::strcmp(probe.last_props, "{\"k\":42}"),
                "ui.slot invokes bound callback with exact props_json and valid size");

    const uint64_t idle_before_p1 = inlay_surface_stats(s).js_ticks;
    const uint32_t slot_calls_before = probe.calls;
    for (int i = 0; i < 8; ++i)
        step();
    ok &= check(inlay_surface_stats(s).js_ticks == idle_before_p1,
                "v0.12 visual/bridge surface runs zero JS ticks over 8 idle frames");
    ok &= check(probe.calls == slot_calls_before + 8u,
                "ui.slot continues rendering on cached idle frames");

    // Verify ui.slot callback calling inlay_surface_set_state_json safely defers update without UAF
    probe.deferred_state_json = "{\"slotUpdated\":99}";
    step();
    const auto slot_state = inlay_surface_state_json(s);
    nodes = inlay_surface_nodes(s, &cnt);
    const uint32_t slot_st_id = etch_id("slot_st");
    bool slot_text_updated = false;
    for (uint32_t i = 0; i < cnt; ++i) {
        if (nodes[i].id == slot_st_id && str_eq(nodes[i].text, "slot=99")) {
            slot_text_updated = true;
            break;
        }
    }
    step();
    ok &= check(probe.deferred_set_ok && probe.deferred_reenter_blocked &&
                    !inlay_surface_stats(s).has_error && slot_state.chars &&
                    std::strstr(slot_state.chars, "\"slotUpdated\":99") != nullptr &&
                    slot_text_updated,
                "ui.slot callback safely defers set_state_json and updates state after frame");

    // Phase 2: Interactive widgets (checkbox, tabs, combobox, tooltip, split, modal).
    ok &= check(inlay_surface_set_state_json(s, ETCH_STR_LIT("{}")), "reset state for widgets");
    static const char widgets_script[] = R"JS(
function render(s) {
  return ui.col({ id: 'wroot', width: 520, gap: 10, padding: 8 },
    ui.checkbox({ id: 'chk', label: 'Enable GPU', checked: !!s.chk, onChange: 'setChk' }),
    ui.tabs({ id: 'tabs', items: ['Alpha', 'Beta', 'Gamma'], active: s.tab || 0, onChange: 'setTab' }),
    ui.combobox({ id: 'cmb', items: ['D3D12', 'Vulkan'], selected: s.cmb || 0, onChange: 'setCmb', width: 180 }),
    ui.tooltip({ id: 'tip', text: 'Helpful tooltip' },
      ui.button({ id: 'tip_btn', text: 'Hover target' })
    ),
    ui.split({ id: 'spl', axis: 'row', ratio: 0.5, width: 280, height: 80 },
      ui.box({ id: 'spl_l', width: -1, height: -1, bg: '#1e293b' }, ui.text('Left')),
      ui.box({ id: 'spl_r', width: -1, height: -1, bg: '#334155' }, ui.text('Right'))
    ),
    ui.modal({ id: 'mdl', open: !!s.modalOpen, title: 'Confirm', onClose: 'closeModal' },
      ui.text('Modal body')
    )
  );
}
function onEvent(name, payload, s) {
  if (name === 'setChk') s.chk = payload;
  if (name === 'setTab') s.tab = payload;
  if (name === 'setCmb') s.cmb = payload;
  if (name === 'openModal') s.modalOpen = true;
  if (name === 'closeModal') s.modalOpen = false;
}
)JS";
    ok &= check(inlay_surface_set_script(
                    s, {widgets_script, uint32_t(sizeof(widgets_script) - 1u), true}),
                "v0.12 interactive widgets script loads");
    if (inlay_surface_stats(s).has_error) {
        const auto err = inlay_surface_error(s);
        std::printf("v0.12 phase 2 error: %.*s\n", int(err.length), err.chars ? err.chars : "");
    }
    ok &= check(!inlay_surface_stats(s).has_error, "v0.12 widgets script has no error");

    nodes = inlay_surface_nodes(s, &cnt);
    ok &= check(nodes && has_node_kind(nodes, cnt, INLAY_UI_CHECKBOX), "checkbox node present");
    ok &= check(nodes && has_node_kind(nodes, cnt, INLAY_UI_TABS), "tabs node present");
    ok &= check(nodes && has_node_kind(nodes, cnt, INLAY_UI_COMBOBOX), "combobox node present");
    ok &= check(nodes && has_node_kind(nodes, cnt, INLAY_UI_TOOLTIP), "tooltip node present");
    ok &= check(nodes && has_node_kind(nodes, cnt, INLAY_UI_SPLIT), "split node present");
    ok &= check(nodes && has_node_kind(nodes, cnt, INLAY_UI_MODAL), "modal node present");

    step();
    step();

    // Verify combobox and split bounds
    const auto cmb_b = etch_element_bounds(find_surface_eid(nodes, cnt, view.id, "cmb"));
    ok &= check(cmb_b.w > 0.f && cmb_b.h > 0.f, "ui.combobox renders with positive bounds");
    const auto spl_l_b = etch_element_bounds(find_surface_eid(nodes, cnt, view.id, "spl_l"));
    const auto spl_r_b = etch_element_bounds(find_surface_eid(nodes, cnt, view.id, "spl_r"));
    ok &= check(spl_l_b.w > 50.f && spl_l_b.h > 40.f && spl_r_b.w > 50.f && spl_r_b.h > 40.f &&
                    spl_r_b.x >= spl_l_b.x + spl_l_b.w,
                "ui.split renders two side-by-side panes");

    // Toggle checkbox via a11y press
    uint32_t chk_eid = find_surface_eid(nodes, cnt, view.id, "chk");
    EtchA11yNode chk_node = {};
    if (!etch_find_a11y_node(chk_eid, &chk_node, ctx, nullptr)) {
        for (uint32_t i = 0; i < etch_a11y_node_count(ctx, nullptr); ++i) {
            EtchA11yNode n = {};
            if (etch_get_a11y_node(i, &n, ctx, nullptr) && n.role == ETCH_A11Y_CHECKBOX &&
                str_eq(n.label, "Enable GPU")) {
                chk_eid = n.id;
                break;
            }
        }
    }
    ok &= check(etch_perform_a11y_action(chk_eid, ETCH_A11Y_ACT_PRESS, 0, {}, ctx, nullptr),
                "press checkbox via a11y");
    step();
    step();
    auto wjson = inlay_surface_state_json(s);
    ok &= check(wjson.chars && std::strstr(wjson.chars, "\"chk\":true") != nullptr,
                "checkbox toggle updates state to true");

    // Press/click 2nd tab ('Beta') -> updates active tab state to 1
    bool tab_pressed = false;
    EtchRect beta_bounds = {};
    for (uint32_t i = 0; i < etch_a11y_node_count(ctx, nullptr); ++i) {
        EtchA11yNode n = {};
        if (!etch_get_a11y_node(i, &n, ctx, nullptr) || !str_eq(n.label, "Beta"))
            continue;
        if (n.bounds.w > 0.f && n.bounds.h > 0.f)
            beta_bounds = n.bounds;
        if (n.role == ETCH_A11Y_TAB || n.role == ETCH_A11Y_BUTTON) {
            tab_pressed = etch_perform_a11y_action(n.id, ETCH_A11Y_ACT_PRESS, 0, {}, ctx, nullptr);
            if (tab_pressed)
                break;
        }
    }
    if (tab_pressed) {
        step();
        step();
    }
    wjson = inlay_surface_state_json(s);
    if ((!wjson.chars || std::strstr(wjson.chars, "\"tab\":1") == nullptr) &&
        beta_bounds.w > 0.f && beta_bounds.h > 0.f) {
        frame.pointer_pos = {beta_bounds.x + beta_bounds.w * 0.5f,
                             beta_bounds.y + beta_bounds.h * 0.5f};
        frame.pointer_down = true;
        step();
        frame.pointer_down = false;
        step();
        frame.pointer_pos = default_ptr;
        step();
        wjson = inlay_surface_state_json(s);
    }
    ok &= check(wjson.chars && std::strstr(wjson.chars, "\"tab\":1") != nullptr,
                "clicking 2nd tab updates active state to 1");

    // Open modal and verify it renders safely without errors
    ok &= check(inlay_surface_dispatch(s, ETCH_STR_LIT("openModal"), ETCH_STR_LIT("true")),
                "dispatch openModal");
    step();
    step();
    ok &= check(!inlay_surface_stats(s).has_error && etch_frame_prim_count(ctx) > 0u,
                "open modal renders cleanly");

    // Dismiss modal via a11y Dismiss action -> fires onClose ('closeModal')
    nodes = inlay_surface_nodes(s, &cnt);
    const uint32_t mdl_eid = find_surface_eid(nodes, cnt, view.id, "mdl");
    ok &= check(mdl_eid != 0u &&
                    etch_perform_a11y_action(mdl_eid, ETCH_A11Y_ACT_DISMISS, 0, {}, ctx, nullptr),
                "dismiss modal via a11y");
    step();
    step();
    wjson = inlay_surface_state_json(s);
    ok &= check(wjson.chars && std::strstr(wjson.chars, "\"modalOpen\":false") != nullptr,
                "modal dismissal via a11y fires onClose");

    // Re-open modal and verify Escape key dismissal also fires onClose
    ok &= check(inlay_surface_dispatch(s, ETCH_STR_LIT("openModal"), ETCH_STR_LIT("true")),
                "re-dispatch openModal for Escape check");
    step();
    step();
    frame.key_pressed_code = SDLK_ESCAPE;
    step();
    frame.key_pressed_code = 0u;
    step();
    wjson = inlay_surface_state_json(s);
    ok &= check(wjson.chars && std::strstr(wjson.chars, "\"modalOpen\":false") != nullptr,
                "modal dismissal via Escape fires onClose");

    // Verify 8 idle frames incur zero JS ticks
    const uint64_t idle_before_p2 = inlay_surface_stats(s).js_ticks;
    for (int i = 0; i < 8; ++i)
        step();
    ok &= check(inlay_surface_stats(s).js_ticks == idle_before_p2,
                "v0.12 interactive widgets run zero JS ticks over 8 idle frames");

    inlay_surface_destroy(s);
    return ok;
}
} // namespace
int main(int argc, char **argv) {
    auto config = etch_context_desc_default();
    config.headless = true;
    auto *context = etch_create_context(&config);
    if (!context)
        return 1;
    // Functional checks tolerate host scheduling; deadline checks use a separate default VM.
    auto functional_desc = inlay_surface_desc_default();
    functional_desc.max_tick_ms = 50;
    auto *surface = inlay_surface_create(context, &functional_desc);
    if (!surface) {
        etch_destroy_context(context);
        return 1;
    }
    bool ok = reactive_controls(context, functional_desc);
    ok &= styled_surface_controls(context, functional_desc);
    ok &= expanded_surface_primitives(context, functional_desc);
    ok &= i2_reactive_reconciler_upgrades(context, functional_desc);
    ok &= v012_expressive_surface_and_bridges(context, functional_desc);
    const auto script = ETCH_STR_LIT(
        "function render(s){return "
        "h('box',{axis:'column',gap:8,padding:12,bg:'var(--etch-bg-elevated)',radius:'panel',"
        "animateLayout:true},h('text',{text:'Count "
        "'+(s.count||0)}),h('button',{text:'Increment',onClick:'increment'}));} function "
        "onEvent(name,payload,s){if(name==='increment')s.count=(s.count||0)+1;if(name==='host')"
        "action('ping',payload);}");
    ok &= check(inlay_surface_set_state_json(surface, ETCH_STR_LIT("{\"count\":7}")),
                "state before source");
    ok &= check(inlay_surface_set_script(surface, script), "basic script");
    if (inlay_surface_stats(surface).has_error)
        std::printf("%.*s\n", int(inlay_surface_error(surface).length),
                    inlay_surface_error(surface).chars);
    uint32_t count = 0;
    const auto *nodes = inlay_surface_nodes(surface, &count);
    ok &= check(count == 3 && nodes[0].first_child == 1 && nodes[1].next_sibling == 2,
                "flat node links");
    ok &= check(inlay_surface_dispatch(surface, ETCH_STR_LIT("increment"), ETCH_STR_LIT("null")),
                "dispatch");
    auto state = inlay_surface_state_json(surface);
    ok &=
        check(state.length == 11 && !std::memcmp(state.chars, "{\"count\":8}", 11), "event state");
    const auto before = inlay_surface_stats(surface).js_ticks;
    auto view = inlay_surface_view_desc_default();
    view.id = etch_id("script-view");
    for (int frame = 0; frame < 32; ++frame) {
        auto f = etch_frame_desc_default();
        f.viewport_w = 640;
        f.viewport_h = 480;
        etch_begin_frame(context, &f);
        inlay_surface_view(surface, &view);
        etch_end_frame(context);
    }
    ok &= check(inlay_surface_stats(surface).js_ticks == before, "zero idle JS ticks");
    if (argc > 2)
        ok &= check(etch_render_headless_png(context, 640, 480, argv[2], etch_color_black(255)),
                    "surface screenshot");
    int calls = 0;
    ok &= inlay_surface_bind_action(surface, ETCH_STR_LIT("ping"), action, &calls);
    ok &= check(inlay_surface_dispatch(surface, ETCH_STR_LIT("host"), ETCH_STR_LIT("{}")) &&
                    calls == 1,
                "bound action");
    auto *setter = inlay_surface_create(context, nullptr);
    if (!setter)
        return 1;
    // Script-defined global setters must run under the same watchdog as events.
    ok &= check(inlay_surface_set_script(
                    setter, ETCH_STR_LIT("function render(){return h('text',{text:'setter'})} "
                                         "Object.defineProperty(globalThis,'state',{get(){return "
                                         "{}},set(){while(true){}},configurable:true});")),
                "hostile setter installed");
    auto setter_start = SDL_GetPerformanceCounter();
    ok &= check(!inlay_surface_set_state_json(setter, ETCH_STR_LIT("{}")),
                "state setter interrupted");
    ok &= check(double(SDL_GetPerformanceCounter() - setter_start) * 1000 /
                        double(SDL_GetPerformanceFrequency()) <
                    40,
                "setter deadline");
    ok &= check(inlay_surface_set_script(setter, script), "setter recovery");
    inlay_surface_destroy(setter);
    ok &= check(!inlay_surface_set_state_json(surface, ETCH_STR_LIT("{broken")),
                "invalid JSON rejected");
    ok &= inlay_surface_set_script(surface, script);
    ok &= check(
        inlay_surface_set_script(
            surface, ETCH_STR_LIT("state.n=0;Promise.resolve().then(()=>{state.n=42});function "
                                  "render(){return h('text',{text:String(state.n)})}")),
        "promise script");
    state = inlay_surface_state_json(surface);
    ok &= check(state.length && std::strstr(state.chars, "42"), "promise jobs drained");
    ok &= check(inlay_surface_set_script(
                    surface, ETCH_STR_LIT("let once=false;function "
                                          "render(){if(!once){once=true;Promise.resolve().then(()=>"
                                          "state.n=43)}return h('text',{text:String(state.n)})}")),
                "render jobs settle");
    state = inlay_surface_state_json(surface);
    ok &= check(std::strstr(state.chars, "43"), "render promise state");
    ok &= check(
        !inlay_surface_set_script(
            surface,
            ETCH_STR_LIT("Promise.resolve().then(()=>action('ping',null));throw Error('failed');")),
        "failed script queues job");
    const int old_calls = calls;
    ok &= check(inlay_surface_set_script(surface, script) && calls == old_calls,
                "failed script jobs discarded");
    ok &= check(
        !inlay_surface_set_script(
            surface,
            ETCH_STR_LIT(
                "state.x='a'.repeat(66000);function render(){return h('text',{text:'big'})}")),
        "oversized state fails update");
    ok &= check(inlay_surface_set_script(surface, script), "oversized state recovery");
    ok &= check(inlay_surface_set_script(
                    surface, ETCH_STR_LIT("function render(){return h('text',{text:'json'})}")),
                "JSON dispatch script");
    const int json_calls = calls;
    ok &= check(!inlay_surface_dispatch(surface, ETCH_STR_LIT("ping"), ETCH_STR_LIT("not JSON")) &&
                    calls == json_calls,
                "host action validates JSON");
    ok &= check(
        !inlay_surface_set_script(
            surface,
            ETCH_STR_LIT(
                "state=undefined;function render(){return h('text',{text:'invalid state'})}")),
        "undefined state rejected");
    ok &= check(inlay_surface_set_script(surface, script), "recover valid persisted state");
    ok &= check(
        !inlay_surface_set_script(
            surface, ETCH_STR_LIT("Promise.resolve().then(()=>{throw Error('oops')});function "
                                  "render(){return h('text',{text:'no'})}")),
        "unhandled Promise rejection reported");
    ok &= check(inlay_surface_set_script(
                    surface, ETCH_STR_LIT("Promise.reject(Error('caught')).catch(()=>{});function "
                                          "render(){return h('text',{text:'yes'})}")),
                "handled rejection allowed");
    auto big_desc = inlay_surface_desc_default();
    big_desc.max_nodes = 65536;
    big_desc.max_text_bytes = 1048576;
    auto *big = inlay_surface_create(context, &big_desc);
    ok &= check(big != nullptr, "large tree surface");
    if (big) {
        const auto started = SDL_GetPerformanceCounter();
        ok &= check(
            !inlay_surface_set_script(
                big, ETCH_STR_LIT(
                         "const t=h('box',{},...new "
                         "Array(60000).fill(h('text',{text:'x'})));function render(){return t}")),
            "native conversion deadline");
        ok &= check(double(SDL_GetPerformanceCounter() - started) * 1000 /
                            double(SDL_GetPerformanceFrequency()) <
                        native_conversion_limit_ms,
                    "large tree bounded");
        inlay_surface_destroy(big);
    }
    const int failed_timer_calls = calls;
    ok &= check(
        !inlay_surface_set_script(
            surface, ETCH_STR_LIT("setTimeout(()=>action('ping',null),0);throw Error('failed')")),
        "failed timer script rejected");
    auto failed_frame = etch_frame_desc_default();
    failed_frame.viewport_w = 640;
    failed_frame.viewport_h = 480;
    etch_begin_frame(context, &failed_frame);
    inlay_surface_view(surface, &view);
    etch_end_frame(context);
    ok &= check(calls == failed_timer_calls, "failed script timers discarded");
    const double frequency = double(SDL_GetPerformanceFrequency());
    auto *watchdog = inlay_surface_create(context, nullptr);
    if (!watchdog)
        return 1;
    auto start = SDL_GetPerformanceCounter();
    ok &= check(!inlay_surface_set_script(watchdog, ETCH_STR_LIT("while(true){}")),
                "loop interrupted");
    const double loop_ms = double(SDL_GetPerformanceCounter() - start) * 1000 / frequency;
    ok &= check(loop_ms < 40 && inlay_surface_stats(watchdog).has_error, "loop deadline");
    if (argc > 3) {
        auto diagnostic = etch_frame_desc_default();
        diagnostic.viewport_w = 640;
        diagnostic.viewport_h = 480;
        etch_begin_frame(context, &diagnostic);
        inlay_surface_view(watchdog, &view);
        etch_end_frame(context);
        ok &= check(etch_render_headless_png(context, 640, 480, argv[3], etch_color_black(255)),
                    "diagnostic screenshot");
    }
    std::printf("Infinite loop interrupted in %.3fms\n", loop_ms);
    ok &= check(inlay_surface_set_script(watchdog, script), "recover after timeout");
    inlay_surface_destroy(watchdog);
    ok &= check(!inlay_surface_set_script(surface, ETCH_STR_LIT("new ArrayBuffer(64*1024*1024);")),
                "OOM contained");
    ok &= check(inlay_surface_set_script(surface, script), "recover after OOM");
    ok &= check(!inlay_surface_set_script(surface, ETCH_STR_LIT("function f(){return f()}f()")),
                "stack bounded");
    ok &= check(inlay_surface_set_script(
                    surface,
                    ETCH_STR_LIT(
                        "if(typeof std!=='undefined'||typeof os!=='undefined'||typeof "
                        "fetch!=='undefined'||typeof Atomics!=='undefined')throw "
                        "Error('ambient');function render(){return h('text',{text:'sandbox'});} ")),
                "no ambient authority");
    ok &= check(inlay_surface_set_state_json(surface, ETCH_STR_LIT("{}")), "reset timer state");
    ok &= check(inlay_surface_set_script(
                    surface, ETCH_STR_LIT("setTimeout(()=>{state.count=42},0);function "
                                          "render(){return h('text',{text:String(state.count)})}")),
                "timer script");
    auto frame = etch_frame_desc_default();
    frame.viewport_w = 640;
    frame.viewport_h = 480;
    etch_begin_frame(context, &frame);
    inlay_surface_view(surface, &view);
    etch_end_frame(context);
    state = inlay_surface_state_json(surface);
    ok &=
        check(state.length == 12 && !std::memcmp(state.chars, "{\"count\":42}", 12), "timer state");
    if (argc > 1) {
        ok &= write_file(argv[1], "function render(s){return h('text',{text:'First '+s.count})}");
        char unicode_path[2048] = {};
        std::snprintf(unicode_path, sizeof(unicode_path), "%s-测试.js", argv[1]);
        // SDL uses UTF-8 paths on every platform, matching the public HMR API.
        ok &=
            check(SDL_SaveFile(unicode_path, "function render(){return h('text',{text:'unicode'})}",
                               std::strlen("function render(){return h('text',{text:'unicode'})}")),
                  "write UTF8 HMR path");
        auto *unicode_surface = inlay_surface_create(context, nullptr);
        ok &= check(unicode_surface && inlay_surface_enable_hmr(unicode_surface, unicode_path),
                    "UTF8 HMR path");
        inlay_surface_destroy(unicode_surface);
        auto *fresh_hmr = inlay_surface_create(context, nullptr);
        ok &= check(fresh_hmr && inlay_surface_enable_hmr(fresh_hmr, argv[1]),
                    "first-use HMR bootstrap");
        inlay_surface_destroy(fresh_hmr);
        ok &= check(inlay_surface_enable_hmr(surface, argv[1]), "initial HMR");
        char moved[2048] = {};
        std::snprintf(moved, sizeof(moved), "%s.moved", argv[1]);
        ok &= check(std::rename(argv[1], moved) == 0, "hide HMR file");
        ok &= check(!inlay_surface_poll_hmr(surface), "missing HMR error");
        ok &= check(std::rename(moved, argv[1]) == 0, "restore HMR file");
        ok &= check(inlay_surface_poll_hmr(surface) && !inlay_surface_stats(surface).has_error,
                    "unchanged restored HMR recovers");
        const auto ticks = inlay_surface_stats(surface).js_ticks;
        ok &= inlay_surface_poll_hmr(surface);
        ok &= check(inlay_surface_stats(surface).js_ticks == ticks, "unchanged HMR idle");
        ok &= write_file(
            argv[1], "function render(s){return h('text',{text:'Reloaded preserved '+s.count})}");
        ok &= check(inlay_surface_poll_hmr(surface), "changed HMR");
        state = inlay_surface_state_json(surface);
        ok &= check(state.length == 12 && !std::memcmp(state.chars, "{\"count\":42}", 12),
                    "HMR state preserved");
        // Measure a median so a scheduler preemption does not masquerade as VM work.
        double samples[9] = {}, totals[9] = {};
        for (int i = 0; i < 9; ++i) {
            ok &= write_file(
                argv[1],
                i % 2 ? "function render(s){return h('text',{text:'Warm '+s.count})}"
                      : "function render(s){return h('text',{text:'Warm reload '+s.count})}");
            ok &= inlay_surface_poll_hmr(surface);
            samples[i] = inlay_surface_stats(surface).last_reload_ms;
            totals[i] = inlay_surface_stats(surface).last_reload_total_ms;
        }
        for (int i = 1; i < 9; ++i)
            for (int j = i; j > 0 && samples[j] < samples[j - 1]; --j) {
                const double tmp = samples[j];
                samples[j] = samples[j - 1];
                samples[j - 1] = tmp;
            }
        for (int i = 1; i < 9; ++i)
            for (int j = i; j > 0 && totals[j] < totals[j - 1]; --j) {
                const double tmp = totals[j];
                totals[j] = totals[j - 1];
                totals[j - 1] = tmp;
            }
        std::printf("HMR engine median %.3fms, filesystem-inclusive median %.3fms\n", samples[4],
                    totals[4]);
        ok &= check(samples[4] < 2, "HMR median below 2ms");
    }
    // Node labels are copied to frame storage before host updates mutate cached UI.
    etch_begin_frame(context, &frame);
    inlay_surface_view(surface, &view);
    ok &= inlay_surface_set_state_json(surface, ETCH_STR_LIT("{\"count\":100}"));
    etch_end_frame(context);
    if (argc > 1) {
        auto *missing = inlay_surface_create(context, nullptr);
        char missing_path[2048] = {};
        std::snprintf(missing_path, sizeof(missing_path), "%s.absent", argv[1]);
        ok &= check(missing && !inlay_surface_enable_hmr(missing, missing_path),
                    "missing HMR file error");
        for (int i = 0; i < 32; ++i) {
            etch_begin_frame(context, &frame);
            inlay_surface_view(missing, &view);
            etch_end_frame(context);
        }
        ok &= check(!etch_needs_redraw(context), "persistent HMR error settles idle");
        inlay_surface_destroy(missing);
    }
    inlay_surface_destroy(surface);
    etch_destroy_context(context);
    std::puts(ok ? "Sandbox reactive/HMR PASS" : "Sandbox reactive/HMR FAIL");
    return ok ? 0 : 1;
}
