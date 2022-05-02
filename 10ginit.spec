# Build this using apx-rpmbuild.
%define name 10ginit

Name:           %{name}
Version:        %{version_rpm_spec_version}
Release:        %{version_rpm_spec_release}%{?dist}
Summary:        A tool to initialize and configure 10GbE on UW boards

License:        Reserved
URL:            https://github.com/uwcms/APx-%{name}
Source0:        %{name}-%{version_rpm_spec_version}.tar.gz

BuildRequires:  easymem easymem-devel libgpiod libgpiod-devel libwisci2c libwisci2c-devel
Requires:       easymem libgpiod libwisci2c

%global debug_package %{nil}

%description
This tool initialzies and configures the 10GbE on UW boards


%prep
%setup -q


%build
##configure
make %{?_smp_mflags}


%install
rm -rf $RPM_BUILD_ROOT
install -d -m 0755 $I %{buildroot}/%{_bindir}
install -D -m 0700 10ginit %{buildroot}/%{_bindir}/10ginit
install -D -m 0644 10ginit.service %{buildroot}/%{_unitdir}/10ginit.service
install -D -m 0644 10ginit.ini %{buildroot}/%{_sysconfdir}/10ginit.conf

%files
%{_bindir}/10ginit
%{_unitdir}/10ginit.service
%config(noreplace) %{_sysconfdir}/10ginit.conf


%post
%systemd_post 10ginit.service


%preun
%systemd_preun 10ginit.service


%postun
%systemd_postun_with_restart 10ginit.service


%changelog
* Thu Apr 9 2020 Jesra Tikalsky <jtikalsky@hep.wisc.edu>
- Initial spec file
