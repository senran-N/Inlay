#include <inlay/inlay_c.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_iostream.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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

uint32_t find_rect_fill_rgba(EtchContext *ctx, EtchRect bounds) {
    for (uint32_t i = 0; i < etch_frame_prim_count(ctx); ++i) {
        EtchPrim prim = {};
        if (!etch_get_frame_prim(ctx, i, &prim))
            continue;
        if (std::fabs(prim.bounds.x - bounds.x) <= 1.5f &&
            std::fabs(prim.bounds.y - bounds.y) <= 1.5f &&
            std::fabs(prim.bounds.z - bounds.w) <= 1.5f &&
            std::fabs(prim.bounds.w - bounds.h) <= 1.5f &&
            prim.style.y != 0u) {
            return prim.style.y;
        }
    }
    return 0u;
}

const InlayUiNode *find_node_by_id(const InlayUiNode *nodes, uint32_t count, const char *name) {
    const uint32_t target = etch_id(name);
    for (uint32_t i = 0; i < count; ++i) {
        if (nodes[i].id == target)
            return &nodes[i];
    }
    return nullptr;
}

bool v013_jsx_transpiler(EtchContext *ctx, const InlaySurfaceDesc &desc) {
    auto *s = inlay_surface_create(ctx, &desc);
    if (!s)
        return false;

    bool ok = true;
    auto frame = etch_frame_desc_default();
    frame.viewport_w = 500;
    frame.viewport_h = 400;
    InlaySurfaceViewDesc view = inlay_surface_view_desc_default();
    view.id = etch_id("v013-jsx");
    view.width_px = 460;
    view.height_px = 360;
    const auto step = [&] {
        etch_begin_frame(ctx, &frame);
        inlay_surface_view(s, &view);
        etch_end_frame(ctx);
    };

    // 1. Intrinsic tags, PascalCase & function components, fragments, self-closing <Spacer />,
    //    spread override order, JSX inside template literals, comparisons & /regex/g, and Math.max + <Math>.
    static const char jsx_full_script[] = R"JS(
function CustomPill({ id, label, children }) {
  return (
    <row id={id} gap={4}>
      <Badge variant="accent">{label}</Badge>
      {children}
    </row>
  );
}

function render(s) {
  const a = 2, b = 5, c = 9, d = 4;
  const cmpOk = (a < b && c > d);
  const reOk = /<div\s*>/g.test('<div>') && !/<span\s*>/g.test('<div>');
  const mathSum = Math.max(1, 2) + Math.min(3, 4);
  const spreadOverride = { width: 180, height: 24 };
  const tmplTag = `kind:${(<Badge>ok</Badge>).tag}`;
  return (
    <Col id="jroot" width={420} gap={6}>
      <card id="lower_card" padding={6}>
        <>
          <Text id="cmp_txt">{cmpOk && reOk ? 'CMP_RE_OK' : 'BAD'}</Text>
          <Text id="mmax_txt">{String(mathSum)}</Text>
          <Text id="tmpl_txt">{tmplTag}</Text>
        </>
      </card>
      <row id="sp_row" width={260} height={24}>
        <Box id="sp1" width={100} {...spreadOverride} bg="#224466" />
        <Spacer id="self_sp" />
      </row>
      <Box id="sp2" {...spreadOverride} width={120} bg="#335577" />
      <CustomPill id="cpill" label="Fn">
        <Text id="cpill_child">Child</Text>
      </CustomPill>
      <Math id="mtex" tex="x^2 + y^2" fontSize={16} />
    </Col>
  );
}
)JS";
    ok &= check(inlay_surface_set_script(
                    s, {jsx_full_script, uint32_t(sizeof(jsx_full_script) - 1u), true}),
                "v0.13 full JSX script loads");
    if (inlay_surface_stats(s).has_error) {
        const auto err = inlay_surface_error(s);
        std::printf("v0.13 JSX error: %.*s\n", int(err.length), err.chars ? err.chars : "");
    }
    ok &= check(!inlay_surface_stats(s).has_error, "v0.13 full JSX script has no error");

    uint32_t cnt = 0;
    const auto *nodes = inlay_surface_nodes(s, &cnt);
    const auto *cmp_n = find_node_by_id(nodes, cnt, "cmp_txt");
    const auto *mmax_n = find_node_by_id(nodes, cnt, "mmax_txt");
    const auto *tmpl_n = find_node_by_id(nodes, cnt, "tmpl_txt");
    const auto *cpill_c = find_node_by_id(nodes, cnt, "cpill_child");
    const auto *mtex_n = find_node_by_id(nodes, cnt, "mtex");
    ok &= check(cmp_n && str_eq(cmp_n->text, "CMP_RE_OK"),
                "JSX preserves comparisons (a < b && c > d) and /regex/g");
    ok &= check(mmax_n && str_eq(mmax_n->text, "5") && mtex_n && mtex_n->kind == INLAY_UI_MATH,
                "Math.max/min works alongside <Math> tag");
    ok &= check(tmpl_n && str_eq(tmpl_n->text, "kind:badge"),
                "nested JSX inside template literal transpiles cleanly");
    ok &= check(cpill_c && str_eq(cpill_c->text, "Child"),
                "custom JSX function component forwards children");
    ok &= check(has_node_kind(nodes, cnt, INLAY_UI_CARD) &&
                    has_node_kind(nodes, cnt, INLAY_UI_SPACER) &&
                    has_node_kind(nodes, cnt, INLAY_UI_BADGE),
                "lowercase <card>, self-closing <Spacer />, and <Badge> build native nodes");

    step();
    step();
    const auto sp1_b = etch_element_bounds(find_surface_eid(nodes, cnt, view.id, "sp1"));
    const auto sp2_b = etch_element_bounds(find_surface_eid(nodes, cnt, view.id, "sp2"));
    ok &= check(std::fabs(sp1_b.w - 180.f) <= 0.5f && std::fabs(sp2_b.w - 120.f) <= 0.5f,
                "JSX spread {...p} respects left-to-right override order");

    // 2. 1-to-1 line number preservation in runtime error messages after multiline JSX.
    static const char line_err_script[] =
        "function render() {\n"
        "  const hdr = (\n"
        "    <Col>\n"
        "      <Text>Line 4</Text>\n"
        "    </Col>\n"
        "  );\n"
        "  throw new Error('line7_boom');\n"
        "  return hdr;\n"
        "}\n";
    ok &= check(!inlay_surface_set_script(
                    s, {line_err_script, uint32_t(sizeof(line_err_script) - 1u), true}),
                "runtime error after multiline JSX fails");
    const auto line_err = inlay_surface_error(s);
    ok &= check(line_err.chars && std::strstr(line_err.chars, "line7_boom") != nullptr &&
                    std::strstr(line_err.chars, ":7:") != nullptr,
                "JSX transpiler preserves 1-to-1 line numbers in runtime error stack");

    // 3. Fast-fail JSXSyntaxError on mismatched tags with accurate line number.
    static const char mismatch_script[] =
        "function render() {\n"
        "  return (\n"
        "    <Col>\n"
        "      <Row>\n"
        "        <Text>hi</Text>\n"
        "      </Card>\n"
        "    </Col>\n"
        "  );\n"
        "}\n";
    ok &= check(!inlay_surface_set_script(
                    s, {mismatch_script, uint32_t(sizeof(mismatch_script) - 1u), true}),
                "mismatched JSX tags rejected");
    const auto mm_err = inlay_surface_error(s);
    ok &= check(mm_err.chars && std::strstr(mm_err.chars, "JSXSyntaxError") != nullptr &&
                    std::strstr(mm_err.chars, ":6:") != nullptr,
                "mismatched JSX tag reports JSXSyntaxError with exact line number");

    // 4. disable_jsx = true bypasses transpiler cleanly.
    InlaySurfaceDesc no_jsx_desc = desc;
    no_jsx_desc.disable_jsx = true;
    auto *no_jsx = inlay_surface_create(ctx, &no_jsx_desc);
    ok &= check(no_jsx != nullptr, "create surface with disable_jsx=true");
    if (no_jsx) {
        ok &= check(inlay_surface_set_script(
                        no_jsx, ETCH_STR_LIT("function render(){return h('text',{text:'plain'});}")),
                    "disable_jsx=true runs plain JS");
        ok &= check(!inlay_surface_set_script(
                        no_jsx, ETCH_STR_LIT("function render(){return <Text>nope</Text>;}")),
                    "disable_jsx=true rejects raw JSX");
        const auto nj_err = inlay_surface_error(no_jsx);
        ok &= check(nj_err.chars && std::strstr(nj_err.chars, "JSXSyntaxError") == nullptr,
                    "disable_jsx=true bypasses JSX transpiler (QuickJS SyntaxError)");
        inlay_surface_destroy(no_jsx);
    }

    inlay_surface_destroy(s);
    return ok;
}

