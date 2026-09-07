#!/usr/bin/env python3
"""Configure the external Yakuza macOS runner against this toolkit.

Only generated files in --build-dir are changed. The runner must already have
the PS3RECOMP_DIR split and macOS host support; no game assets are copied.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def replace_once(text, old, new, label):
    if text.count(old) != 1:
        raise SystemExit(f"Unsupported runner revision: expected one {label}")
    return text.replace(old, new, 1)


def write_changed(path, text):
    if not path.exists() or path.read_text() != text:
        path.write_text(text)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game-dir', type=Path, required=True)
    parser.add_argument('--build-dir', type=Path, required=True)
    args = parser.parse_args()
    toolkit = Path(__file__).resolve().parents[1]
    game = args.game_dir.resolve()
    build = args.build_dir.resolve()
    source = game / 'yakuza'
    adapter = build / 'runner-adapter'
    original_main = (source / 'main.cpp').read_text()
    original_imports = (source / 'import_overrides.cpp').read_text()

    # The legacy wrapper uses the obsolete (size, out-pointer) host signature.
    # The actual guest ABI returns the pitch in r3; r4 must never be touched.
    imports = replace_once(original_imports,
        '''    uint32_t pitch = 0;
    int32_t rc = cellGcmGetTiledPitchSize((uint32_t)ctx->gpr[3], &pitch);
    if (ctx->gpr[4]) vm_write32((uint32_t)ctx->gpr[4], pitch);
    ctx->gpr[3] = (uint64_t)(int64_t)rc;''',
        '    ctx->gpr[3] = cellGcmGetTiledPitchSize((uint32_t)ctx->gpr[3]);',
        'legacy tiled-pitch wrapper')
    imports = replace_once(imports,
        'int32_t  cellGcmGetTiledPitchSize(uint32_t size, uint32_t* pitch);',
        'uint32_t cellGcmGetTiledPitchSize(uint32_t size);',
        'legacy tiled-pitch declaration')

    # A single CFRunLoopRun may return when AppKit's nested event handling
    # stops it or when no sources remain. Do not join a still-running guest:
    # that starves Metal's dispatch_sync and CoreAudio's main-queue work.
    main_cpp = replace_once(original_main, '#include <CoreFoundation/CoreFoundation.h>',
        '#include <CoreFoundation/CoreFoundation.h>\n#include <atomic>', 'CoreFoundation include')
    main_cpp = replace_once(main_cpp, '    pthread_t game_thr;\n',
        '    static std::atomic<bool> game_done{false};\n    pthread_t game_thr;\n', 'guest thread declaration')
    main_cpp = replace_once(main_cpp, '        CFRunLoopStop(CFRunLoopGetMain());',
        '        game_done.store(true, std::memory_order_release);\n        CFRunLoopStop(CFRunLoopGetMain());', 'guest completion')
    main_cpp = replace_once(main_cpp, '    CFRunLoopRun();\n    pthread_join(game_thr, NULL);',
        '''    while (!game_done.load(std::memory_order_acquire)) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, true);
        Sleep(1);
    }
    pthread_join(game_thr, NULL);''', 'main-thread event loop')

    # This legacy runner owns its own import dispatcher, so it never calls
    # the toolkit scaffold's HLE-boundary pump. With no libgcm LLE interrupt
    # thread, deliver HLE events on its dedicated ticker instead.
    imports = replace_once(imports, '      cellGcmTickFlip();',
        """      cellGcmTickFlip();
#ifndef YZ_LLE_LIBGCM_SYS
      extern void ppu_gcm_pump(void);
      ppu_gcm_pump();
