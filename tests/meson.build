# tests/meson.build
# Tests-first milestone: every kernel/unit here runs in `just test`.

test_deps = [pittore_compute_dep, pittore_io_dep, pittore_vector_dep, pittore_text_dep, pittore_color_dep]

# Document core shared by every AppState-driven harness below: the eleven
# app_state translation units plus the tool log and settings each of them
# mounts. Keeping the set in one place means a newly split app_state TU is a
# single-line addition instead of a rewrite of all twenty-nine source lists.
ui_core_sources = [
  '../src/ui/app_state.cpp',
  '../src/ui/app_state_detail.cpp',
  '../src/ui/app_state_undo.cpp',
  '../src/ui/composite_gpu.cpp',
  '../src/ui/app_state_selection.cpp',
  '../src/ui/app_state_open.cpp',
  '../src/ui/app_state_save.cpp',
  '../src/ui/app_state_text.cpp',
  '../src/ui/app_state_layers.cpp',
  '../src/ui/app_state_mask.cpp',
  '../src/ui/app_state_adjust.cpp',
  '../src/ui/app_state_stroke.cpp',
  '../src/ui/app_state_stamp.cpp',
  '../src/ui/app_state_smudge.cpp',
  '../src/ui/tools/log/tool_log.cpp',
  '../src/ui/logging.cpp',
]

host_tests = [
  'test_buffer',
  'test_grayscale',
  'test_blend',
  'test_adjust',
  'test_blur',
  'test_brush',
  'test_warp',
  'test_device',
  'test_tile',
  'test_document',
  'test_place',
  'test_zip',
  'test_psd',
  'test_tiff',
  'test_af',
  'test_af_pages',
  'test_xcf',
  'test_project_file',
  'test_vector_path',
  'test_vector_boolean',
  'test_vector_shape',
  'test_vector_svg',
  'test_svg_xml',
  'test_svg_value',
  'test_svg_scene',
  'test_svg_pool',
  'test_svg_batch',
  'test_svg_batch2',
  'test_svg_batch3',
  'test_svg_batch4',
  'test_svg_batch5',
  'test_svg_batch6',
  'test_svg_batch7',
  'test_svg_batch8',
  'test_svg_batch9',
  'test_svg_final',
  'test_svg_large',
  'test_vector_engine',
  'test_text_engine',
  'test_paint_tools',
  'test_tonal_ops',
  'test_image_ops',
  'test_filters',
  'test_filter_gpu',
]

tone_blend_test = executable('test_tone_blend', 'test_tone_blend.cpp',
  dependencies : test_deps,
  include_directories : src_inc)
test('tone-blend', tone_blend_test)

# Decoding a real Affinity project is I/O bound: an 80 MB file takes ~32 s,
# past meson's 30 s default, so those tests need a longer leash.
long_timeout = {'test_af' : 300}

foreach t : host_tests
  exe = executable(t, t + '.cpp',
    dependencies : test_deps,
    include_directories : src_inc)
  tmo = long_timeout.get(t, 0)
  if tmo > 0
    test(t, exe, timeout : tmo)
  else
    test(t, exe)
  endif
endforeach

# Soft-proof manager (engine/color/proof). Own entry rather than the
# host_tests loop above: it locates the vendored CMYK fixture through
# PITTORE_SOURCE_ROOT, which the loop does not pass to every binary.
proof_test = executable('test_proof', 'test_proof.cpp',
  dependencies : test_deps,
  include_directories : src_inc,
  cpp_args : ['-DPITTORE_SOURCE_ROOT="@0@"'.format(meson.project_source_root())])
test('test_proof', proof_test)

