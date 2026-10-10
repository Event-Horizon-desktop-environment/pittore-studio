Name:           pittore-studio
Version:        0.1.0
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

%description
Raster painting and retouching, vector persona, text, adjustment
layers, filters, and an ICC-aware colour pipeline.

%prep
%autosetup -n %{name}-%{version}

%build
meson setup build --prefix=/usr -Dbuildtype=release \
  -Dbackend-cuda=disabled -Dbackend-hip=disabled -Donnxruntime=disabled -Dapp=enabled
ninja -C build src/app/painter src/app/pittore-mcp

%install
DESTDIR=%{buildroot} meson install -C build --no-rebuild

%files
%{_bindir}/painter
%{_bindir}/pittore-mcp
%{_datadir}/applications/studio.pittore.painter.desktop
%{_datadir}/metainfo/studio.pittore.painter.metainfo.xml
%{_datadir}/mime/packages/painter.xml
%{_datadir}/icons/hicolor/scalable/apps/painter.svg

%changelog
