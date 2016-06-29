Name:           libtbm-sprd
Version:        1.0.1
Release:        0
License:        MIT
Summary:        Tizen Buffer Manager - sprd backend
Group:          System/Libraries
ExcludeArch:    i586 x86_64
%if ("%{?tizen_target_name}" != "TM1")
ExclusiveArch:
%endif
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  pkgconfig(libdrm)
BuildRequires:  pkgconfig(libtbm)
BuildRequires:  pkgconfig(dlog)
BuildRequires:  pkgconfig(libudev)
BuildRequires:  kernel-headers-tizen-dev
BuildConflicts:  linux-glibc-devel

%description
descriptionion:Tizen Buffer manager backend module for spreadtrum

%global TZ_SYS_RO_SHARE  %{?TZ_SYS_RO_SHARE:%TZ_SYS_RO_SHARE}%{!?TZ_SYS_RO_SHARE:/usr/share}

%prep
%setup -q

%build

%reconfigure --prefix=%{_prefix} --libdir=%{_libdir}/bufmgr \
            CFLAGS="${CFLAGS} -Wall -Werror `pkg-config --cflags dlog`" LDFLAGS="${LDFLAGS} -Wl,--hash-style=both -Wl,--as-needed"

make %{?_smp_mflags}

%install
rm -rf %{buildroot}
mkdir -p %{buildroot}/%{TZ_SYS_RO_SHARE}/license
cp -af COPYING %{buildroot}/%{TZ_SYS_RO_SHARE}/license/%{name}
mkdir -p %{buildroot}%{_libdir}/udev/rules.d/
cp -af rules/99-libtbm_sprd.rules %{buildroot}%{_libdir}/udev/rules.d/
%make_install


%post
if [ -f %{_libdir}/bufmgr/libtbm_default.so ]; then
    rm -rf %{_libdir}/bufmgr/libtbm_default.so
fi
ln -s libtbm_sprd.so %{_libdir}/bufmgr/libtbm_default.so

%postun -p /sbin/ldconfig

%files
%defattr(-,root,root,-)
%{TZ_SYS_RO_SHARE}/license/%{name}
%{_libdir}/bufmgr/libtbm_*.so*
%{_libdir}/udev/rules.d/99-libtbm_sprd.rules