bool v013_cls_colors_and_themes(EtchContext *ctx, const InlaySurfaceDesc &desc) {
    const EtchTheme dark_t = etch_theme_dark();
    const EtchTheme light_t = etch_theme_light();
    etch_set_theme(&dark_t, ctx);

    auto *s = inlay_surface_create(ctx, &desc);
    if (!s)
        return false;

    bool ok = true;
    static const char cls_script[] = R"JS(
function render() {
  return (
    <Col id="croot" cls="w-full gap-2 p-2">
      <Box id="util_row" cls="flex-row gap-3 p-4 rounded-xl bg-surface shadow-md hover:bg-hover transition duration-150 ease-out" width={260}>
        <Box id="u1" cls="w-10 h-8 bg-accent/15 rounded-md" />
        <Box id="u2" cls="w-10 h-8 bg-[#334455] rounded-md" />
      </Box>
      <Box id="theme_box" cls="w-16 h-6 bg-surface rounded-md" />
      <Box id="oklch_box" width={80} height={24} bg="oklch(0.7 0.15 240)" />
      <Box id="rgb_box" width={80} height={24} bg="rgb(255 0 0 / 50%)" />
      <Box id="override_box" cls="w-20 h-10 bg-[#112233]" width={150} bg="#445566" />
    </Col>
  );
}
)JS";
    ok &= check(inlay_surface_set_script(
                    s, {cls_script, uint32_t(sizeof(cls_script) - 1u), true}),
                "v0.13 cls & color script loads");
    ok &= check(!inlay_surface_stats(s).has_error, "v0.13 cls & color script has no error");

    uint32_t cnt = 0;
    const auto *nodes = inlay_surface_nodes(s, &cnt);
    InlaySurfaceViewDesc view = inlay_surface_view_desc_default();
    view.id = etch_id("v013-cls");
    view.width_px = 400;
    view.height_px = 300;
    auto frame = etch_frame_desc_default();
    frame.viewport_w = 400;
    frame.viewport_h = 300;
    frame.dt_seconds = 1.f / 60.f;
    const auto step = [&] {
        etch_begin_frame(ctx, &frame);
        inlay_surface_view(s, &view);
        etch_end_frame(ctx);
    };

    step();
    step();

    const auto row_b = etch_element_bounds(find_surface_eid(nodes, cnt, view.id, "util_row"));
    const auto theme_b = etch_element_bounds(find_surface_eid(nodes, cnt, view.id, "theme_box"));
    const auto u1_b = etch_element_bounds(find_surface_eid(nodes, cnt, view.id, "u1"));
    const auto u2_b = etch_element_bounds(find_surface_eid(nodes, cnt, view.id, "u2"));
    const auto oklch_b = etch_element_bounds(find_surface_eid(nodes, cnt, view.id, "oklch_box"));
    const auto rgb_b = etch_element_bounds(find_surface_eid(nodes, cnt, view.id, "rgb_box"));
    const auto ov_b = etch_element_bounds(find_surface_eid(nodes, cnt, view.id, "override_box"));

    ok &= check(std::fabs(u1_b.x - (row_b.x + 16.f)) <= 1.f &&
                    std::fabs(u1_b.y - (row_b.y + 16.f)) <= 1.f &&
                    std::fabs(u2_b.x - (u1_b.x + u1_b.w + 12.f)) <= 1.f &&
                    std::fabs(u1_b.y - u2_b.y) <= 1.f,
                "cls flex-row gap-3 p-4 layout metrics accurate");

    const uint32_t dark_surf_rgba = etch_color_pack_rgba8(dark_t.bg_surface);
    ok &= check(has_rect_prim(ctx, row_b, dark_surf_rgba, 0, true) &&
                    has_rect_prim(ctx, theme_b, dark_surf_rgba, 0, true),
                "cls bg-surface + rounded-xl resolves live Dark theme token");

    const uint8_t a15 = uint8_t((uint32_t(dark_t.accent.a) * 38u + 127u) / 255u);
    const EtchColor expected_acc15 = etch_rgba8(dark_t.accent.r, dark_t.accent.g, dark_t.accent.b, a15);
    ok &= check(has_rect_prim(ctx, u1_b, etch_color_pack_rgba8(expected_acc15), 0, true),
                "cls bg-accent/15 scales token alpha accurately");

    const EtchColor expected_oklch = etch_color_oklch(0.7f, 0.15f, 240.0f, 1.0f);
    ok &= check(has_rect_prim(ctx, oklch_b, etch_color_pack_rgba8(expected_oklch), 0, false),
                "oklch(0.7 0.15 240) resolves via Etch Oklch");

    const EtchColor expected_rgb50 = etch_rgba8(255u, 0u, 0u, 128u);
    ok &= check(has_rect_prim(ctx, rgb_b, etch_color_pack_rgba8(expected_rgb50), 0, false),
                "rgb(255 0 0 / 50%) resolves modern CSS RGB alpha");

    ok &= check(std::fabs(ov_b.w - 150.f) <= 0.5f && std::fabs(ov_b.h - 40.f) <= 0.5f &&
                    has_rect_prim(ctx, ov_b, etch_color_pack_rgba8(etch_rgba8(0x44, 0x55, 0x66, 255)), 0, false),
                "explicit props override cls width and bg while preserving cls height");

    // Live theme switch Dark -> Light with ZERO JS ticks!
    const uint64_t ticks_before_theme = inlay_surface_stats(s).js_ticks;
    etch_set_theme(&light_t, ctx);
    step();
    const uint32_t light_surf_rgba = etch_color_pack_rgba8(light_t.bg_surface);
    ok &= check(light_surf_rgba != dark_surf_rgba &&
                    has_rect_prim(ctx, theme_b, light_surf_rgba, 0, true),
                "bg-surface updates instantaneously on non-transitioned box in Light theme");
    for (int i = 0; i < 30; ++i)
        step();
    ok &= check(inlay_surface_stats(s).js_ticks == ticks_before_theme,
                "live Dark -> Light theme switch runs zero JS ticks");
    ok &= check(has_rect_prim(ctx, row_b, light_surf_rgba, 0, true),
                "transitioned bg-surface settles to Light theme without JS re-evaluation");

    etch_set_theme(&dark_t, ctx);
    inlay_surface_destroy(s);
    return ok;
}

bool v013_transitions_zero_idle_ticks(EtchContext *ctx, const InlaySurfaceDesc &desc) {
    auto *s = inlay_surface_create(ctx, &desc);
    if (!s)
        return false;

    bool ok = true;
    static const char trans_script[] = R"JS(
function render(s) {
  return (
    <Col id="tr_root" width={300} gap={12} padding={12}>
      <Box id="tbox" cls="w-32 h-12 bg-[#203040] hover:bg-[#cc4422] transition duration-150 ease-out" />
      <Box id="tfbox" width={64} height={32} bg="#336699" scale={s.big ? 1.25 : 1.0} transition="transform 150ms ease-out" />
    </Col>
  );
}
function onEvent(name, v, s) {
  if (name === 'setBig') s.big = !!v;
}
)JS";
    ok &= check(inlay_surface_set_script(
                    s, {trans_script, uint32_t(sizeof(trans_script) - 1u), true}),
                "v0.13 transition script loads");
    ok &= check(!inlay_surface_stats(s).has_error, "v0.13 transition script has no error");

    uint32_t cnt = 0;
    const auto *nodes = inlay_surface_nodes(s, &cnt);
    InlaySurfaceViewDesc view = inlay_surface_view_desc_default();
    view.id = etch_id("v013-trans");
    view.width_px = 340;
    view.height_px = 200;
    auto frame = etch_frame_desc_default();
    frame.viewport_w = 340;
    frame.viewport_h = 200;
    frame.dt_seconds = 1.f / 60.f;
    const EtchVec2 default_ptr = frame.pointer_pos;
    const auto step = [&] {
        etch_begin_frame(ctx, &frame);
        inlay_surface_view(s, &view);
        etch_end_frame(ctx);
    };

    for (int i = 0; i < 6; ++i)
        step();

    const auto tbox_b = etch_element_bounds(find_surface_eid(nodes, cnt, view.id, "tbox"));
    const uint32_t start_col = etch_color_pack_rgba8(etch_rgba8(0x20, 0x30, 0x40, 255));
    const uint32_t target_col = etch_color_pack_rgba8(etch_rgba8(0xcc, 0x44, 0x22, 255));
    ok &= check(find_rect_fill_rgba(ctx, tbox_b) == start_col, "transition box starts at base color");

    // Hover over tbox -> smooth tween over 150ms with 0 JS ticks!
    const uint64_t ticks_before_hover = inlay_surface_stats(s).js_ticks;
    frame.pointer_pos = {tbox_b.x + tbox_b.w * 0.5f, tbox_b.y + tbox_b.h * 0.5f};
    bool saw_intermediate = false;
    for (int i = 0; i < 6; ++i) {
        step();
        const uint32_t cur = find_rect_fill_rgba(ctx, tbox_b);
        if (cur != 0u && cur != start_col && cur != target_col)
            saw_intermediate = true;
    }
    ok &= check(saw_intermediate, "hover transition interpolates intermediate Oklab color frames");

    for (int i = 0; i < 36; ++i)
        step();
    ok &= check(find_rect_fill_rgba(ctx, tbox_b) == target_col,
                "hover transition reaches exact target color");
    ok &= check(!etch_needs_redraw(ctx), "settled hover transition reports needs_redraw == false");
    ok &= check(inlay_surface_stats(s).js_ticks == ticks_before_hover,
                "hover transition executes 0 JS ticks during animation and idle");
    frame.pointer_pos = default_ptr;
    for (int i = 0; i < 36; ++i)
        step();

    // Transform state change and smooth return to identity (does not snap!).
    ok &= check(inlay_surface_dispatch(s, ETCH_STR_LIT("setBig"), ETCH_STR_LIT("true")),
                "dispatch scale up");
    for (int i = 0; i < 36; ++i)
        step();
    ok &= check(!etch_needs_redraw(ctx), "scaled-up transform settles cleanly");

    const uint64_t ticks_before_ret = inlay_surface_stats(s).js_ticks;
    ok &= check(inlay_surface_dispatch(s, ETCH_STR_LIT("setBig"), ETCH_STR_LIT("false")),
                "dispatch return to identity transform");
    step();
    step();
    ok &= check(etch_needs_redraw(ctx),
                "returning transform to identity animates smoothly instead of snapping");
    for (int i = 0; i < 36; ++i)
        step();
    ok &= check(!etch_needs_redraw(ctx), "identity return transition settles to needs_redraw == false");
    ok &= check(inlay_surface_stats(s).js_ticks == ticks_before_ret + 1u,
                "transform return transition executes 0 extra JS ticks during animation and idle");

    inlay_surface_destroy(s);
    return ok;
}