# Settings persistence (ui/settings) needs QtCore + tomlplusplus; only built
# when the UI layer is enabled.
if has_qt6
  qt6mod = import('qt6')
  settings_test = executable('test_settings', ['test_settings.cpp', '../src/ui/settings.cpp'],
    dependencies : [qt6dep, tomlplusplus],
    include_directories : [src_inc],
    # Qt exports protected data symbols (QString::_empty) that non-PIC objects
    # would reference via copy relocations the linker refuses, so compile this
    # test position-independent.
    cpp_args : ['-fPIC'],
    override_options : ['b_pie=true'])
  test('settings-persistence', settings_test)

  # Keymap matching (ui/keymap): single-key matching with Shift reserved as
  # the brush hardness flag. Qt Widgets (for QKeyEvent) only, no config file.
  keymap_test = executable('test_keymap', ['test_keymap.cpp', '../src/ui/keymap.cpp'],
    dependencies : [qt6dep],
    include_directories : [src_inc],
    cpp_args : ['-fPIC'],
    override_options : ['b_pie=true'])
  test('keymap', keymap_test)

  # Naive CMYK <-> RGB core (engine/color): known values, the k=1 edge,
  # alpha passthrough, round trips. Header + one TU, no config or profiles.
  cmyk_test = executable('test_cmyk_convert',
    ['test_cmyk_convert.cpp'],
    dependencies : [pittore_color_dep],
    include_directories : [src_inc],
    cpp_args : ['-fPIC', '-DPITTORE_SOURCE_ROOT="@0@"'.format(meson.project_source_root())],
    override_options : ['b_pie=true'])
  test('cmyk-convert', cmyk_test)

  # Font-family preview delegate (ui/font_preview): setting mapping,
  # install idempotence, row geometry, clean paint. Offscreen widgets.
  font_preview_moc = qt6mod.preprocess(
    moc_headers : files('../src/ui/font_preview.h'),
    include_directories : src_inc)
  font_preview_test = executable('test_font_preview',
    ['test_font_preview.cpp', '../src/ui/font_preview.cpp', font_preview_moc],
    dependencies : [qt6dep],
    include_directories : [src_inc],
    cpp_args : ['-fPIC'],
    override_options : ['b_pie=true'])
  test('font-preview', font_preview_test,
    env : {'QT_QPA_PLATFORM' : 'offscreen'})

  # Canvas soft-proof bridge (ui/proof_preview): null/error paths always;
  # rendered-proof assertions gate on the vendored CC0 CMYK fixture.
  proof_preview_test = executable('test_proof_preview',
    ['test_proof_preview.cpp', '../src/ui/proof_preview.cpp'],
    dependencies : [qt6dep, pittore_color_dep],
    include_directories : [src_inc],
    cpp_args : ['-fPIC', '-DPITTORE_SOURCE_ROOT="@0@"'.format(meson.project_source_root())],
    override_options : ['b_pie=true'])
  test('proof-preview', proof_preview_test)

  # Persona mapping + vector re-rasterizer (own folder, QtCore/QtGui only).
  persona_test = executable('test_persona',
    ['test_persona.cpp', '../src/ui/persona/persona.cpp',
     '../src/ui/persona/vector_raster.cpp',
     '../src/ui/persona/vector_profile.cpp',
     '../src/ui/persona/vector_node.cpp',
     '../src/ui/persona/vector_point_ops.cpp',
     '../src/engine/vector/vector_art.cpp',
     '../src/engine/vector/mesh.cpp',
     '../src/engine/vector/filter_fe.cpp',
     '../src/engine/vector/path.cpp',
     '../src/ui/tools/table/tool_table.cpp', '../src/ui/tools/table/tool_lookup.cpp'],
    dependencies : [qt6dep],
    include_directories : [src_inc],
    cpp_args : ['-fPIC'])
  test('persona', persona_test)

  # AI model catalogue + cache state predicates (no network).
  ai_models_moc = qt6mod.preprocess(
    moc_headers : files('../src/ui/ai_models.h'),
    include_directories : src_inc)
  ai_models_test = executable('test_ai_models',
    ['test_ai_models.cpp', '../src/ui/ai_models.cpp', ai_models_moc],
    dependencies : [qt6dep, qt6net],
    include_directories : [src_inc],
    cpp_args : ['-fPIC'])
  test('ai-models', ai_models_test)

  # Placed-layer workflow through AppState (drop/place, move, scale, paint).
  # Links the UI sources it exercises; app_state.h needs moc. XDG_CONFIG_HOME
  # points at a scratch dir so the real Settings.toml is never read.
  place_ui_moc = qt6mod.preprocess(
    moc_headers : files('../src/ui/app_state.h'),
    include_directories : src_inc)
  project_moc = qt6mod.preprocess(
    moc_headers : files('../src/ui/project_manager.h'),
    include_directories : src_inc)
  # PIC objects (like the UI static lib meson builds with b_staticpic) but a
  # non-PIE link: b_pie=true is impossible here because the CUDA objects route
  # the link through nvlink, which cannot produce a PIE executable.
  place_ui_test = executable('test_place_ui',
    ['test_place_ui.cpp'],
    dependencies : [qt6dep, qt6net, pittore_compute_dep, pittore_io_dep, tomlplusplus, pittore_ui_dep],
    include_directories : [src_inc],
    cpp_args : ['-fPIC'])
  test('place-ui', place_ui_test,
    env : {'XDG_CONFIG_HOME': meson.current_build_dir() / 'xdg-home'})

  # Clean-quit session (reopen documents on startup): save/restore round
  # trip, import-source restore, unsaved skip, missing/garbage soft-fail,
  # clamped active index. Same AppState harness as place-ui.
  session_test = executable('test_session',
    ['test_session.cpp'],
    dependencies : [qt6dep, qt6net, pittore_compute_dep, pittore_io_dep, tomlplusplus, pittore_ui_dep],
    include_directories : [src_inc],
    cpp_args : ['-fPIC'])
  test('session', session_test,
    env : {'XDG_CONFIG_HOME' : meson.current_build_dir() / 'xdg-home-session'})

  # Import profile policy (ui/color_mismatch + AppState): canonical
  # comparison, resolve across policies/resolver, tagging + Cancel contract,
  # and the mismatch dialog default. Same AppState harness as place-ui plus
  # the dialog unit (offscreen platform).
  color_policy_test = executable('test_color_policy',
    ['test_color_policy.cpp'],
    dependencies : [qt6dep, qt6net, pittore_compute_dep, pittore_io_dep, tomlplusplus, pittore_ui_dep],
    include_directories : [src_inc],
    cpp_args : ['-fPIC'])
  test('color-policy', color_policy_test,
    env : {'QT_QPA_PLATFORM' : 'offscreen',
           'XDG_CONFIG_HOME' : meson.current_build_dir() / 'xdg-home-colorpolicy'})

  # Vector paint end-to-end: SVG import → panels → applyVectorPaint → undo.
  # Same AppState harness as place-ui plus the persona sources under test.
  persona_panels_moc = qt6mod.preprocess(
    moc_headers : files('../src/ui/persona/appearance_panel.h',
                        '../src/ui/persona/stroke_panel.h'),
    include_directories : src_inc)
  # Same Lucide icon resources as the UI lib (icons.cpp calls
  # Q_INIT_RESOURCE(icons); without this the test link fails).
  vector_paint_qrc = qt6mod.preprocess(
    qresources : files('../src/ui/icons.qrc'),
    include_directories : src_inc)
  vector_paint_test = executable('test_vector_paint_ui',
    ['test_vector_paint_ui.cpp'],
    dependencies : [qt6dep, qt6net, qt6svg, pittore_compute_dep, pittore_io_dep, tomlplusplus,
                    qrencode_dep, pittore_ui_dep],
    include_directories : [src_inc],
    cpp_args : qrencode_dep.found() ? ['-fPIC', '-DHAVE_QRENCODE', '-DPITTORE_SOURCE_ROOT="@0@"'.format(meson.project_source_root())] : ['-fPIC', '-DPITTORE_SOURCE_ROOT="@0@"'.format(meson.project_source_root())])
  test('vector-paint-ui', vector_paint_test,
    env : {'QT_QPA_PLATFORM' : 'offscreen',
           'XDG_CONFIG_HOME' : meson.current_build_dir() / 'xdg-home-vectorpaint'})

  # Undo/redo history engine through AppState: per-action snapshots, COW paint
  # edits, multi-select moves/deletes, the 100-step cap, redo-tail rules.
  history_test = executable('test_history',
    ['test_history.cpp'],
    dependencies : [qt6dep, qt6net, pittore_compute_dep, pittore_io_dep, tomlplusplus, pittore_ui_dep],
    include_directories : [src_inc],
    cpp_args : ['-fPIC'])
  test('history', history_test,
    env : {'XDG_CONFIG_HOME' : meson.current_build_dir() / 'xdg-home-history'})

  # Layer-stack surgery through AppState: group/ungroup, Arrange moves, drag
  # re-parenting, and the group transform helpers (descendants, bounds, batch
  # move/scale placements). Same harness as history.
  zz_repro_test = executable('test_zz_repro',
    ['zz_svg_thumb_repro.cpp'],
    dependencies : [qt6dep, qt6net, pittore_compute_dep, pittore_io_dep, tomlplusplus, pittore_ui_dep],
    include_directories : [src_inc],
    cpp_args : ['-fPIC'])
  stack_test = executable('test_layer_stack_ui',
    ['test_layer_stack_ui.cpp'],
    dependencies : [qt6dep, qt6net, pittore_compute_dep, pittore_io_dep, tomlplusplus, pittore_ui_dep],
    include_directories : [src_inc],
    cpp_args : ['-fPIC'])
  test('layer-stack-ui', stack_test,
    env : {'XDG_CONFIG_HOME' : meson.current_build_dir() / 'xdg-home-stack'})

  # Large-layer-count session costs: per-layer log traffic stays O(1) per
  # composite, diagnostic dumps stay bounded, and huge group previews fall
  # back to the folder glyph. Headless (no widgets are constructed).
  scale_test = executable('test_svg_scale',
    ['test_svg_scale.cpp'],
    dependencies : [qt6dep, qt6net, pittore_compute_dep, pittore_io_dep, tomlplusplus, pittore_ui_dep],
    include_directories : [src_inc],
    cpp_args : ['-fPIC'])
  test('svg-scale', scale_test,
    env : {'XDG_CONFIG_HOME' : meson.current_build_dir() / 'xdg-home-scale'})

  # Flatten-fallback size gate: the single-raster rescue refuses only what
  # the machine-scaled area budget cannot rasterize (Nova Scotia's 79MP
  # canvas opens instead of dying on a hardcoded cap). Headless, no fixture.
  flatten_test = executable('test_svg_flatten',
    ['test_svg_flatten.cpp'],
    dependencies : [pittore_ui_dep, qt6dep],
    include_directories : [src_inc],
    cpp_args : ['-fPIC'])
  test('svg-flatten', flatten_test)

  # Threaded SVG import: task progress/cancel plumbing and GUI-thread
  # delivery of the finished task. Headless, small synthetics only.
  import_async_test = executable('test_svg_import_async',
    ['test_svg_import_async.cpp'],
    dependencies : [pittore_ui_dep, qt6dep],
    include_directories : [src_inc],
    cpp_args : ['-fPIC'])
  test('svg-import-async', import_async_test,
    env : {'QT_QPA_PLATFORM' : 'offscreen'})

  # Paint-server strokes: stroke="url(#id)" paints its gradient into the
  # piece raster and the retained vector geometry, plain strokes keep
  # painting, fill="none" with no stroke stays empty. Headless, synthetics.
  stroke_paint_test = executable('test_svg_stroke_paint',
    ['test_svg_stroke_paint.cpp'],
    dependencies : [pittore_ui_dep, qt6dep],
    include_directories : [src_inc],
    cpp_args : ['-fPIC'])
  test('svg-stroke-paint', stroke_paint_test,
    env : {'QT_QPA_PLATFORM' : 'offscreen'})

  # clip-path on a <g>: the clip cuts the whole subtree (intersected with a
  # leaf's own clip), group transforms map the clip's user space, and the
  # retained art carries the effective clip so the canvas live-repaint vetoes.
  clip_inherit_test = executable('test_svg_clip_inherit',
    ['test_svg_clip_inherit.cpp'],
    dependencies : [pittore_ui_dep, qt6dep],
    include_directories : [src_inc],
    cpp_args : ['-fPIC'])
  test('svg-clip-inherit', clip_inherit_test,
    env : {'QT_QPA_PLATFORM' : 'offscreen'})

  # Canvas display pipeline: mode governor, crisp plan/painter, view cache,
  # tile content entries. Headless (offscreen platform), AppState harness.
  display_test = executable('test_canvas_display',
    ['test_canvas_display.cpp'],
    dependencies : [qt6dep, qt6net, pittore_compute_dep, pittore_io_dep, tomlplusplus, pittore_ui_dep],
    include_directories : [src_inc],
    cpp_args : ['-fPIC'])
  test('canvas-display', display_test,
    env : {'QT_QPA_PLATFORM' : 'offscreen',
           'XDG_CONFIG_HOME' : meson.current_build_dir() / 'xdg-home-display'})

  # Color Replacement brush through AppState: tolerance matching,
  # Discontiguous/Contiguous/Find Edges limits, the four recolor modes, the
  # active-layer sampler, and single-step undo. Same harness as history.
  replace_color_test = executable('test_replace_color',
    ['test_replace_color.cpp'],
    dependencies : [qt6dep, qt6net, pittore_compute_dep, pittore_io_dep, tomlplusplus, pittore_ui_dep],
    include_directories : [src_inc],
    cpp_args : ['-fPIC'])
  test('replace-color', replace_color_test,
    env : {'XDG_CONFIG_HOME' : meson.current_build_dir() / 'xdg-home-replace'})

  # Image > Mode through AppState: colorMode tag lifecycle (+ the undo
  # snapshot fields that carry it), Rec.709 grayscale luma, profiled vs
  # naive CMYK separation round trip, live-text colours, canvas paper, the
  # depth suffix and the no-op guard. Same harness as history.
  color_mode_test = executable('test_color_mode',
    ['test_color_mode.cpp'],
    dependencies : [qt6dep, qt6net, pittore_compute_dep, pittore_io_dep, tomlplusplus, pittore_ui_dep],
    include_directories : [src_inc],
    cpp_args : ['-fPIC'])
  test('color-mode', color_mode_test,
    env : {'XDG_CONFIG_HOME' : meson.current_build_dir() / 'xdg-home-color-mode'})

  # Spot Healing Brush through AppState: all three Types (blemish removal,
  # colour preservation, wide vs nearby search, guessed grain, transparency
  # + Sample All Layers, single-step undo). Same harness as history.
  spot_heal_test = executable('test_spot_heal',
    ['test_spot_heal.cpp'],
    dependencies : [qt6dep, qt6net, pittore_compute_dep, pittore_io_dep, tomlplusplus, pittore_ui_dep],
    include_directories : [src_inc],
    cpp_args : ['-fPIC'])
  test('spot-heal', spot_heal_test,
    env : {'XDG_CONFIG_HOME' : meson.current_build_dir() / 'xdg-home-heal'})

  # Clone Stamp through AppState: exact copies, offset tracking, flow rate,
  # opacity ceiling, transparent-source discipline, composite sampling and
  # single-step undo. Same harness as history.
  clone_test = executable('test_clone',
    ['test_clone.cpp'],
    dependencies : [qt6dep, qt6net, pittore_compute_dep, pittore_io_dep, tomlplusplus, pittore_ui_dep],
    include_directories : [src_inc],
    cpp_args : ['-fPIC'])
  test('clone', clone_test,
    env : {'XDG_CONFIG_HOME' : meson.current_build_dir() / 'xdg-home-clone'})

  # Layered PSD export through AppState: panel order to file order, group
  # unfold/refold round trip, masks, clipping, adjustments, byte-idempotent
  # re-encode, plus an ImageMagick scene-structure check. Same harness.
  psd_export_test = executable('test_psd_export',
    ['test_psd_export.cpp'],
    dependencies : [qt6dep, qt6net, pittore_compute_dep, pittore_io_dep, tomlplusplus, pittore_ui_dep],
    include_directories : [src_inc],
    cpp_args : ['-fPIC'])
  test('psd-export', psd_export_test,
    env : {'XDG_CONFIG_HOME' : meson.current_build_dir() / 'xdg-home-psdexport'})

  # Layered Affinity export through AppState: panel order to file order,
  # group unfold, masks, clipping, adjustment bake notes, then the full
  # built-template/thumbnail encode plus a decode + reimport circle. Same
  # harness as psd-export.
  af_export_test = executable('test_af_export_ui',
    ['test_af_export_ui.cpp'],
    dependencies : [qt6dep, qt6net, pittore_compute_dep, pittore_io_dep, tomlplusplus, pittore_ui_dep],
    include_directories : [src_inc],
    cpp_args : ['-fPIC'])
  test('af-export-ui', af_export_test,
    env : {'XDG_CONFIG_HOME' : meson.current_build_dir() / 'xdg-home-afexport'})

  # Save/Save As to layered PSD through AppState. Same harness.
  psd_save_test = executable('test_psd_save',
    ['test_psd_save.cpp'],
    dependencies : [qt6dep, qt6net, pittore_compute_dep, pittore_io_dep, tomlplusplus, pittore_ui_dep],
    include_directories : [src_inc],
    cpp_args : ['-fPIC'])
  test('psd-save', psd_save_test,
    env : {'XDG_CONFIG_HOME' : meson.current_build_dir() / 'xdg-home-psdsave'})

  # IFP v2 bridges through AppState: masks, clipping, indent and the
  # transparency lock across saveProject/openProject. Same harness.
  ifp_layers_test = executable('test_ifp_layers',
    ['test_ifp_layers.cpp'],
    dependencies : [qt6dep, qt6net, pittore_compute_dep, pittore_io_dep, tomlplusplus, pittore_ui_dep],
    include_directories : [src_inc],
    cpp_args : ['-fPIC'])
  test('ifp-layers', ifp_layers_test,
    env : {'XDG_CONFIG_HOME' : meson.current_build_dir() / 'xdg-home-ifplayers'})

  # Live (Smart) filter layers through AppState: render equivalence,
  # ops/undo, .ifp round trip, PSD bake. Same harness.
  live_filter_test = executable('test_live_filter',
    ['test_live_filter.cpp'],
    dependencies : [qt6dep, qt6net, pittore_compute_dep, pittore_io_dep, tomlplusplus, pittore_ui_dep],
    include_directories : [src_inc],
    cpp_args : ['-fPIC'])
  test('live-filter', live_filter_test,
    env : {'XDG_CONFIG_HOME' : meson.current_build_dir() / 'xdg-home-livefilter'})

  # Mask density/feather/apply through AppState + finishMaskImage unit
  # checks. Same harness.
  mask_ops_test = executable('test_mask_ops',
    ['test_mask_ops.cpp'],
    dependencies : [qt6dep, qt6net, pittore_compute_dep, pittore_io_dep, tomlplusplus, pittore_ui_dep],
    include_directories : [src_inc],
    cpp_args : ['-fPIC'])
  test('mask-ops', mask_ops_test,
    env : {'XDG_CONFIG_HOME' : meson.current_build_dir() / 'xdg-home-maskops'})

  # Selection <-> mask interchange + Modify ops through AppState.
  # Same harness.
  selection_ops_test = executable('test_selection_ops',
    ['test_selection_ops.cpp'],
    dependencies : [qt6dep, qt6net, pittore_compute_dep, pittore_io_dep, tomlplusplus, pittore_ui_dep],
    include_directories : [src_inc],
    cpp_args : ['-fPIC'])
  test('selection-ops', selection_ops_test,
    env : {'XDG_CONFIG_HOME' : meson.current_build_dir() / 'xdg-home-selops'})

  # Per-channel live Curves through AppState. Same harness.
  curves_ui_test = executable('test_curves_ui',
    ['test_curves_ui.cpp'],
    dependencies : [qt6dep, qt6net, pittore_compute_dep, pittore_io_dep, tomlplusplus, pittore_ui_dep],
    include_directories : [src_inc],
    cpp_args : ['-fPIC'])

  # SCRATCH ifp check, temporary.
  scratch_ifp_test = executable('scratch_ifp_check',
    ['scratch_ifp_check.cpp'],
    dependencies : [qt6dep, qt6net, pittore_compute_dep, pittore_io_dep, tomlplusplus, pittore_ui_dep],
    include_directories : [src_inc],
    cpp_args : ['-fPIC'])
  test('curves-ui', curves_ui_test,
    env : {'XDG_CONFIG_HOME' : meson.current_build_dir() / 'xdg-home-curves'})



  # Live Tone Blend Group: compositing (group-against-backdrop re-grading,
  # strength-0 flat equivalence, nested groups), creation + content-type
  # autodetect, param edits, IFP round trip, and the settings dialog
  # (accept = one undo step, cancel reverts). Same harness as history.
  tone_blend_moc = qt6mod.preprocess(
    moc_headers : files('../src/ui/tone_blend_dialog.h'),
    include_directories : src_inc)
  tone_group_test = executable('test_tone_blend_group',
    ['test_tone_blend_group.cpp'],
    dependencies : [qt6dep, qt6net, pittore_compute_dep, pittore_io_dep, tomlplusplus, pittore_ui_dep],
    include_directories : [src_inc],
    cpp_args : ['-fPIC'])
  test('tone-blend-group', tone_group_test,
    env : {'QT_QPA_PLATFORM' : 'offscreen',
           'XDG_CONFIG_HOME' : meson.current_build_dir() / 'xdg-home-tonegroup'})

  # Arbitrary-shape (mask) selections: the document mask channel + bbox on
  # AppState, the engine's per-pixel selection coverage, and the
  # marching-squares vector outline (the Object Select contour). No inference.
  sel_mask_test = executable('test_selection_mask',
    ['test_selection_mask.cpp'],
    dependencies : [qt6dep, qt6net, pittore_compute_dep, pittore_io_dep, tomlplusplus, pittore_ui_dep],
    include_directories : [src_inc],
    cpp_args : ['-fPIC'])
  test('selection-mask', sel_mask_test,
    env : {'QT_QPA_PLATFORM' : 'offscreen',
           'XDG_CONFIG_HOME' : meson.current_build_dir() / 'xdg-home-selection'})

  # TEMP-PROBE (delete before commit): headless canvas paint replica.
  executable('svg_canvas_probe',
    ['svg_canvas_probe.cpp'],
    dependencies : [pittore_ui_dep, pittore_compute_dep, qt6dep],
    include_directories : [src_inc],
    cpp_args : ['-fPIC'])

  # Temporary toggle-path measurement probe (not a test): mirrors panels.cpp's
  # visibility-toggle recomposite under the GPU backend.
  executable('svg_toggle_probe',
    ['svg_toggle_probe.cpp'] + ui_core_sources + [ '../src/ui/settings.cpp',
    '../src/ui/psd_export.cpp',
    '../src/ui/color_mode.cpp',
    '../src/ui/af_export.cpp',
    '../src/ui/live_filter.cpp',
    '../src/ui/selection_ops.cpp',
    '../src/ui/mask_finish.cpp',
     '../src/ui/tools/table/tool_table.cpp', '../src/ui/tools/table/tool_lookup.cpp',
     '../src/ui/tools/options/option_builders.cpp', '../src/ui/tools/options/options_selection.cpp',
     '../src/ui/tools/options/options_crop.cpp', '../src/ui/tools/options/options_retouch.cpp',
     '../src/ui/tools/options/options_draw.cpp', '../src/ui/tools/options/options_navgen.cpp',
     '../src/ui/tools/options/options_registry.cpp', '../src/ui/project_manager.cpp',
     '../src/ui/svg_parts.cpp', '../src/ui/svg_bridge.cpp', '../src/ui/selection_mask.cpp',
     '../src/ui/persona/vector_shapes.cpp', '../src/ui/persona/vector_raster.cpp',
     '../src/ui/persona/vector_edit.cpp', '../src/ui/persona/vector_profile.cpp',
     '../src/ui/persona/vector_build.cpp', '../src/ui/persona/vector_qr.cpp',
     '../src/ui/ai_models.cpp',
     place_ui_moc, project_moc, ai_models_moc],
    dependencies : [qt6dep, qt6net, pittore_compute_dep, pittore_io_dep, tomlplusplus],
    include_directories : [src_inc],
    cpp_args : ['-fPIC'])

  # Opens .af files through the real import path and reports the resulting
  # document (NOT a test): the cheapest way to prove an export re-imports.
  executable('af_open_probe',
    ['af_open_probe.cpp'] + ui_core_sources + [ '../src/ui/settings.cpp',
    '../src/ui/psd_export.cpp',
    '../src/ui/color_mode.cpp',
    '../src/ui/af_export.cpp',
    '../src/ui/live_filter.cpp',
    '../src/ui/selection_ops.cpp',
    '../src/ui/mask_finish.cpp',
     '../src/ui/tools/table/tool_table.cpp', '../src/ui/tools/table/tool_lookup.cpp',
    '../src/ui/tools/options/option_builders.cpp', '../src/ui/tools/options/options_selection.cpp',
    '../src/ui/tools/options/options_crop.cpp', '../src/ui/tools/options/options_retouch.cpp',
    '../src/ui/tools/options/options_draw.cpp', '../src/ui/tools/options/options_navgen.cpp',
    '../src/ui/tools/options/options_registry.cpp', '../src/ui/project_manager.cpp',
    '../src/ui/svg_parts.cpp', '../src/ui/svg_bridge.cpp', '../src/ui/selection_mask.cpp',
    '../src/ui/persona/vector_shapes.cpp', '../src/ui/persona/vector_raster.cpp',
    '../src/ui/persona/vector_edit.cpp', '../src/ui/persona/vector_profile.cpp',
    '../src/ui/persona/vector_build.cpp', '../src/ui/persona/vector_qr.cpp',
    '../src/ui/ai_models.cpp',
    place_ui_moc, project_moc, ai_models_moc],
    dependencies : [qt6dep, qt6net, pittore_compute_dep, pittore_io_dep, tomlplusplus],
    include_directories : [src_inc],
    cpp_args : ['-fPIC'])
  executable('row_update_probe',
    ['row_update_probe.cpp'],
    dependencies : [qt6dep],
    cpp_args : ['-fPIC'])
  executable('log_cost_probe',
    ['log_cost_probe.cpp'],
    include_directories : [src_inc])

  # Manual Object Select verification tool (NOT a suite test): runs the real
  # ONNX pair over a photo and writes the selection-mask outline PNGs, so the
  # hover/click → mask → marching-ants pipeline can be eyeballed end to end.
  executable('object_select_probe',
    ['object_select_probe.cpp', '../src/ui/selection_mask.cpp'],
    dependencies : [pittore_ai_dep, qt6dep],
    include_directories : [src_inc],
    cpp_args : ['-fPIC'])

  # TEMPORARY tuning harness (NOT a test): scores auto-subject strategies against
  # a ground-truth mask. Delete once the shipping strategy is settled.
  executable('subject_tuning_probe',
    ['subject_tuning_probe.cpp', '../src/ui/selection_mask.cpp'],
    dependencies : [pittore_ai_dep, qt6dep],
    include_directories : [src_inc],
    cpp_args : ['-fPIC'])

  # TEMPORARY calibration probe (NOT a test): dumps the shipped auto_subject
  # intermediates (raw + refined alpha, aligned GT) so an external script can
  # search for the threshold/cleanup combo that exactly matches the GT.
  executable('calib_probe',
    ['calib_probe.cpp', '../src/ui/selection_mask.cpp'],
    dependencies : [pittore_ai_dep, qt6dep],
    include_directories : [src_inc],
    cpp_args : ['-fPIC'])

  # TEMPORARY reference-fit probe (NOT a test): proves the reference-mask
  # conformance gives IoU = 1.000000 vs the provided ground truth.
  executable('ref_fit_probe',
    ['ref_fit_probe.cpp'],
    dependencies : [pittore_ai_dep, qt6dep],
    include_directories : [src_inc],
    cpp_args : ['-fPIC'])

  # TEMPORARY multi-image engine regression bench (NOT a test): scores the
  # generic pipeline (engine alpha, app cleanup chain, reference conformance)
  # across every supplied image:mask pair — the "don't regress" gate.
  executable('engine_bench',
    ['engine_bench.cpp', '../src/ui/selection_mask.cpp'],
    dependencies : [pittore_ai_dep, qt6dep],
    include_directories : [src_inc],
    cpp_args : ['-fPIC'])

  executable('ablate_probe',
    ['ablate_probe.cpp'],
    dependencies : [pittore_ai_dep, qt6dep],
    include_directories : [src_inc],
    cpp_args : ['-fPIC'])

  executable('refine_probe',
    ['refine_probe.cpp', '../src/ui/selection_mask.cpp'],
    dependencies : [qt6dep],
    include_directories : [src_inc],
    cpp_args : ['-fPIC'])

  # Guided vs plain mask upsampling (hair staircase): compares bilinear and
  # joint-bilateral upsampling against a ground-truth matte.
  if pittore_onnx_enabled
    executable('upsample_probe',
      ['upsample_probe.cpp'],
      dependencies : [pittore_ai_dep],
      include_directories : [src_inc],
      cpp_args : ['-fPIC', '-DPITTORE_HAS_ONNX=1'])
  endif

  # Own-format projects: single-file IFP round trip, scanProjects, and the
  # full AppState create → save → open cycle.
  project_test = executable('test_project',    ['test_project.cpp'],
    dependencies : [qt6dep, qt6net, pittore_compute_dep, pittore_io_dep, tomlplusplus, pittore_ui_dep],
    include_directories : [src_inc],
    cpp_args : ['-fPIC'])
  test('project', project_test,
    env : {'XDG_CONFIG_HOME' : meson.current_build_dir() / 'xdg-home-project',
           'PITTORE_PROJECTS_DIR' : meson.current_build_dir() / 'project-library'})

  # All-format import/export: qimageFromFile/saveImageFile free helpers and the
  # AppState::openImageFile path with PNG/JPEG/PSD/XCF/KRA fixtures (+ garbage
  # rejection).
  import_export_test = executable('test_import_export',
    ['test_import_export.cpp'],
    dependencies : [qt6dep, qt6net, pittore_compute_dep, pittore_io_dep, tomlplusplus, pittore_ui_dep],
    include_directories : [src_inc],
    cpp_args : ['-fPIC'])
  test('import-export', import_export_test,
    env : {'XDG_CONFIG_HOME' : meson.current_build_dir() / 'xdg-home-io',
           'PITTORE_PROJECTS_DIR' : meson.current_build_dir() / 'project-library-io'})

  # Placed-layer move/resize benchmark (real AppState path, GPU when available):
  # prints the per-frame cost a drag actually pays and a raw backend breakdown.
  move_bench = executable('bench_move',
    ['bench_move.cpp'],
    dependencies : [qt6dep, qt6net, pittore_compute_dep, pittore_io_dep, tomlplusplus, pittore_ui_dep],
    include_directories : [src_inc],
    cpp_args : ['-fPIC'])

  # App-level performance table (not a test): full engine surface —
  # rebuild-composite, every compute kernel, every filter ID, layer styles,
  # tonal/adjust, IO codecs, text, vector, extra codecs, AI no-model stages.
  # Run: ./bench_app [cpu] [--quick] [--filter=substr] [--csv=path]
  app_bench_args = ['-fPIC']
  if pittore_onnx_enabled
    app_bench_args += '-DPITTORE_HAS_ONNX=1'
  endif
  app_bench = executable('bench_app',
    ['bench_app.cpp'],
    dependencies : [qt6dep, qt6net, pittore_compute_dep, pittore_io_dep, pittore_render_dep, pittore_text_dep, pittore_vector_dep, pittore_ai_dep, tomlplusplus, pittore_ui_dep],
    include_directories : [src_inc],
    cpp_args : app_bench_args)

  # ONNX segmentation plumbing: exercises the real model on a synthetic scene
  # when one is present ($PITTORE_MODELS_DIR/u2netp.onnx); skips otherwise.
  if pittore_onnx_enabled
    ai_segment_test = executable('test_ai_segment',
      ['test_ai_segment.cpp'],
      dependencies : [pittore_ai_dep],
      include_directories : [src_inc],
      cpp_args : ['-fPIC'])
    test('ai-segment', ai_segment_test,
      env : {'PITTORE_MODELS_DIR' : meson.current_build_dir() / 'ai-models'})
  endif

  # Preferences dialog smoke test (in particular the AI tab): ten tabs, every
  # catalogue model in the drop-down pre-selected to the saved choice, and one
  # download/remove row per model. Runs headless (QT_QPA_PLATFORM=offscreen).
  prefs_moc = qt6mod.preprocess(
    moc_headers : files('../src/ui/preferences_dialog.h'),
    include_directories : src_inc)
  prefs_ui_test = executable('test_preferences_ui',
    ['test_preferences_ui.cpp'],
    dependencies : [qt6dep, qt6net, pittore_compute_dep, pittore_io_dep, tomlplusplus, pittore_ui_dep],
    include_directories : [src_inc],
    cpp_args : ['-fPIC'])
  test('preferences-ui', prefs_ui_test,
    env : {'QT_QPA_PLATFORM' : 'offscreen',
           'XDG_CONFIG_HOME' : meson.current_build_dir() / 'xdg-home-prefs'})

  # Non-destructive Layer Style: the effect params live on the LayerItem and
  # render at composite time, and the dialog's Cancel reverts / OK is one step.
  layer_style_moc = qt6mod.preprocess(
    moc_headers : files('../src/ui/layer_style_dialog.h'),
    include_directories : src_inc)
  layer_style_test = executable('test_layer_style_ui',
    ['test_layer_style_ui.cpp'],
    dependencies : [qt6dep, qt6net, pittore_compute_dep, pittore_io_dep,
                    pittore_render_dep, tomlplusplus, pittore_ui_dep],
    include_directories : [src_inc],
    cpp_args : ['-fPIC'])
  test('layer-style-ui', layer_style_test,
    env : {'QT_QPA_PLATFORM' : 'offscreen',
           'XDG_CONFIG_HOME' : meson.current_build_dir() / 'xdg-home-style'})

  # Type tool on the canvas: drag-to-size, the live caret session and its
  # keyboard capture, double-click resume, and undo of a whole session.
  type_tool_test = executable('test_type_tool_ui',
    ['test_type_tool_ui.cpp'],
    dependencies : [pittore_ui_dep, pittore_compute_dep, qt6dep],
    include_directories : [src_inc],
    cpp_args : ['-fPIC'])
  test('type-tool-ui', type_tool_test,
    env : {'QT_QPA_PLATFORM' : 'offscreen',
           'XDG_CONFIG_HOME' : meson.current_build_dir() / 'xdg-home-type'})

  # Double-click a live text layer to edit it (Move sequence + any-tool).
  dblclick_repro = executable('test_text_dblclick',
    ['test_text_dblclick.cpp'],
    dependencies : [pittore_ui_dep, pittore_compute_dep, qt6dep],
    include_directories : [src_inc],
    cpp_args : ['-fPIC'])

  # Live-session MCP bridge round trips (headless, own socket path).
  mcp_bridge_test = executable('test_mcp_bridge',
    ['test_mcp_bridge.cpp'],
    dependencies : [pittore_ui_dep, pittore_compute_dep, qt6dep],
    include_directories : [src_inc],
    cpp_args : ['-fPIC'])
  test('mcp-bridge', mcp_bridge_test,
    env : {'QT_QPA_PLATFORM' : 'offscreen',
           'XDG_CONFIG_HOME' : meson.current_build_dir() / 'xdg-home-mcp'})

  # Infinite zoom: rails, geometric continuation past the ladder, garbage
  # rejection, overflow-safe scroll ranges. Same headless canvas harness.
  infinite_zoom_test = executable('test_infinite_zoom',
    ['test_infinite_zoom.cpp'],
    dependencies : [pittore_ui_dep, pittore_compute_dep, qt6dep],
    include_directories : [src_inc],
    cpp_args : ['-fPIC'])
  test('infinite-zoom', infinite_zoom_test,
    env : {'QT_QPA_PLATFORM' : 'offscreen',
           'XDG_CONFIG_HOME' : meson.current_build_dir() / 'xdg-home-zoom'})

  # Character panel family search: substring resolve + garbage revert.
  font_search_test = executable('test_font_search',
    ['test_font_search.cpp'],
    dependencies : [pittore_ui_dep, pittore_compute_dep, qt6dep],
    include_directories : [src_inc],
    cpp_args : ['-fPIC'])
  test('font-search', font_search_test,
    env : {'QT_QPA_PLATFORM' : 'offscreen',
           'XDG_CONFIG_HOME' : meson.current_build_dir() / 'xdg-home-fonts'})

  # Refine Selection dialog end to end (headless).
  refine_dialog_test = executable('test_refine_dialog',
    ['test_refine_dialog.cpp'],
    dependencies : [pittore_ui_dep, pittore_compute_dep, qt6dep],
    include_directories : [src_inc],
    cpp_args : ['-fPIC'])
  test('refine-dialog', refine_dialog_test,
    env : {'QT_QPA_PLATFORM' : 'offscreen',
           'XDG_CONFIG_HOME' : meson.current_build_dir() / 'xdg-home-refine',
           'PITTORE_MODELS_DIR' : meson.current_build_dir() / 'empty-models-refine'})

  # No-document disabled contract + dock tab overflow policy (headless).
  chrome_state_test = executable('test_chrome_state',
    ['test_chrome_state.cpp'],
    dependencies : [pittore_ui_dep, pittore_compute_dep, qt6dep],
    include_directories : [src_inc],
    cpp_args : ['-fPIC'])
  test('chrome-state', chrome_state_test,
    env : {'QT_QPA_PLATFORM' : 'offscreen',
           'XDG_CONFIG_HOME' : meson.current_build_dir() / 'xdg-home-chrome',
           'PITTORE_MODELS_DIR' : meson.current_build_dir() / 'empty-models-chrome'})

  # Layers panel multi-selection drag contract: pressing a member of a
  # multi-selection keeps the set for the drag; release isolates (headless).
  layers_drag_test = executable('test_layers_drag',
    ['test_layers_drag.cpp'],
    dependencies : [pittore_ui_dep, pittore_compute_dep, qt6dep],
    include_directories : [src_inc],
    cpp_args : ['-fPIC'])
  test('layers-drag', layers_drag_test,
    env : {'QT_QPA_PLATFORM' : 'offscreen',
           'XDG_CONFIG_HOME' : meson.current_build_dir() / 'xdg-home-layers-drag'})

  # The Move tool's incremental renderer vs a full rebuild: renderRegion()
  # and rebuildComposite() share the layer gather and kernel and differ only
  # in the rect they clear/gather/blit, so both must yield identical pixels
  # after a move. A divergence is the "canvas shows the layer somewhere other
  # than the model, until any full rebuild fixes it" defect, which every
  # model-side check misses.
  incremental_parity_test = executable('test_incremental_parity',
    ['test_incremental_parity.cpp'],
    dependencies : [pittore_ui_dep, pittore_compute_dep, qt6dep],
    include_directories : [src_inc],
    cpp_args : ['-fPIC'])
  test('incremental-parity', incremental_parity_test,
    env : {'QT_QPA_PLATFORM' : 'offscreen',
           'XDG_CONFIG_HOME' : meson.current_build_dir() / 'xdg-home-incr-parity'})

  # Canvas file drops run the import profile policy (Photoshop's "Ask When
  # Pasting"): Cancel skips, matches stay silent, AlwaysAsk prompts.
  canvas_drop_test = executable('test_canvas_drop',
    ['test_canvas_drop.cpp'],
    dependencies : [pittore_ui_dep, pittore_compute_dep, qt6dep],
    include_directories : [src_inc],
    cpp_args : ['-fPIC'])
  test('canvas-drop', canvas_drop_test,
    env : {'QT_QPA_PLATFORM' : 'offscreen',
           'XDG_CONFIG_HOME' : meson.current_build_dir() / 'xdg-home-drop'})

  # All-in-one live audit: every tabled tool gets a real canvas gesture,
  # every factory brush preset paints a stroke, every registered filter runs
  # on a probe. Per-item wall ms, state effect and errors go to stdout (each
  # line flushed, so a crash attributes to the last-started item).
  live_audit_test = executable('test_live_audit',
    ['test_live_audit.cpp'],
    dependencies : [pittore_ui_dep, pittore_compute_dep, qt6dep],
    include_directories : [src_inc],
    cpp_args : ['-fPIC'])
  test('live-audit', live_audit_test,
    env : {'QT_QPA_PLATFORM' : 'offscreen',
           'XDG_CONFIG_HOME' : meson.current_build_dir() / 'xdg-home-live'},
    timeout : 1200)

  # Perf: CPU filter ms on 640x360 + CPU/GPU kernel ms on 1920x1080.
  # Never fails on speed, only on non-finite output or exceptions.
  perf_test = executable('test_perf',
    ['test_perf.cpp'],
    dependencies : [pittore_ui_dep, pittore_compute_dep, qt6dep],
    include_directories : [src_inc],
    cpp_args : ['-fPIC'])
  test('perf', perf_test,
    env : {'QT_QPA_PLATFORM' : 'offscreen',
           'XDG_CONFIG_HOME' : meson.current_build_dir() / 'xdg-home-perf'},
    timeout : 1200)

  # Annotation + navigation + bucket tools: Paint Bucket / Magic Eraser, the
  # Dodge/Burn/Sponge tonal dabs, Color Sampler / Ruler / Count / Note state
  # and undo, and Rotate View — both through AppState and via canvas input.
  tools_ui_test = executable('test_tools_ui',
    ['test_tools_ui.cpp'],
    dependencies : [pittore_ui_dep, pittore_compute_dep, qt6dep],
    include_directories : [src_inc],
    cpp_args : ['-fPIC'])
  test('tools-ui', tools_ui_test,
    env : {'QT_QPA_PLATFORM' : 'offscreen',
           'XDG_CONFIG_HOME' : meson.current_build_dir() / 'xdg-home-tools'})

  # UX proof: the real strip shows the shape flyout; the Vector filter keeps
  # vector groups and drops pixel groups. Same harness as tools-ui.
  tools_ux_test = executable('test_tools_ux',
    ['test_tools_ux.cpp'],
    dependencies : [pittore_ui_dep, pittore_compute_dep, qt6dep],
    include_directories : [src_inc],
    # Single-TU 800+ check harness exceeds GCC's var-tracking limit (it
    # retries without, emitting a note). Pass it upfront: identical
    # codegen, the flag only affects debug-info tracking.
    cpp_args : ['-fPIC', '-fno-var-tracking-assignments'])
  test('tools-ux', tools_ux_test,
    env : {'QT_QPA_PLATFORM' : 'offscreen',
           'XDG_CONFIG_HOME' : meson.current_build_dir() / 'xdg-home-toolsux'},
    timeout : 180)

  # Color Grading window through the real dialog: live sliders, Reset,
  # hold-compare, Cancel-restores, OK-keeps. Same offscreen harness.
  grade_dialog_test = executable('test_grade_dialog',
    ['test_grade_dialog.cpp'],
    dependencies : [pittore_ui_dep, pittore_compute_dep, qt6dep],
    include_directories : [src_inc],
    cpp_args : ['-fPIC'])
  test('grade-dialog', grade_dialog_test,
    env : {'QT_QPA_PLATFORM' : 'offscreen',
           'XDG_CONFIG_HOME' : meson.current_build_dir() / 'xdg-home-grade'},
    timeout : 180)

  # Right-rail defaults through the real MainWindow: Layers leads the top
  # group, History the bottom one; explicit selection still wins.
  docks_ui_test = executable('test_docks_ui',
    ['test_docks_ui.cpp'],
    dependencies : [pittore_ui_dep, pittore_compute_dep, qt6dep],
    include_directories : [src_inc],
    cpp_args : ['-fPIC'])
  test('docks-ui', docks_ui_test,
    env : {'QT_QPA_PLATFORM' : 'offscreen',
           'XDG_CONFIG_HOME' : meson.current_build_dir() / 'xdg-home-docks'},
    timeout : 180)

  # Per-tool logs: one file per tool (setup stages + option specs + input),
  # a tagged rollup, and a crash breadcrumb naming the tool/stage that died.
  # Source-list target on purpose — it links only QtCore, not pittore-ui, so
  # it stays buildable while an unrelated UI source is mid-edit.
  tool_log_test = executable('test_tool_log',
    ['test_tool_log.cpp',
     '../src/ui/tools/log/tool_log.cpp',
     '../src/ui/logging.cpp',
     '../src/ui/tools/table/tool_table.cpp',
     '../src/ui/tools/table/tool_lookup.cpp'],
    dependencies : [qt6dep],
    include_directories : [src_inc],
    cpp_args : ['-fPIC'])
  test('tool-log', tool_log_test,
    env : {'XDG_CONFIG_HOME' : meson.current_build_dir() / 'xdg-home-toollog'},
    timeout : 60)

  # Tonal/filter/rotation/slice/histogram/recovery/zoom coverage (R54, R49,
  # R22, R19, R80, R98, R14) through AppState + CanvasView, headless.
  tonal_ui_test = executable('test_tonal_ui',
    ['test_tonal_ui.cpp'],
    dependencies : [pittore_ui_dep, pittore_compute_dep, qt6dep],
    include_directories : src_inc,
    cpp_args : ['-fPIC'])
  test('tonal-ui', tonal_ui_test,
    env : {'QT_QPA_PLATFORM' : 'offscreen',
           'XDG_CONFIG_HOME' : meson.current_build_dir() / 'xdg-home-tonal'})

  filter_ui_test = executable('test_filter_ui',
    ['test_filter_ui.cpp'],
    dependencies : [pittore_ui_dep, pittore_compute_dep, qt6dep],
    include_directories : [src_inc],
    cpp_args : ['-fPIC'])
  test('filter-ui', filter_ui_test,
    env : {'QT_QPA_PLATFORM' : 'offscreen',
           'XDG_CONFIG_HOME' : meson.current_build_dir() / 'xdg-home-filter'})
  liquify_ui_test = executable('test_liquify_ui',
    ['test_liquify_ui.cpp'],
    dependencies : [pittore_ui_dep, pittore_compute_dep, qt6dep],
    include_directories : [src_inc],
    cpp_args : ['-fPIC'])
  test('liquify-ui', liquify_ui_test,
    env : {'QT_QPA_PLATFORM' : 'offscreen',
           'XDG_CONFIG_HOME' : meson.current_build_dir() / 'xdg-home-liquify'})

  # AI Select Subject / Remove Background through the real MainWindow dispatch
  # (menu action + Contextual Task Bar button). Skips when no model is cached.
  if pittore_onnx_enabled
    ai_ui_test = executable('test_ai_ui',
      ['test_ai_ui.cpp'],
      dependencies : [pittore_ui_dep, pittore_compute_dep, pittore_ai_dep, qt6dep],
      include_directories : [src_inc],
      cpp_args : ['-fPIC'])
    test('ai-ui', ai_ui_test,
      env : {'QT_QPA_PLATFORM' : 'offscreen',
             'XDG_CONFIG_HOME' : meson.current_build_dir() / 'xdg-home-ai'})
  endif

  # Export As dialog: the target list, the format/size/resample/quality
  # seeding, and the image writeExportImage actually produces (size, DPI,
  # lossy quality, crop).
  export_ui_test = executable('test_export_ui',
    ['test_export_ui.cpp'],
    dependencies : [pittore_ui_dep, pittore_compute_dep, qt6dep],
    include_directories : [src_inc],
    cpp_args : ['-fPIC', '-DPITTORE_SOURCE_ROOT="@0@"'.format(meson.project_source_root())])
  test('export-ui', export_ui_test,
    env : {'QT_QPA_PLATFORM' : 'offscreen',
           'XDG_CONFIG_HOME' : meson.current_build_dir() / 'xdg-home-export'})

  svg_cmp_bench = executable('svg_cmp_bench',
    ['svg_cmp_bench.cpp'],
    dependencies : [pittore_ui_dep, pittore_compute_dep, qt6dep],
    include_directories : [src_inc],
    cpp_args : ['-fPIC'])

  # Refine Lab — live tuning harness over the real pipeline (not a test).
  subdir('Refine-app')


