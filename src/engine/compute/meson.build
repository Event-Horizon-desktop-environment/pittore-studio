# src/engine/compute/meson.build
# The compute engine: interface + CPU reference backend always; CUDA and HIP
# backends joined in when (and only when) their toolchains are present.

fs = import('fs')
cc = meson.get_compiler('cpp')

compute_sources = files(
  'blend.cpp',
  'cpu_backend.cpp',
  'layer_mask.cpp',
  'warp.cpp',
  'factory.cpp',
  'brushes/dab/dab.cpp',
  'brushes/erase/erase.cpp',
  'brushes/stamp/stamp.cpp',
  'brushes/smudge/smudge.cpp',
  'brushes/loaders/loaders.cpp',
  'brushes/tone/tone.cpp',
  'brushes/flood/flood.cpp',
  'brushes/replace/replace.cpp',
  'brushes/heal/heal.cpp',
  'brushes/clone/clone.cpp',
  'brushes/composite/composite.cpp',
  'tone_blend.cpp',
)
compute_args = []
compute_deps = []

# --- CUDA backend (NVIDIA) ------------------------------------------------
# Meson's cuda language hunts for nvcc on PATH only (no env var, no extra
# search dirs), while toolkit installers put it in /usr/local/cuda/bin
# without touching the shell profile. Locate nvcc ourselves — the standard
# install roots, or an explicit -Dcuda-root — and compile the kernels through
# a custom_target like the HIP backend below. The object still archives into
# libpittore-compute, and builds work off-PATH (or on GPU-less CI via
# -Dcuda-archs).
cuda_feature = get_option('backend-cuda')
pittore_cuda_enabled = false
if cuda_feature.allowed()
  cuda_root = get_option('cuda-root')
  if cuda_root != ''
    nvcc = find_program(join_paths(cuda_root, 'bin', 'nvcc'),
                        required : cuda_feature.enabled())
  else
    nvcc = find_program('nvcc',
                        dirs : ['/usr/local/cuda/bin', '/opt/cuda/bin',
                                '/usr/lib/cuda/bin'],
                        required : cuda_feature.enabled())
  endif
  if nvcc.found()
    if cuda_root == ''
      cuda_root = fs.parent(fs.parent(nvcc.full_path()))
    endif
    compute_args += '-DPITTORE_HAS_CUDA=1'
    pittore_cuda_enabled = true

    cuda_lib_dir = join_paths(cuda_root, 'lib64')
    # Static runtime: NVIDIA's toolkit EULA (Attachment A) lists
    # libcudart_static.a as redistributable, so release binaries carry GPU
    # support while users need only the driver (libcuda.so) installed. It
    # also drops the rpath into the build machine's toolkit, which must
    # never ship in a release artifact.
    cudart = cc.find_library('cudart_static', dirs : [cuda_lib_dir], required : true)
    compute_deps += declare_dependency(dependencies : cudart)

    # Which GPUs the shipped kernels run on. "native" probes the build
    # machine's GPU (needs a driver, so dev machines only — nvcc otherwise
    # defaults to sm_75, which the driver JIT-compiles on first launch:
    # slower code plus a compile stall). GPU-less builders (CI) pass an
    # explicit fat SASS list plus PTX for the newest arch so newer cards
    # still work via the driver's JIT. Native SASS also lets ptxas schedule
    # for Blackwell and friends.
    cuda_arch_flags = []
    cuda_archs_opt = get_option('cuda-archs')
    if cuda_archs_opt == '' or cuda_archs_opt == 'native'
      cuda_arch_flags += '-arch=native'
    else
      max_sm = ''
      foreach a : cuda_archs_opt.split(',')
        a = a.strip()
        if a == '' or a == '+ptx'
          continue
        endif
        if a.startswith('sm_')
          sm = a.substring(3)
          cuda_arch_flags += '-gencode=arch=compute_' + sm + ',code=sm_' + sm
          max_sm = sm
        elif a.startswith('compute_')
          cuda_arch_flags += '-gencode=arch=' + a + ',code=' + a
          max_sm = a.substring(8)
        else
          error('pittore-studio: bad -Dcuda-archs entry "' + a + '" (want sm_NN, compute_NN or +ptx)')
        endif
      endforeach
      if '+ptx' in cuda_archs_opt and max_sm != ''
        cuda_arch_flags += '-gencode=arch=compute_' + max_sm + ',code=compute_' + max_sm
      endif
    endif

    # nvcc-safe spellings of meson's build options, mirroring how the C++
    # TUs are built: the optimization levels translated for nvcc (device
    # debug -G at 0, -lineinfo with -O2), -g for debug info, and -DNDEBUG
    # per b_ndebug with the same if-release rule meson applies to gcc.
    cuda_build_args = []
    if get_option('optimization') == 'plain'
      cuda_build_args = []
    elif get_option('optimization') == '0'
      cuda_build_args = ['-G']
    elif get_option('optimization') == 'g'
      cuda_build_args = ['-O0']
    elif get_option('optimization') == '1'
      cuda_build_args = ['-O1']
    elif get_option('optimization') == '2'
      cuda_build_args = ['-O2', '-lineinfo']
    else
      # '3', 's' and 'fast' all land on nvcc's top level.
      cuda_build_args = ['-O3']
    endif
    if get_option('debug')
      cuda_build_args += '-g'
    endif
    if get_option('b_ndebug') == 'true' \
        or (get_option('b_ndebug') == 'if-release'
            and get_option('buildtype') in ['release', 'plain'])
      cuda_build_args += '-DNDEBUG'
    endif

    # Header deps: nvcc -MD writes absolute paths (ninja reads them fine),
    # and -MF lands exactly on the depfile ninja expects for this target.
    cuda_obj = custom_target('cuda_backend_obj',
      input : files('cuda_backend.cu'),
      output : 'cuda_backend.o',
      command : [nvcc, '-c',
                 '-std=c++20',
                 '--expt-relaxed-constexpr',
                 '-DPITTORE_HAS_CUDA=1',
                 '-I' + meson.project_source_root() + '/src',
                 '-Xcompiler=-fPIC',
                 ] + cuda_build_args + cuda_arch_flags + [
                 '-MD', '-MT', '@OUTPUT@',
                 '-MF', join_paths(meson.current_build_dir(), 'cuda_backend.d'),
                 '@INPUT@', '-o', '@OUTPUT@'],
      depfile : 'cuda_backend.d',
      build_by_default : true)
    compute_sources += cuda_obj
    message('pittore-studio: CUDA backend enabled  (toolkit @ ' + cuda_root + ')')
  endif