bool v013_bugfix_regressions(EtchContext *ctx, const InlaySurfaceDesc &desc) {
    auto *s = inlay_surface_create(ctx, &desc);
    if (!s)
        return false;

    bool ok = true;
    static const char reg_script[] = R"JS(
function render(s) {
  const deep = [[[ui.text({ id: 'deep3', text: 'Deep3' })]]];
  return (
    <Col id="rroot" width={400} gap={8} padding={8}>
      <Row gap={8}>
        <Box id="neg_sc" width={40} height={24} bg="#4488cc" scale={-2} opacity={0.6} translate={[4, 2]} />
        <Box id="zero_sc" width={40} height={24} bg="#8844cc" scale={0} opacity={0.7} />
      </Row>
      <Row gap={8}>
        <Card id="click_card" width={100} height={34} padding={6} onClick={() => { state.cardClicks = (state.cardClicks || 0) + 1; }}>
          <Text>Card</Text>
        </Card>
        <Box id="click_box" width={100} height={34} padding={6} bg="#223344" onClick={() => { state.boxClicks = (state.boxClicks || 0) + 1; }}>
          <Text>Box</Text>
        </Box>
      </Row>
      <Row gap={8}>
        <Button id="prim_btn" variant="primary" bg="#10b981" color="#111111" width={90} height={32}>Prim</Button>
        <Button id="dang_btn" variant="danger" bg="#991b1b" color="#fef08a" width={90} height={32}>Dang</Button>
        <Button id="dis_btn" variant="primary" bg="#ff00ff" elevation="sm" disabled width={90} height={32} onClick={() => { state.disClicks = (state.disClicks || 0) + 1; }}>Off</Button>
      </Row>
      <Button id="mbtn" onClick={() => { state.mbtnClicks = (state.mbtnClicks || 0) + 1; }}>
        <Badge id="mbdg" variant="danger">3</Badge> Alerts
      </Button>
      {deep}
      {ui.box(null, s.ins ? ui.text('Ins') : null, ui.text({ id: 'np_a', text: 'NullPropsA' }), ui.text('NullPropsB'))}
    </Col>
  );
}
function onEvent(name, v, s) {
  if (name === 'ins') s.ins = !!v;
}
)JS";
    ok &= check(inlay_surface_set_script(
                    s, {reg_script, uint32_t(sizeof(reg_script) - 1u), true}),
                "v0.13 bugfix regression script loads");
    if (inlay_surface_stats(s).has_error) {
        const auto err = inlay_surface_error(s);
        std::printf("v0.13 bugfix error: %.*s\n", int(err.length), err.chars ? err.chars : "");
    }
    ok &= check(!inlay_surface_stats(s).has_error, "v0.13 bugfix script has no error");

    uint32_t cnt = 0;
    const auto *nodes = inlay_surface_nodes(s, &cnt);
    const auto *deep3_n = find_node_by_id(nodes, cnt, "deep3");
    const auto *npa_n = find_node_by_id(nodes, cnt, "np_a");
    ok &= check(deep3_n && str_eq(deep3_n->text, "Deep3"),
                "depth-3 nested child arrays flatten cleanly");
    uint32_t npb_id_before = 0u;
    for (uint32_t i = 0; i < cnt; ++i) {
        if (str_eq(nodes[i].text, "NullPropsB"))
            npb_id_before = nodes[i].id;
    }
    ok &= check(npa_n != nullptr && npb_id_before != 0u,
                "ui.box(null, child...) builds children cleanly");

    InlaySurfaceViewDesc view = inlay_surface_view_desc_default();
    view.id = etch_id("v013-bugfix");
    view.width_px = 440;
    view.height_px = 380;
    auto frame = etch_frame_desc_default();
    frame.viewport_w = 440;
    frame.viewport_h = 380;
    const EtchVec2 default_ptr = frame.pointer_pos;
    const auto step = [&] {
        etch_begin_frame(ctx, &frame);
        inlay_surface_view(s, &view);
        etch_end_frame(ctx);
    };
    const auto click_at = [&](EtchRect r) {
        frame.pointer_pos = {r.x + r.w * 0.5f, r.y + r.h * 0.5f};
        frame.pointer_down = true;
        step();
        frame.pointer_down = false;
        step();
        frame.pointer_pos = default_ptr;
        step();
    };

    step();
    step();

    // Bug 1: negative & zero scale boxes render without wiping layer/opacity.
    const auto neg_b = etch_element_bounds(find_surface_eid(nodes, cnt, view.id, "neg_sc"));
    const auto zero_b = etch_element_bounds(find_surface_eid(nodes, cnt, view.id, "zero_sc"));
    ok &= check(neg_b.w > 0.f && zero_b.w > 0.f && etch_frame_prim_count(ctx) > 0u,
                "negative and zero scale clamp cleanly without breaking layer/opacity");

    // Bug 3: primary/danger custom bg & color + disabled rich button.
    const auto theme = etch_get_theme(ctx);
    const auto prim_b = etch_element_bounds(find_surface_eid(nodes, cnt, view.id, "prim_btn"));
    const auto dang_b = etch_element_bounds(find_surface_eid(nodes, cnt, view.id, "dang_btn"));
    const auto dis_b = etch_element_bounds(find_surface_eid(nodes, cnt, view.id, "dis_btn"));
    ok &= check(has_rect_prim(ctx, prim_b, etch_color_pack_rgba8(etch_rgba8(0x10, 0xb9, 0x81, 255)), 0, false) &&
                    has_text_prim(ctx, prim_b, etch_color_pack_rgba8(etch_rgba8(0x11, 0x11, 0x11, 255))),
                "primary button honors custom bg and text color");
    ok &= check(has_rect_prim(ctx, dang_b, etch_color_pack_rgba8(etch_rgba8(0x99, 0x1b, 0x1b, 255)), 0, false) &&
                    has_text_prim(ctx, dang_b, etch_color_pack_rgba8(etch_rgba8(0xfe, 0xf0, 0x8a, 255))),
                "danger button honors custom bg and text color");
    ok &= check(has_rect_prim(ctx, dis_b, etch_color_pack_rgba8(theme.surface_disabled), 0, false) &&
                    !has_rect_prim(ctx, dis_b, etch_color_pack_rgba8(etch_rgba8(0xff, 0x00, 0xff, 255)), 0, false),
                "disabled rich button uses surface_disabled instead of custom bg");

    // Bug 4: multi-child button renders child <Badge> inside button bounds.
    const auto mbtn_b = etch_element_bounds(find_surface_eid(nodes, cnt, view.id, "mbtn"));
    const auto mbdg_b = etch_element_bounds(find_surface_eid(nodes, cnt, view.id, "mbdg"));
    ok &= check(mbtn_b.w > mbdg_b.w && mbdg_b.w > 10.f && mbdg_b.h > 10.f &&
                    mbdg_b.x >= mbtn_b.x && mbdg_b.x + mbdg_b.w <= mbtn_b.x + mbtn_b.w + 1.f,
                "multi-child <Button><Badge>3</Badge> Alerts</Button> lays out badge child");

    // Bug 2 & 3 & 4 clicks: card, box, multi-child button fire; disabled button does NOT.
    const auto card_b = etch_element_bounds(find_surface_eid(nodes, cnt, view.id, "click_card"));
    const auto box_b = etch_element_bounds(find_surface_eid(nodes, cnt, view.id, "click_box"));
    click_at(card_b);
    click_at(box_b);
    click_at(dis_b);
    click_at(mbtn_b);
    const auto st = inlay_surface_state_json(s);
    ok &= check(st.chars && std::strstr(st.chars, "\"cardClicks\":1") != nullptr &&
                    std::strstr(st.chars, "\"boxClicks\":1") != nullptr &&
                    std::strstr(st.chars, "\"mbtnClicks\":1") != nullptr &&
                    std::strstr(st.chars, "disClicks") == nullptr,
                "ui.card, ui.box, multi-child button dispatch onClick while disabled button ignores clicks");

    // Bug 5: sibling ID stability in ui.box(null, ...) across conditional mount.
    ok &= check(inlay_surface_dispatch(s, ETCH_STR_LIT("ins"), ETCH_STR_LIT("true")),
                "insert conditional sibling in ui.box(null, ...)");
    nodes = inlay_surface_nodes(s, &cnt);
    uint32_t npb_id_after = 0u;
    for (uint32_t i = 0; i < cnt; ++i) {
        if (str_eq(nodes[i].text, "NullPropsB"))
            npb_id_after = nodes[i].id;
    }
    ok &= check(npb_id_after == npb_id_before,
                "ui.box(null, ...) preserves stable sibling IDs across conditional mount");

    // Bug 8: 0-sized inlay_surface_view grows to fill parent container!
    InlaySurfaceViewDesc grow_view = inlay_surface_view_desc_default();
    grow_view.id = etch_id("v013-grow-surface");
    grow_view.width_px = 0.0f;
    grow_view.height_px = 0.0f;
    auto gframe = etch_frame_desc_default();
    gframe.viewport_w = 500;
    gframe.viewport_h = 400;
    for (int i = 0; i < 2; ++i) {
        etch_begin_frame(ctx, &gframe);
        auto pbox = etch_box_desc_default();
        pbox.axis = ETCH_AXIS_COL;
        pbox.width = etch_sizing_fixed(420.f);
        pbox.height = etch_sizing_fixed(260.f);
        etch_begin_box(&pbox);
        inlay_surface_view(s, &grow_view);
        etch_end_box();
        etch_end_frame(ctx);
    }
    const auto grow_b = etch_element_bounds(grow_view.id);
    ok &= check(std::fabs(grow_b.w - 420.f) <= 1.f && std::fabs(grow_b.h - 260.f) <= 1.f,
                "0-sized inlay_surface_view grows to fill parent container");

    inlay_surface_destroy(s);
    return ok;
}

