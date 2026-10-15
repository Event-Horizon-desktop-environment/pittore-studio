# src/engine/io/meson.build
# File-format codecs (ZIP, PSD, XCF, WebP, TIFF, JPEG, Affinity) — pure C++ over
# byte buffers, no toolkit dependency besides zlib (raw deflate for ZIP and PNG,
# and the Affinity preview decode) and, when present on the system, libwebp,
# libtiff, libjpeg and libzstd. WebP/TIFF/JPEG/ZSTD are optional: the parity
# guards (#ifdef PITTORE_WEBP / PITTORE_TIFF / PITTORE_JPEG) are what keep
# callers honest on systems without them. The Affinity (.af) codec needs only
# zlib for probe + preview decode; PITTORE_AF additionally enables the zstd
# container entries (af.cpp's frame walk and af_layers' layered decode). libjpeg
# is used only by af_layers, to decode the embedded JPEG originals behind
# source-backed placed images. af_layers is kept in its own translation unit so
# the two codecs stay fully separate.

zlib_dep = dependency('zlib', required : true)
webp_dep = dependency('libwebp', required : false)
tiff_dep = dependency('libtiff-4', required : false)
jpeg_dep = dependency('libjpeg', required : false)
zstd_dep = dependency('libzstd', required : false)

io_sources = files('zip.cpp', 'psd.cpp', 'xcf.cpp', 'icc_scan.cpp',
                   'project.cpp',
                   'af/shared/af_preview.cpp',
                   'af/png/af_png.cpp',
                   'af/meta/af_meta.cpp',
                   'af/doc/af_document.cpp',
                   'af_layers/container/af_archive.cpp',
                   'af_layers/emit/af_emit.cpp',
                   'af_layers/emit/af_write.cpp',
                   'af_layers/graph/af_graph.cpp',
                   'af_layers/graph/af_values.cpp',
                   'af_layers/image/af_image.cpp',
                   'af_layers/image/af_tiles.cpp',
                   'af_layers/image/af_raster.cpp',
                   'af_layers/geom/af_geom.cpp',
                   'af_layers/geom/af_homography.cpp',
                   'af_layers/filter/af_distort.cpp',
                   'af_layers/filter/af_blur.cpp',
                   'af_layers/filter/af_live.cpp',
                   'af_layers/filter/af_fx.cpp',
                   'af_layers/vector/af_shapes.cpp',
                   'af_layers/walker/af_walker_decode.cpp',
                   'af_layers/walker/af_walker_emit.cpp',
                   'af_layers/walker/af_document.cpp',
                   'af_layers/pages/af_pages.cpp')
io_deps = [zlib_dep, pittore_render_dep, pittore_vector_dep, pittore_text_dep,
         pittore_color_dep]
io_feature_args = []

if webp_dep.found()
  io_sources += files('webp.cpp')
  io_deps += webp_dep
  io_feature_args += '-DPITTORE_WEBP'
endif

if tiff_dep.found()
  io_sources += files('tiff.cpp')
  io_deps += tiff_dep
  io_feature_args += '-DPITTORE_TIFF'
endif

if jpeg_dep.found()
  io_deps += jpeg_dep
  io_feature_args += '-DPITTORE_JPEG'
endif

if zstd_dep.found()
  io_deps += zstd_dep
  io_feature_args += '-DPITTORE_AF'
endif

pittore_io = static_library('pittore-io',
  io_sources,
  include_directories : src_inc,
  dependencies : io_deps,
  cpp_args : io_feature_args,
  install : false)

pittore_io_dep = declare_dependency(
  link_with : pittore_io,
  include_directories : src_inc,
  dependencies : io_deps,
  compile_args : io_feature_args)