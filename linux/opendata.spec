Name:           pc-opendata
Version:        27.0
Release:        1%{?dist}
Summary:        High-performance ISO C11 ACID relational database engine
License:        BSD-3-Clause
URL:            https://www.pcopendata.com
Group:          Applications/Databases

Requires:       openssl-libs
Requires:       openldap
Requires(pre):  shadow-utils

%description
PC/OpenData SQL Server (Release 27.0) is a high-performance, embeddable ACID
relational database engine written in ISO C11. It features dual storage engines
(in-memory Copy-on-Write and 4KB slotted binary disk pages with B+ Trees),
a Cost-Based Optimizer (CBO), SIMD-vectorized columnar evaluation, cascading
referential integrity, synchronous 3-node Raft quorum consensus, native
VECTOR(dim) HNSW indexing, and an integrated GNU Nano-styled text editor (IRIS).

%install
mkdir -p %{buildroot}%{_bindir}
mkdir -p %{buildroot}%{_unitdir}
mkdir -p %{buildroot}%{_sysconfdir}/opendata
mkdir -p %{buildroot}%{_sharedstatedir}/opendata
mkdir -p %{buildroot}%{_localstatedir}/log/opendata
mkdir -p %{buildroot}%{_docdir}/%{name}

install -m 0755 %{_sourcedir}/opendata %{buildroot}%{_bindir}/opendata
install -m 0644 %{_sourcedir}/opendata.service %{buildroot}%{_unitdir}/opendata.service
install -m 0644 %{_sourcedir}/README.md %{buildroot}%{_docdir}/%{name}/README.md
install -m 0644 %{_sourcedir}/LICENSE %{buildroot}%{_docdir}/%{name}/LICENSE

%pre
getent group opendata >/dev/null || groupadd -r opendata
getent passwd opendata >/dev/null || \
    useradd -r -g opendata -d %{_sharedstatedir}/opendata -s /sbin/nologin \
    -c "PC/OpenData SQL Server Daemon" opendata
exit 0

%post
if [ -x /usr/bin/systemctl ]; then
    /usr/bin/systemctl daemon-reload >/dev/null 2>&1 || :
fi

%preun
if [ $1 -eq 0 ] && [ -x /usr/bin/systemctl ]; then
    /usr/bin/systemctl --no-reload disable --now opendata.service >/dev/null 2>&1 || :
fi

%postun
if [ -x /usr/bin/systemctl ]; then
    /usr/bin/systemctl daemon-reload >/dev/null 2>&1 || :
fi

%files
%{_bindir}/opendata
%{_unitdir}/opendata.service
%doc %{_docdir}/%{name}/README.md
%license %{_docdir}/%{name}/LICENSE
%dir %attr(0750, opendata, opendata) %{_sysconfdir}/opendata
%dir %attr(0750, opendata, opendata) %{_sharedstatedir}/opendata
%dir %attr(0750, opendata, opendata) %{_localstatedir}/log/opendata