bool v013_extended_widgets_and_etch028(EtchContext *ctx, const InlaySurfaceDesc &desc) {
    InlaySurfaceDesc big_desc = desc;
    big_desc.max_text_bytes = 512u * 1024u;
    big_desc.max_nodes = 8192u;
    auto *s = inlay_surface_create(ctx, &big_desc);
    if (!s)
        return false;

    bool ok = true;
    auto frame = etch_frame_desc_default();
    frame.viewport_w = 720;
    frame.viewport_h = 900;
    frame.dt_seconds = 1.f / 60.f;
    const EtchVec2 default_ptr = frame.pointer_pos;
    InlaySurfaceViewDesc view = inlay_surface_view_desc_default();
    view.id = etch_id("v013-ext");
    view.width_px = 680;
    view.height_px = 860;
    const auto step = [&] {
        etch_begin_frame(ctx, &frame);
        inlay_surface_view(s, &view);
        etch_end_frame(ctx);
    };
    const auto click_pt = [&](float x, float y) {
        frame.pointer_pos = {x, y};
        frame.pointer_down = true;
        step();
        frame.pointer_down = false;
        step();
        frame.pointer_pos = default_ptr;
        step();
    };

    // Phase A: Doc (streaming + link click), Terminal (ANSI 256/truecolor + CJK + >8KiB lines growth),
    //          Table (numeric sort 0-JS-tick + row click), Tree/TreeItem, Menu/ContextMenu,
    //          Avatar, Toast (presence enter/exit), Accordion, Skeleton, Progress (ring/indet),
    //          Badge (variant/dot), Switch, blendMode, alphaMask, OpenType tabularNums/noLiga.
    static const char ext_script[] = R"JS(
const BIG_LINES = Array.from({ length: 240 }, (_, i) =>
  `L${i} \x1b[38;2;52;211;153m\x1b[38;5;208m中文测试\x1b[0m 0123456789ABCDEF0123456789`
);
function render(s) {
  return (
    <Col id="eroot" width={640} gap={8} padding={8}>
      <Text id="before_term">BeforeTerm</Text>
      <Doc
        id="udoc"
        width={300}
        blockPadding={4}
        fontSize={16}
        markdown={'[OpenInlayDocs](https://inlay.dev)' + (s.docExtra || '')}
        onLinkClick={e => { state.linkUrl = e.url; }}
      />
      <Table
        id="utbl"
        width={280}
        headerHeight={32}
        rowHeight={30}
        columns={[
          { key: 'name', title: 'Name' },
          { key: 'val', title: 'Value', numeric: true }
        ]}
        rows={[
          { name: 'Beta',  val: '20ms'  },
          { name: 'Alpha', val: '100ms' },
          { name: 'Gamma', val: '9ms'   }
        ]}
        onRowClick={e => { state.clickedRow = e.row; }}
      />
      <Row cls="items-center" gap={10}>
        <Avatar id="uav" name="Ada Lovelace" size="md" status="online" ring />
        <Badge id="ubdg_s" variant="success" dot>Healthy</Badge>
        <Badge id="ubdg_d" variant="danger" dot />
        <Progress id="upring" variant="ring" value={72} min={0} max={100} width={48} height={48} thickness={5} showPercent />
        <Progress id="upind" indeterminate width={100} height={8} />
        <Switch id="usw" label="Mode" size="sm" checked={!!s.sw} onChange={v => { state.sw = v; }} />
      </Row>
      <Row gap={10}>
        <Tree id="utree" width={180}>
          <TreeItem id="ti_root" label="Root" defaultOpen>
            <TreeItem id="ti_leaf" label="LeafNode" leaf onSelect={e => { state.treeSel = e.label; }} />
          </TreeItem>
        </Tree>
        <Accordion id="uacc" title="Details" defaultOpen={false} onToggle={e => { state.accOpen = e.open; }}>
          <Text>Accordion Body</Text>
        </Accordion>
      </Row>
      <Row gap={10}>
        <Menu
          id="umnu"
          label="Actions"
          items={[
            { label: 'Copy', shortcut: 'Ctrl+C', value: 'copy' },
            { separator: true },
            { label: 'Delete', danger: true, value: 'del' }
          ]}
          onSelect={e => { state.menuSel = e.value; }}
        />
        <ContextMenu id="uctx" items={['Inspect', 'Reload']}>
          <Box id="uctx_box" width={90} height={28} bg="#1e293b"><Text>Target</Text></Box>
        </ContextMenu>
        <Skeleton id="usk_r" variant="rect" width={80} height={16} animated={false} />
        <Skeleton id="usk_c" variant="circle" width={28} height={28} animated={false} />
        <Skeleton id="usk_t" variant="text" lines={3} width={90} animated={false} />
      </Row>
      <Row gap={8}>
        <Box id="ubm_m" width={40} height={20} bg="#3b82f6" blendMode="multiply" />
        <Box id="ubm_s" width={40} height={20} bg="#10b981" blendMode="screen" />
        <Box id="ubm_a" width={40} height={20} bg="#f59e0b" blendMode="additive" />
        <Box id="uam_f" width={60} height={20} bg="#6366f1" alphaMask="fade-bottom" />
        <Box id="uam_sp" width={60} height={20} bg="#ec4899" alphaMask="spotlight" />
        <Text id="uot" tabularNums noLiga cls="tabular-nums no-liga">0123 fi fl</Text>
      </Row>
      <Terminal id="uterm" title="ansi" cols={42} rows={4} fontSize={11} lines={BIG_LINES} />
      <Toast
        id="utoast"
        open={!!s.toastOpen}
        variant="success"
        title="Saved"
        description="Toast body"
        actionLabel="Undo"
        onClose={() => { state.toastOpen = false; }}
      />
    </Col>
  );
}
function onEvent(name, v, s) {
  if (name === 'docAppend') s.docExtra = v;
  if (name === 'toast') s.toastOpen = !!v;
}
)JS";
    ok &= check(inlay_surface_set_script(
                    s, {ext_script, uint32_t(sizeof(ext_script) - 1u), true}),
                "v0.13 extended widgets + Etch 0.28 props script loads");
    if (inlay_surface_stats(s).has_error) {
        const auto err = inlay_surface_error(s);
        std::printf("v0.13 ext error: %.*s\n", int(err.length), err.chars ? err.chars : "");
    }
    ok &= check(!inlay_surface_stats(s).has_error, "v0.13 extended widgets script has no error");

    uint32_t cnt = 0;
    const auto *nodes = inlay_surface_nodes(s, &cnt);
    ok &= check(has_node_kind(nodes, cnt, INLAY_UI_DOC) &&
                    has_node_kind(nodes, cnt, INLAY_UI_TERMINAL) &&
                    has_node_kind(nodes, cnt, INLAY_UI_TABLE) &&
                    has_node_kind(nodes, cnt, INLAY_UI_TREE) &&
                    has_node_kind(nodes, cnt, INLAY_UI_TREE_ITEM) &&
                    has_node_kind(nodes, cnt, INLAY_UI_MENU) &&
                    has_node_kind(nodes, cnt, INLAY_UI_CONTEXT_MENU) &&
                    has_node_kind(nodes, cnt, INLAY_UI_AVATAR) &&
                    has_node_kind(nodes, cnt, INLAY_UI_TOAST) &&
                    has_node_kind(nodes, cnt, INLAY_UI_ACCORDION) &&
                    has_node_kind(nodes, cnt, INLAY_UI_SKELETON),
                "all extended widget node kinds present");

    // Verify dynamic s->text growth (> 8 KiB terminal lines) rebased earlier node text cleanly!
    const auto *before_term_n = find_node_by_id(nodes, cnt, "before_term");
    ok &= check(before_term_n && str_eq(before_term_n->text, "BeforeTerm"),
                "terminal >8KiB lines growth preserves and rebases earlier node text");

    step();
    step();

    // Check widget bounds: Avatar, dot-only Badge, Ring Progress, Skeletons, Terminal.
    const auto uav_b = etch_element_bounds(find_surface_eid(nodes, cnt, view.id, "uav"));
    const auto bdg_d_b = etch_element_bounds(find_surface_eid(nodes, cnt, view.id, "ubdg_d"));
    const auto ring_b = etch_element_bounds(find_surface_eid(nodes, cnt, view.id, "upring"));
    const auto skr_b = etch_element_bounds(find_surface_eid(nodes, cnt, view.id, "usk_r"));
    const auto skc_b = etch_element_bounds(find_surface_eid(nodes, cnt, view.id, "usk_c"));
    const auto skt_b = etch_element_bounds(find_surface_eid(nodes, cnt, view.id, "usk_t"));
    const auto term_b = etch_element_bounds(find_surface_eid(nodes, cnt, view.id, "uterm"));
    ok &= check(std::fabs(uav_b.w - 32.f) <= 0.5f && std::fabs(uav_b.h - 32.f) <= 0.5f,
                "ui.avatar md renders 32x32");
    ok &= check(std::fabs(bdg_d_b.w - 8.f) <= 0.5f && std::fabs(bdg_d_b.h - 8.f) <= 0.5f,
                "dot-only ui.badge renders 8x8");
    ok &= check(std::fabs(ring_b.w - 48.f) <= 0.5f && std::fabs(ring_b.h - 48.f) <= 0.5f,
                "ui.progress ring renders 48x48");
    ok &= check(std::fabs(skr_b.w - 80.f) <= 0.5f && std::fabs(skc_b.w - 28.f) <= 0.5f &&
                    skt_b.h > 30.f && term_b.w > 100.f && term_b.h > 40.f,
                "ui.skeleton (rect/circle/text) and ui.terminal render with valid bounds");

    // ui.doc link click + streaming prefix append.
    const auto udoc_b = etch_element_bounds(find_surface_eid(nodes, cnt, view.id, "udoc"));
    const float doc_h1 = udoc_b.h;
    click_pt(udoc_b.x + 24.f, udoc_b.y + 12.f);
    auto st_json = inlay_surface_state_json(s);
    ok &= check(st_json.chars && std::strstr(st_json.chars, "https://inlay.dev") != nullptr,
                "ui.doc link click dispatches onLinkClick with URL");
    ok &= check(inlay_surface_dispatch(
                    s, ETCH_STR_LIT("docAppend"),
                    ETCH_STR_LIT("\"\\n\\nStreamed paragraph appended via prefix fast-path.\"")),
                "ui.doc streaming prefix append");
    step();
    step();
    nodes = inlay_surface_nodes(s, &cnt);
    const auto udoc_b2 = etch_element_bounds(find_surface_eid(nodes, cnt, view.id, "udoc"));
    ok &= check(udoc_b2.h > doc_h1 + 10.f, "ui.doc streaming prefix append grows rendered height");

    // ui.table: click numeric Value header -> 0 JS ticks + numeric descending puts 100ms (orig row 1) first!
    // Note: in Etch begin_table, utbl's element_id is the body virtual list below the 32px header.
    const auto tbl_b = etch_element_bounds(find_surface_eid(nodes, cnt, view.id, "utbl"));
    const uint64_t ticks_before_sort = inlay_surface_stats(s).js_ticks;
    click_pt(tbl_b.x + tbl_b.w * 0.72f, tbl_b.y - 16.f);
    ok &= check(inlay_surface_stats(s).js_ticks == ticks_before_sort,
                "ui.table native column sort runs 0 JS ticks");
    click_pt(tbl_b.x + 60.f, tbl_b.y + 15.f);
    st_json = inlay_surface_state_json(s);
    ok &= check(st_json.chars && std::strstr(st_json.chars, "\"clickedRow\":1") != nullptr,
                "ui.table numeric sort orders 100ms > 20ms > 9ms and maps row click to orig row 1");

    // ui.switch toggle via a11y press.
    const uint32_t usw_eid = find_surface_eid(nodes, cnt, view.id, "usw");
    ok &= check(etch_perform_a11y_action(usw_eid, ETCH_A11Y_ACT_PRESS, 0, {}, ctx, nullptr),
                "activate ui.switch");
    step();
    step();
    st_json = inlay_surface_state_json(s);
    ok &= check(st_json.chars && std::strstr(st_json.chars, "\"sw\":true") != nullptr,
                "ui.switch updates state to true");

    // ui.treeItem click fires onSelect (click past depth-1 16px indent + 16px chevron spacer onto label).
    nodes = inlay_surface_nodes(s, &cnt);
    const auto leaf_b = etch_element_bounds(find_surface_eid(nodes, cnt, view.id, "ti_leaf"));
    click_pt(leaf_b.x + 60.f, leaf_b.y + leaf_b.h * 0.5f);
    st_json = inlay_surface_state_json(s);
    ok &= check(st_json.chars && std::strstr(st_json.chars, "\"treeSel\":\"LeafNode\"") != nullptr,
                "ui.treeItem click fires onSelect");

    // ui.accordion header click toggles open and fires onToggle.
    nodes = inlay_surface_nodes(s, &cnt);
    const uint32_t acc_eid = find_surface_eid(nodes, cnt, view.id, "uacc");
    const auto acc_hdr_b = etch_element_bounds(etch_id_index(acc_eid, 0xC011u));
    click_pt(acc_hdr_b.x + 24.f, acc_hdr_b.y + acc_hdr_b.h * 0.5f);
    st_json = inlay_surface_state_json(s);
    ok &= check(st_json.chars && std::strstr(st_json.chars, "\"accOpen\":true") != nullptr,
                "ui.accordion toggle opens and dispatches onToggle");

    // ui.menu trigger press opens popup cleanly and Escape dismisses it.
    nodes = inlay_surface_nodes(s, &cnt);
    const uint32_t mnu_eid = find_surface_eid(nodes, cnt, view.id, "umnu");
    const uint32_t mnu_trig = etch_id_index(mnu_eid, 0xE101u);
    const uint32_t mnu_pop = etch_id_index(mnu_eid, 0xE102u);
    ok &= check(etch_perform_a11y_action(mnu_trig, ETCH_A11Y_ACT_PRESS, 0, {}, ctx, nullptr),
                "press ui.menu trigger");
    step();
    bool menu_opened = false;
    etch_begin_frame(ctx, &frame);
    inlay_surface_view(s, &view);
    menu_opened = etch_is_popup_open(mnu_pop);
    etch_end_frame(ctx);
    ok &= check(menu_opened, "ui.menu opens popup cleanly");
    frame.key_pressed_code = SDLK_ESCAPE;
    step();
    frame.key_pressed_code = 0u;
    bool menu_closed = false;
    etch_begin_frame(ctx, &frame);
    inlay_surface_view(s, &view);
    menu_closed = !etch_is_popup_open(mnu_pop);
    etch_end_frame(ctx);
    ok &= check(menu_closed, "ui.menu closes popup on Escape");

    // ui.toast presence enter & exit settles cleanly with 0 extra JS ticks.
    ok &= check(inlay_surface_dispatch(s, ETCH_STR_LIT("toast"), ETCH_STR_LIT("true")),
                "open ui.toast");
    for (int i = 0; i < 36; ++i)
        step();
    ok &= check(!inlay_surface_stats(s).has_error && etch_frame_prim_count(ctx) > 0u,
                "ui.toast presence enter renders cleanly");
    const uint64_t ticks_before_toast_exit = inlay_surface_stats(s).js_ticks;
    ok &= check(inlay_surface_dispatch(s, ETCH_STR_LIT("toast"), ETCH_STR_LIT("false")),
                "close ui.toast");
    for (int i = 0; i < 48; ++i)
        step();
    ok &= check(inlay_surface_stats(s).js_ticks == ticks_before_toast_exit + 1u,
                "ui.toast presence exit runs 0 extra JS ticks");

    // Phase B: ui.virtualList — 10,000-item data mode + 10,000-item windowed renderItem mode (O(visible) calls!).
    ok &= check(inlay_surface_set_state_json(s, ETCH_STR_LIT("{}")), "reset state for virtualList");
    static const char vlist_data_script[] = R"JS(
