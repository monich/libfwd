Name: libfwd
Version: 1.0.1
Release: 0

Summary: Connection forwarding
License: BSD
URL: https://github.com/monich/libfwd
Source: %{name}-%{version}.tar.bz2

%define libglibutil_version 1.0.76

BuildRequires: pkgconfig
BuildRequires: pkgconfig(glib-2.0) >= 2.48
BuildRequires: pkgconfig(libglibutil) >= %{libglibutil_version}
BuildRequires: pkgconfig(libgiorpc)

# license macro requires rpm >= 4.11
BuildRequires: pkgconfig(rpm)
%define license_support %(pkg-config --exists 'rpm >= 4.11'; echo $?)

# make_build macro appeared in rpm 4.12
%{!?make_build:%define make_build make %{_smp_mflags}}

Requires: glib2 >= 2.32
Requires: libglibutil >= %{libglibutil_version}
Requires(post): /sbin/ldconfig
Requires(postun): /sbin/ldconfig

%description
Connection forwarding library.

%package devel
Summary: Development library for %{name}
Requires:  pkgconfig(libglibutil) >= %{libglibutil_version}
Requires: %{name} = %{version}

%description devel
This package contains the development library for %{name}.

%package tools
Summary: %{name} tools
Requires: glib2 >= 2.48

%description tools
%{name} example applications

%prep
%setup -q

%build
%make_build LIBDIR=%{_libdir} KEEP_SYMBOLS=1 release pkgconfig
%make_build KEEP_SYMBOLS=1 -C tools release

%install
make LIBDIR=%{_libdir} DESTDIR=%{buildroot} install-dev
make DESTDIR=%{buildroot} -C tools install

%check
make test

%post -p /sbin/ldconfig

%postun -p /sbin/ldconfig

%files
%defattr(-,root,root,-)
%{_libdir}/%{name}.so.*
%if %{license_support} == 0
%license LICENSE
%endif

%files devel
%defattr(-,root,root,-)
%dir %{_includedir}/fwd
%{_libdir}/pkgconfig/*.pc
%{_libdir}/%{name}.so
%{_includedir}/fwd/*.h

%files tools
%defattr(-,root,root,-)
%{_bindir}/fwd