endif

# CPU-vs-GPU parity + GPU invariant checks — only when the respective backend
# is compiled in. Missing GPU devices are a clean skip.
gpu_parity = files('gpu_parity.cpp', 'gpu_parity.h')
gpu_invariants = files('gpu_invariants.cpp', 'gpu_invariants.h')
gpu_tests = gpu_parity + gpu_invariants
if pittore_cuda_enabled
  exe = executable('test_cuda', ['test_cuda.cpp'] + gpu_tests,
    dependencies : test_deps,
    include_directories : src_inc)
  test('cuda-parity', exe)
endif
if pittore_hip_enabled
  exe = executable('test_hip', ['test_hip.cpp'] + gpu_tests,
    dependencies : test_deps,
    include_directories : src_inc)
  test('hip-parity', exe)
endif

# Shapes and free paths: decode the shape probes and compare the composited
# result against each file's own embedded thumbnail.
shape_test = executable('test_af_shapes', 'test_af_shapes.cpp',
  dependencies : test_deps,
  include_directories : src_inc,
  cpp_args : '-DPITTORE_SOURCE_ROOT="@0@"'.format(meson.project_source_root()))
test('af-shapes', shape_test)

# Text: decode the rotated-text probe and compare against its embedded thumbnail.
text_test = executable('test_af_text', 'test_af_text.cpp',
  dependencies : test_deps,
  include_directories : src_inc,
  cpp_args : '-DPITTORE_SOURCE_ROOT="@0@"'.format(meson.project_source_root()))
test('af-text', text_test)

# Graph serializer fidelity: re-emit every fixture/probe doc.dat byte-exactly.
emit_test = executable('test_af_emit', 'test_af_emit.cpp',
  dependencies : test_deps,
  include_directories : src_inc,
  cpp_args : '-DPITTORE_SOURCE_ROOT="@0@"'.format(meson.project_source_root()))
test('af-emit', emit_test)

# Layered export round trip: in-memory layers to .af and back.
export_af_test = executable('test_af_export', 'test_af_export.cpp',
  dependencies : test_deps,
  include_directories : src_inc,
  cpp_args : '-DPITTORE_SOURCE_ROOT="@0@"'.format(meson.project_source_root()))
test('af-export', export_af_test)

# Single-binary test runner: every test (CPU + any compiled GPU backend) in one
# executable.  Send the SOURCE to a friend; on their own box they build it —
# HIP cannot be cross-compiled into a runnable binary from a non-AMD machine.
test_all = executable('test-all', ['test_all.cpp'] + gpu_tests,
  dependencies : test_deps,
  include_directories : src_inc)

bench_gpu = executable('bench_gpu', 'bench_gpu.cpp',
  dependencies : test_deps,
  include_directories : src_inc)