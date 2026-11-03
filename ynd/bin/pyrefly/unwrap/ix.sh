{% extends '//die/rust/cargo.sh' %}

{% block pkg_name %}
pyrefly
{% endblock %}

{% block version %}
1.4.0
{% endblock %}

{% block cargo_url %}
https://github.com/facebook/pyrefly/archive/refs/tags/{{self.version().strip()}}.tar.gz
{% endblock %}

{% block cargo_sha %}
86625664627d2cbdae82bcb1c3c30c4c1561f320fc835a2b899e3f76c8be83c2
{% endblock %}

{% block cargo_fetch_sha %}
491cef3a7daf3028233ce9a1f22b36bce52670f29848559f998c4fd6cf2b4b61
{% endblock %}

{% block cargo_bins %}
pyrefly
{% endblock %}

{% block cargo_packages %}
pyrefly
{% endblock %}

{% block cargo_ver %}
v4
{% endblock %}

{% block bld_libs %}
lib/c
lib/zstd
{% if darwin %}
ynd/bin/pyrefly/darwin
{% endif %}
{{super()}}
{% endblock %}

{% block host_libs %}
lib/c
lib/zstd
{{super()}}
{% endblock %}

{% block bld_tool %}
{{super()}}
bld/compiler
{% endblock %}

{% block setup_tools %}
{% if darwin %}
ln -s $(which llvm-objcopy) rust-objcopy
{% endif %}
{{super()}}
{% if host.os == 'linux' %}
# Host proc-macros are Linux shared objects even for Darwin targets.
# Add the musl search path only to rustcc's freestanding candidate.  Passing it
# as a linker argument would also expose ELF libraries to the Darwin linker.
freestanding_clang=$(command -v "${FREESTANDING_CLANG}")
cat << EOF > rustcc-freestanding-clang
#!/usr/bin/env sh
exec "${freestanding_clang}" -L"${LDSO%/bin/*}/lib" "\${@}"
EOF
chmod +x rustcc-freestanding-clang

for tool in cc c++; do
    mv "${tool}" "${tool}.rustcc"
    cat << EOF > "${tool}"
#!/usr/bin/env sh
export FREESTANDING_CLANG="${PWD}/rustcc-freestanding-clang"
exec "${PWD}/${tool}.rustcc" "\${@}"
EOF
    chmod +x "${tool}"
done
{% endif %}
{% endblock %}

{% block cargo_flags %}
{{super()}}
--locked
{% endblock %}

{% block cargo_tool %}
bld/rust/96
{% endblock %}
