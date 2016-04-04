%global _hardened_build 1

Name:           drpm
Version:        0.3.0
Release:        1%{?dist}
Summary:        A small library for fetching information from deltarpm packages
License:        LGPLv3+ and BSD
URL:            http://fedorahosted.org/%{name}
Source:         http://fedorahosted.org/released/%{name}/%{name}-%{version}.tar.bz2

BuildRequires:  pkgconfig
BuildRequires:  rpm-devel
BuildRequires:  zlib-devel
BuildRequires:  bzip2-devel
BuildRequires:  xz-devel
BuildRequires:  openssl-devel

BuildRequires:  cmake >= 2.8
BuildRequires:  libcmocka-devel >= 1.0
%ifarch x86_64 %{ix86} %{arm} ppc ppc32 %{power64} s390x aarch64 amd64 mips32 mips64
BuildRequires:  valgrind
%endif

%package devel
Summary:        C interface for the drpm library
Requires:       %{name}%{?_isa} = %{version}-%{release}

%description
The drpm package provides a small library allowing one to fetch
various info from deltarpm packages.

%description devel
The drpm-devel package provides a C interface (drpm.h) for the drpm library.

%prep
%setup -q

%build
%cmake .
make %{?_smp_mflags}

%install
%make_install

%check
make check %{?_smp_mflags}

%post -p /sbin/ldconfig

%postun -p /sbin/ldconfig

%files
%{_libdir}/libdrpm.so.*
%license COPYING COPYING.LESSER LICENSE.BSD

%files devel
%{_libdir}/libdrpm.so
%{_includedir}/drpm.h
%{_libdir}/pkgconfig/drpm.pc

%changelog
* Thu Sep 3 2015 Matej Chalk <mchalk@redhat.com> 0.3.0-1
- Bumped minor version (deltarpm creation added)

* Tue Aug 4 2015 Matej Chalk <mchalk@redhat.com> 0.2.1-1
- Added openssl dependency

* Fri Jul 24 2015 Matej Chalk <mchalk@redhat.com> 0.2.0-2
- Fixed bug in test suite

* Tue Jun 23 2015 Matej Chalk <mchalk@redhat.com> 0.2.0-1
- Bumped minor version

* Fri Jun 19 2015 Matej Chalk <mchalk@redhat.com> 0.1.3-4
- Memory test only for architectures that have valgrind (#1232157)

* Wed Mar 11 2015 Matej Chalk <mchalk@redhat.com> 0.1.3-3
- Added cmocka and valgrind package dependencies

* Fri Mar 6 2015 Matej Chalk <mchalk@redhat.com> 0.1.3-2
- Added check section

* Fri Feb 13 2015 Matej Chalk <mchalk@redhat.com> 0.1.3-1
- Bumped version to 0.1.3
- Added CMake tool

* Fri Dec 19 2014 Matej Chalk <mchalk@redhat.com> 0.1.2-4
- Enabled hardened build

* Mon Dec 15 2014 Matej Chalk <mchalk@redhat.com> 0.1.2-3
- Added unversioned .so to package to enable linking with -ldrpm

* Thu Dec 11 2014 Matej Chalk <mchalk@redhat.com> 0.1.2-2
- Removed unversioned .so from package
- Included copies of both GPLv3 and LGPLv3

* Wed Dec 3 2014 Matej Chalk <mchalk@redhat.com> 0.1.2-1
- Bumped version to 0.1.2
- Added drpm.pc file for pkgconfig tool

* Thu Nov 6 2014 Matej Chalk <mchalk@redhat.com> 0.1.1-1
- Bumped version to 0.1.1

* Wed Nov 5 2014 Matej Chalk <mchalk@redhat.com> 0.1.0-1
- Initial RPM release