#endif""", 'HLE interrupt delivery')
    main_cpp = replace_once(main_cpp, '#include <atomic>',
        '#include <atomic>\n#include <mutex>', 'callback allocator include')
    main_cpp = replace_once(main_cpp,
        '        cb_stack = vm_stack_allocate(&g_stacks, 256 * 1024);',
        """        static std::mutex stack_lock;
        {
            std::lock_guard<std::mutex> guard(stack_lock);
            cb_stack = vm_stack_allocate(&g_stacks, 256 * 1024);
        }""", 'callback stack allocation')
    main_cpp = replace_once(main_cpp,
        '    cb_ctx.thread_id = yz_thread_current_id();',
        """    cb_ctx.thread_id = yz_thread_current_id();
    if (!g_yz_cur_ctx || !cb_ctx.thread_id) {
        static std::atomic<uint32_t> next_id{0x70000000u};
        static thread_local uint32_t interrupt_id = next_id.fetch_add(1);
        cb_ctx.thread_id = interrupt_id;
    }""", 'interrupt callback identity')
    main_cpp = replace_once(main_cpp,
        '    CreateThread(NULL, 0, yz_vblank_thread, NULL, 0, NULL);',
        '    CreateThread(NULL, 256ull * 1024 * 1024, yz_vblank_thread, NULL, 0, NULL);',
        'interrupt host stack')
    main_cpp = replace_once(main_cpp,
        '    pthread_create(&game_thr, NULL, +[](void*) -> void* {',
        """    pthread_attr_t guest_attr;
    pthread_attr_init(&guest_attr);
    if (pthread_attr_setstacksize(&guest_attr, 256ull * 1024 * 1024) != 0) {
        pthread_attr_destroy(&guest_attr);
        fprintf(stderr, "[boot] could not reserve guest host stack\\n");
        return 1;
    }
    int guest_rc = pthread_create(&game_thr, &guest_attr, +[](void*) -> void* {""",
        'guest host stack')
    main_cpp = replace_once(main_cpp, '    }, NULL);\n    while (!game_done.load',
        """    }, NULL);
    pthread_attr_destroy(&guest_attr);
    if (guest_rc != 0) {
        fprintf(stderr, "[boot] could not start guest thread\\n");
        return 1;
    }
    while (!game_done.load""", 'guest thread startup result')

    adapter.mkdir(parents=True, exist_ok=True)
    write_changed(adapter / 'main.cpp', main_cpp)
    write_changed(adapter / 'import_overrides.cpp', imports)
    # Defer until the external project's add_executable has defined its target.
    injection = '''function(ps3recomp_adapt_yakuza)
  get_target_property(runner_sources yakuza_recomp SOURCES)
  list(REMOVE_ITEM runner_sources main.cpp import_overrides.cpp)
  set_property(TARGET yakuza_recomp PROPERTY SOURCES "${runner_sources}")
  target_sources(yakuza_recomp PRIVATE
    "${CMAKE_BINARY_DIR}/runner-adapter/main.cpp"
    "${CMAKE_BINARY_DIR}/runner-adapter/import_overrides.cpp")
  target_include_directories(yakuza_recomp PRIVATE "${CMAKE_SOURCE_DIR}")
endfunction()
cmake_language(DEFER CALL ps3recomp_adapt_yakuza)
'''
    write_changed(adapter / 'attach.cmake', injection)
    write_changed(adapter / 'provenance.json', json.dumps({
        'game_dir': str(game), 'toolkit_dir': str(toolkit),
        'main_sha256': hashlib.sha256(original_main.encode()).hexdigest(),
        'imports_sha256': hashlib.sha256(original_imports.encode()).hexdigest(),
        'adaptations': ['tiled-pitch guest ABI', 'main run loop until guest completion',
                        'HLE interrupt delivery', 'guest and interrupt host stacks'],
    }, indent=2) + '\n')
    subprocess.run(['cmake', '-S', str(source), '-B', str(build), '-G', 'Ninja',
        '-DCMAKE_BUILD_TYPE=RelWithDebInfo', f'-DPS3RECOMP_DIR={toolkit}',
        '-DRECOMP_JOBS=3', f'-DCMAKE_PROJECT_YakuzaRecomp_INCLUDE={adapter / "attach.cmake"}'], check=True)


if __name__ == '__main__':
    main()