const ITEMS_10K = Array.from({ length: 10000 }, (_, i) => 'Item ' + i);
function render() {
  return (
    <VirtualList
      id="vdata"
      items={ITEMS_10K}
      itemHeight={28}
      width={320}
      height={200}
    />
  );
}
)JS";
    ok &= check(inlay_surface_set_script(
                    s, {vlist_data_script, uint32_t(sizeof(vlist_data_script) - 1u), true}),
                "ui.virtualList 10,000-item data mode loads");
    nodes = inlay_surface_nodes(s, &cnt);
    ok &= check(has_node_kind(nodes, cnt, INLAY_UI_VIRTUAL_LIST) && !inlay_surface_stats(s).has_error,
                "ui.virtualList 10,000-item data mode builds node");
    step();
    step();
    const uint64_t vdata_ticks = inlay_surface_stats(s).js_ticks;
    for (int i = 0; i < 6; ++i)
        step();
    ok &= check(inlay_surface_stats(s).js_ticks == vdata_ticks,
                "ui.virtualList 10,000-item data mode runs 0 idle JS ticks");

    static const char vlist_win_script[] = R"JS(
function render(s) {
  s.calls = 0;
  return (
    <VirtualList
      id="vwin"
      count={10000}
      itemHeight={28}
      width={320}
      height={200}
      renderItem={i => {
        s.calls = (s.calls || 0) + 1;
        return <Text key={'r' + i}>{'Row ' + i}</Text>;
      }}
    />
  );
}
)JS";
    ok &= check(inlay_surface_set_script(
                    s, {vlist_win_script, uint32_t(sizeof(vlist_win_script) - 1u), true}),
                "ui.virtualList 10,000-item windowed renderItem mode loads");
    step();
    step();
    const auto vwin_json = inlay_surface_state_json(s);
    int render_calls = 0;
    if (vwin_json.chars) {
        if (const char *p = std::strstr(vwin_json.chars, "\"calls\":"))
            render_calls = std::atoi(p + 8);
    }
    ok &= check(render_calls > 0 && render_calls <= 64,
                "ui.virtualList renderItem invokes O(visible) callbacks, not 10,000");
    const uint64_t vwin_ticks = inlay_surface_stats(s).js_ticks;
    for (int i = 0; i < 6; ++i)
        step();
    ok &= check(inlay_surface_stats(s).js_ticks == vwin_ticks,
                "ui.virtualList renderItem mode runs 0 idle JS ticks");

    inlay_surface_destroy(s);
    return ok;
}

bool v013_review_defect_regressions(EtchContext *ctx, const InlaySurfaceDesc &desc) {
    InlaySurfaceDesc big_desc = desc;
    big_desc.max_text_bytes = 512u * 1024u;
    big_desc.max_nodes = 4096u;
    auto *s = inlay_surface_create(ctx, &big_desc);
    if (!s)
        return false;

    bool ok = true;
    auto frame = etch_frame_desc_default();
    frame.viewport_w = 720;
    frame.viewport_h = 900;
    frame.dt_seconds = 1.f / 60.f;
    const EtchVec2 default_ptr = frame.pointer_pos;
    InlaySurfaceViewDesc view = inlay_surface_view_desc_default();
    view.id = etch_id("v013-review");
    view.width_px = 680;
    view.height_px = 860;
    const auto step = [&] {
        etch_begin_frame(ctx, &frame);
        inlay_surface_view(s, &view);
        etch_end_frame(ctx);
    };
    const auto click_pt = [&](float x, float y) {
        frame.pointer_pos = {x, y};
        frame.pointer_down = true;
        step();
        frame.pointer_down = false;
        step();
        frame.pointer_pos = default_ptr;
        step();
    };
    const auto has_text = [](const InlayUiNode *nodes, uint32_t cnt, const char *lit) {
        for (uint32_t i = 0; i < cnt; ++i) {
            if (str_eq(nodes[i].text, lit))
                return true;
        }
        return false;
    };

    // 1. P0: >64 KiB JSON payload safety (no heap overflow) & control-char escaping (no truncation).
    static const char p0_script[] = R"JS(
const BIG_LBL = 'x'.repeat(70000);
const CTRL_LBL = '\x01\x02\x03\x1f"\\'.repeat(80);
function render() {
  return (
    <Tree id="p0tree" width={300}>
      <TreeItem id="ti_big" label={BIG_LBL} leaf onSelect={e => { state.bigLen = e.label.length; }} />
      <TreeItem id="ti_ctrl" label={CTRL_LBL} leaf onSelect={e => { state.ctrlLen = e.label.length; }} />
    </Tree>
  );
}
)JS";
    ok &= check(inlay_surface_set_script(s, {p0_script, uint32_t(sizeof(p0_script) - 1u), true}),
                "P0 oversized/control-char script loads");
    step();
    step();
    uint32_t cnt = 0;
    const auto *nodes = inlay_surface_nodes(s, &cnt);
    const auto big_b = etch_element_bounds(find_surface_eid(nodes, cnt, view.id, "ti_big"));
    const auto ctrl_b = etch_element_bounds(find_surface_eid(nodes, cnt, view.id, "ti_ctrl"));
    click_pt(big_b.x + 24.f, big_b.y + big_b.h * 0.5f);
    auto st_json = inlay_surface_state_json(s);
    ok &= check(!inlay_surface_stats(s).has_error &&
                    (!st_json.chars || std::strstr(st_json.chars, "bigLen") == nullptr),
                "P0 >64KiB widget event payload safely rejected without crash or error");
    click_pt(ctrl_b.x + 24.f, ctrl_b.y + ctrl_b.h * 0.5f);
    st_json = inlay_surface_state_json(s);
    ok &= check(!inlay_surface_stats(s).has_error && st_json.chars &&
                    std::strstr(st_json.chars, "\"ctrlLen\":480") != nullptr,
                "P0 control-char-heavy JSON string escapes completely without truncation");

    // 2. P1: Two unkeyed side-by-side VirtualList(renderItem) never collide + static-child VirtualList has 0 scroll rebuilds.
    ok &= check(inlay_surface_set_state_json(s, ETCH_STR_LIT("{}")), "reset state for vlist collision check");
    static const char vl_script[] = R"JS(
