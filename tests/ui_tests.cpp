#include <inlay/inlay_c.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_iostream.h>
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
    const InlaySurfaceViewDesc view = {etch_id("reactive-controls"), 300, 200};
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
    const InlaySurfaceViewDesc view = {etch_id("styled-surface"), 400, 300};
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
