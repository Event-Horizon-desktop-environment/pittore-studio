# Refine Lab — a live tuning harness over the real segmentation + refine
# pipeline. Links the shipped selection_mask.cpp and the AI backend so what
# you tune here is exactly what the app runs. Not part of the test suite;
# build with `ninja -C build refine-lab` and run `./build/tests/Refine-app/refine-lab`.

refine_lab_args = ['-fPIC']
if pittore_onnx_enabled
  refine_lab_args += '-DPITTORE_HAS_ONNX=1'
endif

executable('refine-lab',
  [
    'main.cpp',
    'lab_engine.cpp',
    'lab_mainwindow.cpp',
    'mcp_server.cpp',
  ],
  dependencies : [pittore_ui_dep, pittore_compute_dep, pittore_ai_dep, qt6dep],
  include_directories : [src_inc, include_directories('.')],
  cpp_args : refine_lab_args,
  install : false)