function render() {
  return (
    <Row id="vl_row" width={640} gap={8}>
      <Box id="vl_pane_a" width={200} height={160}>
        <VirtualList count={1000} itemHeight={28} width={200} height={140}
          renderItem={i => <Text key={'a' + i}>{'A_' + i}</Text>} />
      </Box>
      <Box id="vl_pane_b" width={200} height={160}>
        <VirtualList count={1000} itemHeight={28} width={200} height={140}
          renderItem={i => <Text key={'b' + i}>{'B_' + i}</Text>} />
      </Box>
      <Box id="vl_pane_s" width={200} height={160}>
        <VirtualList count={1000} itemHeight={28} width={200} height={140}>
          <Text>StaticRow0</Text>
        </VirtualList>
      </Box>
    </Row>
  );
}
)JS";
    ok &= check(inlay_surface_set_script(s, {vl_script, uint32_t(sizeof(vl_script) - 1u), true}),
                "P1 dual unkeyed VirtualList script loads");
    step();
    step();
    nodes = inlay_surface_nodes(s, &cnt);
    const auto *pane_a_n = find_node_by_id(nodes, cnt, "vl_pane_a");
    const auto *pane_s_n = find_node_by_id(nodes, cnt, "vl_pane_s");
    const uint32_t pane_a_eid = find_surface_eid(nodes, cnt, view.id, "vl_pane_a");
    const uint32_t pane_s_eid = find_surface_eid(nodes, cnt, view.id, "vl_pane_s");
    const uint32_t vl_a_eid = (pane_a_n && pane_a_n->first_child != UINT32_MAX)
        ? etch_id_index(pane_a_eid, nodes[pane_a_n->first_child].id)
        : 0u;
    const uint32_t vl_s_eid = (pane_s_n && pane_s_n->first_child != UINT32_MAX)
        ? etch_id_index(pane_s_eid, nodes[pane_s_n->first_child].id)
        : 0u;
    ok &= check(vl_a_eid != 0u && vl_s_eid != 0u && has_text(nodes, cnt, "A_0") && has_text(nodes, cnt, "B_0"),
                "P1 both unkeyed VirtualLists render row 0 initially");
    etch_begin_frame(ctx, &frame);
    etch_set_virtual_list_scroll_offset(vl_a_eid, 2800.0, true);
    inlay_surface_view(s, &view);
    etch_end_frame(ctx);
    step();
    step();
    nodes = inlay_surface_nodes(s, &cnt);
    ok &= check(has_text(nodes, cnt, "A_100") && has_text(nodes, cnt, "B_0"),
                "P1 scrolling unkeyed VirtualList A refills A_100 while unkeyed VirtualList B stays at B_0");
    const uint64_t vl_idle_ticks = inlay_surface_stats(s).js_ticks;
    for (int i = 0; i < 6; ++i)
        step();
    ok &= check(inlay_surface_stats(s).js_ticks == vl_idle_ticks,
                "P1 two unkeyed VirtualLists at different offsets run 0 idle JS ticks");
    etch_begin_frame(ctx, &frame);
    etch_set_virtual_list_scroll_offset(vl_s_eid, 2800.0, true);
    inlay_surface_view(s, &view);
    etch_end_frame(ctx);
    step();
    step();
    ok &= check(inlay_surface_stats(s).js_ticks == vl_idle_ticks,
                "P1 scrolling static-children VirtualList triggers 0 spurious JS rebuilds");

    // 3. P1: Sparse JS array watchdog safety (`const a = []; a[0x7FFFFFFF] = 'x'`).
    auto *wd = inlay_surface_create(ctx, nullptr);
    ok &= check(wd != nullptr, "watchdog surface for sparse array checks");
    if (wd) {
        const double freq = double(SDL_GetPerformanceFrequency());
        const auto run_bounded = [&](const char *src, bool must_fail, double limit_ms, const char *label) {
            const uint64_t t0 = SDL_GetPerformanceCounter();
            const bool loaded = inlay_surface_set_script(wd, {src, uint32_t(std::strlen(src)), true});
            const double ms = double(SDL_GetPerformanceCounter() - t0) * 1000.0 / freq;
            ok &= check((!must_fail || !loaded) && ms < limit_ms, label);
        };
        run_bounded("const a=[];a[0x7FFFFFFF]='x';function render(){return <Text>{a}</Text>;}",
                    true, native_conversion_limit_ms, "P1 sparse array in <Text> bounded & rejected");
        run_bounded("const a=[];a[0x7FFFFFFF]='x';function render(){return <Terminal lines={a} />;}",
                    true, native_conversion_limit_ms, "P1 sparse array in <Terminal lines> bounded & rejected");
        run_bounded("const a=[];a[0x7FFFFFFF]='x';function render(){return <VirtualList items={a} />;}",
                    false, native_conversion_limit_ms * 3.0, "P1 sparse array in <VirtualList items> bounded by watchdog");
        run_bounded("const a=[];a[0x7FFFFFFF]='x';function render(){return <Table columns={['A']} rows={a} />;}",
                    false, native_conversion_limit_ms * 3.0, "P1 sparse array in <Table rows> bounded by watchdog");
        inlay_surface_destroy(wd);
    }

    // 4. P1: Controlled Table sortColumn switching from non-sortable col 0 -> sortable col 1.
    ok &= check(inlay_surface_set_state_json(s, ETCH_STR_LIT("{\"sc\":0,\"sd\":false}")),
                "init controlled table sort state");
    static const char ctbl_script[] = R"JS(
function render(s) {
  return (
    <Table
      id="ctbl"
      width={280}
      headerHeight={32}
      rowHeight={30}
      sortColumn={s.sc}
      sortDesc={!!s.sd}
      columns={[
        { key: 'a', title: 'A', sortable: false },
        { key: 'v', title: 'V', numeric: true, sortable: true }
      ]}
      rows={[
        { a: 'R0', v: '50' },
        { a: 'R1', v: '10' },
        { a: 'R2', v: '90' }
      ]}
      onRowClick={e => { state.row = e.row; }}
    />
  );
}
function onEvent(n, v, s) {
  if (n === 'sort') { s.sc = v.c; s.sd = !!v.d; }
}
)JS";
    ok &= check(inlay_surface_set_script(s, {ctbl_script, uint32_t(sizeof(ctbl_script) - 1u), true}),
                "P1 controlled table script loads");
    step();
    step();
    const uint64_t tbl_idle0 = inlay_surface_stats(s).js_ticks;
    for (int i = 0; i < 4; ++i)
        step();
    ok &= check(inlay_surface_stats(s).js_ticks == tbl_idle0,
                "P1 controlled table on non-sortable col 0 runs 0 idle JS ticks");
    ok &= check(inlay_surface_dispatch(s, ETCH_STR_LIT("sort"), ETCH_STR_LIT("{\"c\":1,\"d\":true}")),
                "P1 switch controlled table sortColumn 0 -> 1 desc");
    step();
    step();
    nodes = inlay_surface_nodes(s, &cnt);
    const auto ctbl_b = etch_element_bounds(find_surface_eid(nodes, cnt, view.id, "ctbl"));
    click_pt(ctbl_b.x + 60.f, ctbl_b.y + 15.f);
    st_json = inlay_surface_state_json(s);
    ok &= check(st_json.chars && std::strstr(st_json.chars, "\"row\":2") != nullptr,
                "P1 controlled sortColumn switch from non-sortable col 0 to sortable col 1 sorts 90 first (orig row 2)");

    // 5. P1: 16 concurrent <Skeleton animate /> nodes animate cleanly without collision freeze.
    static const char sk16_script[] = R"JS(
function render() {
  const items = [];
  for (let i = 0; i < 16; ++i) {
    items.push(
      <Skeleton key={'sk' + i} id={'sk' + i} variant="rect" width={60} height={12}
                bg="#101820" shimmerColor="#f0f8ff" animate />
    );
  }
  return <Col id="sk_root" width={120} gap={4}>{items}</Col>;
}
)JS";
    ok &= check(inlay_surface_set_script(s, {sk16_script, uint32_t(sizeof(sk16_script) - 1u), true}),
                "P1 16 concurrent animated skeletons script loads");
    const uint64_t sk_ticks0 = inlay_surface_stats(s).js_ticks;
    for (int i = 0; i < 25; ++i)
        step();
    ok &= check(inlay_surface_stats(s).js_ticks == sk_ticks0,
                "P1 16 animated skeletons run 0 idle JS ticks");
    nodes = inlay_surface_nodes(s, &cnt);
    const uint32_t sk_base_rgba = etch_color_pack_rgba8(etch_rgba8(0x10u, 0x18u, 0x20u, 0xFFu));
    uint32_t animated_ok = 0u;
    for (int i = 0; i < 16; ++i) {
        char id_buf[16] = {};
        std::snprintf(id_buf, sizeof(id_buf), "sk%d", i);
        const auto b = etch_element_bounds(find_surface_eid(nodes, cnt, view.id, id_buf));
        const uint32_t col = find_rect_fill_rgba(ctx, b);
        if (col != 0u && col != sk_base_rgba)
            ++animated_ok;
    }
    ok &= check(animated_ok == 16u,
                "P1 all 16 concurrent <Skeleton animate /> nodes pulse without collision freeze");

    // 6. P2: <Comp children="explicit" />, cls="flex-1" inside <Col> & <Row>,
    //        Button/Avatar events, Menu controlled open={false}, Toast exit disable,
    //        ColorTween duration-0 snap, and non-HMR JSX syntax error state preservation.
    ok &= check(inlay_surface_set_state_json(s, ETCH_STR_LIT("{\"topen\":true,\"cstep\":0}")),
                "init P2 state");
    static const char p2_script[] = R"JS(
