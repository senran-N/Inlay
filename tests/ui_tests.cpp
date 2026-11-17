#include <inlay/inlay_c.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_iostream.h>
#include <cstdio>
#include <cstring>
namespace {
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
    bool ok = true;
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
                        40,
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
