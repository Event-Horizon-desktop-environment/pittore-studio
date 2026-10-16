Name:           pittore-studio
Version:        0.1.2
Release:        1%{?dist}
Summary:        Layer-based photo editor for Linux
License:        MIT
URL:            https://github.com/arrifat346afs/pittore-studio
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  gcc-c++ meson ninja-build git pkgconf
BuildRequires:  zlib-ng-compat-devel harfbuzz-devel freetype-devel fontconfig-devel
BuildRequires:  tomlplusplus-devel lcms2-devel
BuildRequires:  qt6-qtbase-devel qt6-qtsvg-devel
BuildRequires:  libwebp-devel libtiff-devel libjpeg-turbo-devel libzstd-devel qrencode-devel
Requires:       qt6-qtbase qt6-qtsvg zlib-ng harfbuzz freetype fontconfig lcms2
Requires:       tomlplusplus
Recommends:     hip-runtime-amd

%description
Raster painting and retouching, vector persona, text, adjustment
layers, filters, and an ICC-aware colour pipeline.

%prep
%autosetup -n %{name}-%{version}

%build
meson setup build --prefix=/usr -Dbuildtype=release \
  -Dbackend-cuda=enabled -Dcuda-archs=sm_75,sm_80,sm_86,sm_89,sm_90,sm_100,sm_120,+ptx \
  -Dbackend-hip=enabled -Dhip-archs=gfx906,gfx908,gfx90a,gfx942,gfx950,gfx1010,gfx1012,gfx1030,gfx1031,gfx1032,gfx1100,gfx1101,gfx1102,gfx1103,gfx1150,gfx1151,gfx1200,gfx1201 \
  -Donnxruntime=disabled -Dapp=enabled
ninja -C build src/app/painter src/app/pittore-mcp src/engine/compute/libpittore-hip.so

%install
DESTDIR=%{buildroot} meson install -C build --no-rebuild

%files
%{_bindir}/painter
%{_bindir}/pittore-mcp
%{_libdir}/pittore/libpittore-hip.so
%{_datadir}/applications/studio.pittore.painter.desktop
%{_datadir}/metainfo/studio.pittore.painter.metainfo.xml
%{_datadir}/mime/packages/painter.xml
%{_datadir}/icons/hicolor/scalable/apps/painter.svg

%changelog