function Comp(props) {
  return <Text id="comp_out">{props.children}</Text>;
}
function render(s) {
  const step = s.cstep || 0;
  const cbg = step === 0 ? '#112233' : (step === 1 ? '#ee2222' : (step === 2 ? '#22ee22' : '#112233'));
  const ccls = step === 2 ? 'transition duration-0' : 'transition duration-200';
  return (
    <Col id="p2root" width={400} gap={6}>
      <Comp children="explicit" />
      <Col id="col_flex" width={200} height={260}>
        <Box width={200} height={60} bg="#223344" />
        <Box id="col_grow" cls="w-full flex-1" bg="#334455" />
      </Col>
      <Row id="row_flex" width={300} height={40}>
        <Box width={80} height={40} bg="#223344" />
        <Box id="row_grow" cls="h-full flex-1" bg="#445566" />
      </Row>
      <Row gap={8}>
        <Button id="p2btn" onHover={h => { state.btnHov = h; }} onFocus={f => { state.btnFoc = f; }}>Btn</Button>
        <Avatar id="p2av" name="Linus Torvalds" onHover={h => { state.avHov = h; }} onRightClick={() => { state.avRc = true; }} />
        <Menu id="p2mnu" open={false} label="Locked" items={['One']} onOpenChange={v => { state.mnuReq = v; }} />
      </Row>
      <Box id="ctw" width={60} height={20} bg={cbg} cls={ccls} />
      <Toast id="p2toast" open={!!s.topen} title="Hi" actionLabel="Undo"
             onAction={() => { state.toastAct = true; }} />
    </Col>
  );
}
function onEvent(n, v, s) {
  if (n === 'topen') s.topen = !!v;
  if (n === 'cstep') s.cstep = v;
  if (n === 'keep') s.kept = v;
}
)JS";
    ok &= check(inlay_surface_set_script(s, {p2_script, uint32_t(sizeof(p2_script) - 1u), true}),
                "P2 polish regressions script loads");
    step();
    step();
    nodes = inlay_surface_nodes(s, &cnt);
    const auto *comp_n = find_node_by_id(nodes, cnt, "comp_out");
    ok &= check(comp_n && str_eq(comp_n->text, "explicit"),
                "P2 <Comp children=\"explicit\" /> preserves explicit props.children");
    const auto col_g_b = etch_element_bounds(find_surface_eid(nodes, cnt, view.id, "col_grow"));
    const auto row_g_b = etch_element_bounds(find_surface_eid(nodes, cnt, view.id, "row_grow"));
    ok &= check(std::fabs(col_g_b.h - 200.f) <= 1.f && std::fabs(row_g_b.w - 220.f) <= 1.f,
                "P2 cls=\"flex-1\" grows vertically inside <Col> and horizontally inside <Row>");

    // Button onHover/onFocus & Avatar onHover/onRightClick
    const uint32_t btn_eid = find_surface_eid(nodes, cnt, view.id, "p2btn");
    const auto btn_b = etch_element_bounds(btn_eid);
    frame.pointer_pos = {btn_b.x + btn_b.w * 0.5f, btn_b.y + btn_b.h * 0.5f};
    step();
    step();
    ok &= check(etch_perform_a11y_action(btn_eid, ETCH_A11Y_ACT_FOCUS, 0, {}, ctx, nullptr),
                "P2 focus button via a11y");
    step();
    step();
    frame.pointer_pos = default_ptr;
    step();
    st_json = inlay_surface_state_json(s);
    ok &= check(st_json.chars && std::strstr(st_json.chars, "\"btnHov\":") != nullptr &&
                    std::strstr(st_json.chars, "\"btnFoc\":true") != nullptr,
                "P2 Button dispatches onHover and onFocus");

    nodes = inlay_surface_nodes(s, &cnt);
    const auto av_b = etch_element_bounds(find_surface_eid(nodes, cnt, view.id, "p2av"));
    frame.pointer_pos = {av_b.x + av_b.w * 0.5f, av_b.y + av_b.h * 0.5f};
    frame.right_pointer_down = true;
    step();
    frame.right_pointer_down = false;
    step();
    frame.pointer_pos = default_ptr;
    step();
    st_json = inlay_surface_state_json(s);
    ok &= check(st_json.chars && std::strstr(st_json.chars, "\"avHov\":") != nullptr &&
                    std::strstr(st_json.chars, "\"avRc\":true") != nullptr,
                "P2 Avatar dispatches onHover and onRightClick");

    // Menu controlled open={false} -> zero 1-frame popup flicker + emits onOpenChange(true)
    nodes = inlay_surface_nodes(s, &cnt);
    const uint32_t mnu_eid = find_surface_eid(nodes, cnt, view.id, "p2mnu");
    const uint32_t mnu_trig = etch_id_index(mnu_eid, 0xE101u);
    const uint32_t mnu_pop = etch_id_index(mnu_eid, 0xE102u);
    ok &= check(etch_perform_a11y_action(mnu_trig, ETCH_A11Y_ACT_PRESS, 0, {}, ctx, nullptr),
                "P2 press controlled closed Menu trigger");
    bool flicker_open = true;
    etch_begin_frame(ctx, &frame);
    inlay_surface_view(s, &view);
    flicker_open = etch_is_popup_open(mnu_pop);
    etch_end_frame(ctx);
    st_json = inlay_surface_state_json(s);
    ok &= check(!flicker_open && st_json.chars && std::strstr(st_json.chars, "\"mnuReq\":true") != nullptr,
                "P2 controlled Menu open={false} has zero 1-frame flicker and emits onOpenChange");

    // Toast action button disabled during exit fade-out
    for (int i = 0; i < 20; ++i)
        step();
    nodes = inlay_surface_nodes(s, &cnt);
    const uint32_t toast_eid = find_surface_eid(nodes, cnt, view.id, "p2toast");
    const auto toast_act_b = etch_element_bounds(etch_id_index(toast_eid, 0x70A1u));
    ok &= check(inlay_surface_dispatch(s, ETCH_STR_LIT("topen"), ETCH_STR_LIT("false")),
                "P2 begin Toast exit fade-out");
    step();
    click_pt(toast_act_b.x + toast_act_b.w * 0.5f, toast_act_b.y + toast_act_b.h * 0.5f);
    st_json = inlay_surface_state_json(s);
    ok &= check(st_json.chars && std::strstr(st_json.chars, "\"toastAct\":true") == nullptr,
                "P2 Toast action button disabled during exit fade-out");

    // ColorTweenSlot duration == 0 snaps slot cleanly before next timed transition
    ok &= check(inlay_surface_dispatch(s, ETCH_STR_LIT("cstep"), ETCH_STR_LIT("1")), "cstep 1 (#ee2222)");
    for (int i = 0; i < 20; ++i)
        step();
    ok &= check(inlay_surface_dispatch(s, ETCH_STR_LIT("cstep"), ETCH_STR_LIT("2")), "cstep 2 (duration-0 #22ee22)");
    step();
    ok &= check(inlay_surface_dispatch(s, ETCH_STR_LIT("cstep"), ETCH_STR_LIT("3")), "cstep 3 (duration-200 #112233)");
    step();
    nodes = inlay_surface_nodes(s, &cnt);
    const auto ctw_b = etch_element_bounds(find_surface_eid(nodes, cnt, view.id, "ctw"));
    const uint32_t ctw_col = find_rect_fill_rgba(ctx, ctw_b);
    const uint8_t ctw_g = static_cast<uint8_t>((ctw_col >> 8) & 0xFFu);
    const uint8_t ctw_r = static_cast<uint8_t>(ctw_col & 0xFFu);
    ok &= check(ctw_g > ctw_r + 80u,
                "P2 ColorTween duration-0 updates slot so next transition starts from snapped green, not stale red");

    // Non-HMR set_script preserves state across JSX syntax error
    ok &= check(inlay_surface_dispatch(s, ETCH_STR_LIT("keep"), ETCH_STR_LIT("77")),
                "P2 mutate non-HMR state before JSX syntax error");
    ok &= check(!inlay_surface_set_script(s, ETCH_STR_LIT("function render(){return <Box><Text></Box>;}")),
                "P2 broken JSX rejected");
    ok &= check(inlay_surface_set_script(s, ETCH_STR_LIT("function render(s){return <Text>{String(s.kept)}</Text>;}")),
                "P2 valid JSX reloads cleanly");
    st_json = inlay_surface_state_json(s);
    ok &= check(st_json.chars && std::strstr(st_json.chars, "\"kept\":77") != nullptr,
                "P2 non-HMR set_script preserves prior state after JSX syntax error");

    inlay_surface_destroy(s);
    return ok;
}
bool test_scoped_hooks_cls_cache_and_dark(EtchContext *ctx) {
    bool ok = true;
    auto *s = inlay_surface_create(ctx, nullptr);
    if (!check(s != nullptr, "scoped hooks surface created")) return false;
    auto view = inlay_surface_view_desc_default();
    view.id = etch_id("scoped_hooks_cls_view");
    view.width_px = 640.f;
    view.height_px = 480.f;

    auto step_frame = [&](EtchVec2 ptr = {-1.f, -1.f}, bool down = false) {
        auto fd = etch_frame_desc_default();
        fd.viewport_w = 640u;
        fd.viewport_h = 480u;
        fd.pointer_pos = ptr;
        fd.pointer_down = down;
        etch_begin_frame(ctx, &fd);
        inlay_surface_view(s, &view);
        etch_end_frame(ctx);
    };

    // 1. Key-scoped & conditional-mount-safe useState + useRef + useMemo + useCallback + useEffect cleanup
    ok &= check(inlay_surface_set_state_json(s, ETCH_STR_LIT("{\"showA\":true,\"swap\":false,\"dep\":1,\"tick\":0}")),
                "scoped hooks initial state");
    ok &= check(inlay_surface_set_script(s, ETCH_STR_LIT(
        "let memoRuns=0, fxRuns=0, fxCleanups=0, lastCb=null, cbSame=0;"
        "function Counter({label, dep}) {"
        "  const [n, setN] = useState(0);"
        "  const ref = useRef(0);"
        "  ref.current += 1;"
        "  const doubled = useMemo(() => { memoRuns++; return dep * 2; }, [dep]);"
        "  const inc = useCallback(() => setN(c => c + 1), []);"
        "  if (label === 'B') { if (lastCb === inc) cbSame++; lastCb = inc; }"
        "  useEffect(() => { fxRuns++; return () => { fxCleanups++; }; }, [dep]);"
        "  return <Row><Text>{label+':'+n+':'+doubled+':'+ref.current}</Text><Button text={'+'+label} onClick={inc} /></Row>;"
        "}"
        "function render(st) {"
        "  const a = st.showA ? <Counter key='a' label='A' dep={st.dep} /> : null;"
        "  const b = <Counter key='b' label='B' dep={st.dep} />;"
        "  return <Col>"
        "    <Text>{'M='+memoRuns+',E='+fxRuns+',C='+fxCleanups+',S='+cbSame}</Text>"
        "    {st.swap ? [b, a] : [a, b]}"
        "  </Col>;"
        "}"
        "function onEvent(name, val, st) {"
        "  if (name === 'hideA') st.showA = false;"
        "  if (name === 'showA') st.showA = true;"
        "  if (name === 'swap') st.swap = !st.swap;"
        "  if (name === 'dep') st.dep = val;"
        "  if (name === 'noop') st.tick++;"
        "}")),
        "scoped hooks JSX script");

    auto find_text = [&](const char *prefix) -> EtchStr {
        uint32_t cnt = 0;
        const auto *nodes = inlay_surface_nodes(s, &cnt);
        const size_t plen = std::strlen(prefix);
        for (uint32_t i = 0; i < cnt; ++i) {
            if (nodes[i].kind == INLAY_UI_TEXT && nodes[i].text.length >= plen &&
                std::memcmp(nodes[i].text.chars, prefix, plen) == 0)
            {
                return nodes[i].text;
            }
        }
        return {};
    };
    auto str_is = [](EtchStr a, const char *lit) {
        const size_t n = std::strlen(lit);
        return a.length == n && a.chars && std::memcmp(a.chars, lit, n) == 0;
    };

    // Initial mount: both A and B mounted -> memoRuns=2, fxRuns=2, fxCleanups=0
    ok &= check(str_is(find_text("A:"), "A:0:2:1") && str_is(find_text("B:"), "B:0:2:1"),
                "scoped hooks initial render values");
    step_frame();
    uint32_t cnt = 0;
    const auto *nodes = inlay_surface_nodes(s, &cnt);
    uint32_t btn_b_id = 0;
    for (uint32_t i = 0; i < cnt; ++i) {
        if (nodes[i].kind == INLAY_UI_BUTTON && str_is(nodes[i].text, "+B")) {
            uint32_t path[16] = {};
            uint32_t depth = 0;
            uint32_t cur = i;
            while (cur < cnt && depth < 16) {
                path[depth++] = nodes[cur].id;
                uint32_t parent = UINT32_MAX;
                for (uint32_t p = 0; p < cnt; ++p) {
                    for (uint32_t ch = nodes[p].first_child; ch != UINT32_MAX && ch < cnt; ch = nodes[ch].next_sibling) {
                        if (ch == cur) { parent = p; break; }
                    }
                    if (parent != UINT32_MAX) break;
                }
                cur = parent;
            }
            uint32_t eid = view.id;
            for (uint32_t d = depth; d > 0; --d) eid = etch_id_index(eid, path[d - 1]);
            btn_b_id = eid;
            break;
        }
    }
    const EtchRect bb = etch_element_bounds(btn_b_id);
    ok &= check(bb.w > 0.f && bb.h > 0.f, "+B button bounds valid");
    const EtchVec2 bp = {bb.x + bb.w * 0.5f, bb.y + bb.h * 0.5f};
    step_frame(bp, true);
    step_frame(bp, false);
    ok &= check(str_is(find_text("A:"), "A:0:2:2") && str_is(find_text("B:"), "B:1:2:2"),
                "scoped useState click increments only B");
    ok &= check(str_is(find_text("M="), "M=2,E=2,C=0,S=1"),
                "useMemo/useCallback/useEffect stable when dep unchanged");

    // Hide A: A unmounts -> A's useEffect cleanup runs (C=1), B preserves state B:1!
    ok &= check(inlay_surface_dispatch(s, ETCH_STR_LIT("hideA"), ETCH_STR_LIT("null")), "hide A");
    ok &= check(find_text("A:").length == 0 && str_is(find_text("B:"), "B:1:2:3"),
                "hiding A preserves B state cleanly");
    ok &= check(inlay_surface_dispatch(s, ETCH_STR_LIT("noop"), ETCH_STR_LIT("null")), "noop render after hideA");
    ok &= check(str_is(find_text("M="), "M=2,E=2,C=1,S=3"),
                "unmount cleanup ran exactly once and callback identity held");

    // Remount A + swap order: A starts fresh at 0 while B stays at 1!
    ok &= check(inlay_surface_dispatch(s, ETCH_STR_LIT("showA"), ETCH_STR_LIT("null")), "show A");
    ok &= check(inlay_surface_dispatch(s, ETCH_STR_LIT("swap"), ETCH_STR_LIT("null")), "swap A and B");
    ok &= check(str_is(find_text("A:"), "A:0:2:2") && str_is(find_text("B:"), "B:1:2:6"),
                "keyed swap keeps B=1 and fresh A=0 isolated");

    // Change dep 1 -> 5: both useMemo & useEffect re-run with cleanups first!
    ok &= check(inlay_surface_dispatch(s, ETCH_STR_LIT("dep"), ETCH_STR_LIT("5")), "change dep to 5");
    ok &= check(str_is(find_text("A:"), "A:0:10:3") && str_is(find_text("B:"), "B:1:10:7"),
                "useMemo recomputed on dep change");
    ok &= check(inlay_surface_dispatch(s, ETCH_STR_LIT("noop"), ETCH_STR_LIT("null")), "observe post-dep counters");
    ok &= check(str_is(find_text("M="), "M=5,E=5,C=3,S=7"),
                "useEffect cleanup ran before re-run on dep change");

    // 2. Live `dark:` Tailwind prefix with 0 JS ticks across Light <-> Dark theme switches
    ok &= check(inlay_surface_set_state_json(s, ETCH_STR_LIT("{\"n\":0}")), "dark cls state");
    ok &= check(inlay_surface_set_script(s, ETCH_STR_LIT(
        "function render(st) {"
        "  return <Box cls='w-40 h-20 p-3 rounded-lg bg-[#FFFFFF] dark:bg-[#112233] border border-[#DDDDDD] dark:border-[#445566]'>"
        "    <Text cls='text-[#111111] dark:text-[#EEEEEE]'>{'Mode ' + st.n}</Text>"
        "  </Box>;"
        "}")),
        "dark: cls script");
    const uint64_t ticks_before_theme = inlay_surface_stats(s).js_ticks;
    auto has_rect_color = [&](uint32_t expected_rgba8) -> bool {
        const uint32_t pcnt = etch_frame_prim_count(ctx);
        for (uint32_t i = 0; i < pcnt; ++i) {
            EtchPrim p = {};
            if (etch_get_frame_prim(ctx, i, &p) &&
                (p.style.x & 15u) == uint32_t(ETCH_PRIM_RECT) && p.style.y == expected_rgba8)
            {
                return true;
            }
        }
        return false;
    };
    const EtchTheme prev_theme = etch_get_theme(ctx);
    const EtchTheme light_t = etch_theme_light();
    const EtchTheme dark_t = etch_theme_dark();
    etch_set_theme(&light_t, ctx);
    step_frame();
    ok &= check(has_rect_color(etch_color_pack_rgba8(etch_rgb_hex(0xFFFFFFu, 255u))),
                "light theme uses base bg-[#FFFFFF]");
    etch_set_theme(&dark_t, ctx);
    step_frame();
    ok &= check(has_rect_color(etch_color_pack_rgba8(etch_rgb_hex(0x112233u, 255u))),
                "dark theme switches live to dark:bg-[#112233]");
    ok &= check(inlay_surface_stats(s).js_ticks == ticks_before_theme,
                "dark: theme switch runs 0 JS ticks");
    etch_set_theme(&prev_theme, ctx);

    // 3. ClsCache stress & latency check (150 repeated Tailwind cards = 301 nodes across 15 rebuilds)
    ok &= check(inlay_surface_set_state_json(s, ETCH_STR_LIT("{\"v\":0}")), "cls cache bench state");
    ok &= check(inlay_surface_set_script(s, ETCH_STR_LIT(
        "function render(st) {"
        "  const items = [];"
        "  for (let i = 0; i < 150; ++i) {"
        "    items.push(<Row key={i} cls='w-full px-3 py-2 gap-2 items-center rounded-md bg-surface dark:bg-elevated border border-subtle hover:bg-hover transition-colors'>"
        "<Text cls='text-sm font-medium text-primary dark:text-[#F8FAFC]'>{'Row ' + i + ':' + st.v}</Text>"
        "</Row>);"
        "  }"
        "  return <Col cls='w-full gap-1 p-2'>{items}</Col>;"
        "}"
        "function onEvent(n, v, st) { if (n === 'bump') st.v++; }")),
        "150-row cls cache script");
    double rebuild_ms[15] = {};
    for (int i = 0; i < 15; ++i) {
        ok &= check(inlay_surface_dispatch(s, ETCH_STR_LIT("bump"), ETCH_STR_LIT("null")), "cls bench bump");
        rebuild_ms[i] = inlay_surface_stats(s).last_tick_ms;
    }
    for (int i = 1; i < 15; ++i)
        for (int j = i; j > 0 && rebuild_ms[j] < rebuild_ms[j - 1]; --j) {
            const double tmp = rebuild_ms[j]; rebuild_ms[j] = rebuild_ms[j - 1]; rebuild_ms[j - 1] = tmp;
        }
    std::printf("Cached 150-row (301-node) cls rebuild median: %.3fms\n", rebuild_ms[7]);
    ok &= check(inlay_surface_stats(s).node_count == 301u && rebuild_ms[7] < 3.0,
                "cached 150-row cls rebuild stays fast and complete");

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
    ok &= v013_jsx_transpiler(context, functional_desc);
    ok &= v013_cls_colors_and_themes(context, functional_desc);
    ok &= v013_transitions_zero_idle_ticks(context, functional_desc);
    ok &= v013_bugfix_regressions(context, functional_desc);
    ok &= v013_extended_widgets_and_etch028(context, functional_desc);
    ok &= v013_review_defect_regressions(context, functional_desc);
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
    ok &= test_scoped_hooks_cls_cache_and_dark(context);
    inlay_surface_destroy(surface);
    etch_destroy_context(context);
    std::puts(ok ? "Sandbox reactive/HMR PASS" : "Sandbox reactive/HMR FAIL");
    return ok ? 0 : 1;
}