else
  message('pittore-studio: CUDA backend disabled')
endif

# --- HIP backend (AMD) ------------------------------------------------------
# Meson has no "hip" language (add_languages('hip') errors on 1.11+), so the
# kernels + runtime are compiled directly by hipcc through a custom_target.
# The resulting object is archived into libpittore-compute, so every consumer
# (app, test_hip, test-all) picks it up through pittore_compute_dep.
hip_feature = get_option('backend-hip')
pittore_hip_enabled = false
if hip_feature.allowed()
  hip_root = get_option('hip-root')
  if hip_root != ''
    hipcc = find_program(join_paths(hip_root, 'bin', 'hipcc'),
                         required : hip_feature.enabled())
  else
    hipcc = find_program('hipcc', required : hip_feature.enabled())
  endif
  if hipcc.found()
    if hip_root == ''
      hip_root = fs.parent(fs.parent(hipcc.full_path()))
    endif
    hip_obj = custom_target('hip_backend_obj',
      input : files('hip_backend.cpp'),
      output : 'hip_backend.hip.o',
      command : [hipcc, '-c',
                 '-DPITTORE_HAS_HIP=1',
                 '-std=c++20',
                 '-I' + meson.project_source_root() + '/src',
                 '-I' + join_paths(hip_root, 'include'),
                 '@INPUT@',
                 '-o', '@OUTPUT@'],
      build_by_default : true)
    compute_sources += hip_obj
    compute_args += '-DPITTORE_HAS_HIP=1'
    pittore_hip_enabled = true

    hip_lib_dir = join_paths(hip_root, 'lib')
    hip_rt = cc.find_library('amdhip64', dirs : [hip_lib_dir], required : true)
    compute_deps += declare_dependency(
      dependencies : hip_rt,
      link_args : ['-Wl,-rpath,' + hip_lib_dir])
    message('pittore-studio: HIP backend enabled  (ROCm @ ' + hip_root + ')')
  endif
else
  message('pittore-studio: HIP backend disabled')
endif

pittore_compute = static_library('pittore-compute',
  compute_sources,
  include_directories : src_inc,
  cpp_args : compute_args,
  dependencies : compute_deps,
  install : false)

pittore_compute_dep = declare_dependency(
  link_with : pittore_compute,
  include_directories : src_inc,
  dependencies : compute_deps)