Name:           libtbm-sprd
Version:        1.0.0
Release:        0
License:        MIT
Summary:        Tizen Buffer Manager - sprd backend
Group:          System/Libraries
ExcludeArch:    i586 x86_64
%if ("%{?tizen_target_name}" != "TM1")
ExclusiveArch:
%endif
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  pkgconfig(pthread-stubs)
BuildRequires:  pkgconfig(libdrm)
BuildRequires:  pkgconfig(libtbm)
BuildRequires:  pkgconfig(dlog)
BuildRequires:  pkgconfig(libudev)
BuildRequires:  kernel-headers-tizen-dev
BuildConflicts:  linux-glibc-devel

%description
descriptionion:Tizen Buffer manager backend module for spreadtrum

%prep
%setup -q

%build

%reconfigure --prefix=%{_prefix} --libdir=%{_libdir}/bufmgr \
            CFLAGS="${CFLAGS} -Wall -Werror `pkg-config --cflags dlog`" LDFLAGS="${LDFLAGS} -Wl,--hash-style=both -Wl,--as-needed"

make %{?_smp_mflags}

%install
rm -rf %{buildroot}
mkdir -p %{buildroot}/usr/share/license
cp -af COPYING %{buildroot}/usr/share/license/%{name}
%make_install


%post
if [ -f %{_libdir}/bufmgr/libtbm_default.so ]; then
    rm -rf %{_libdir}/bufmgr/libtbm_default.so
fi
ln -s libtbm_sprd.so %{_libdir}/bufmgr/libtbm_default.so

%postun -p /sbin/ldconfig

%files
%{_libdir}/bufmgr/libtbm_*.so*
/usr/share/license/%{name}

