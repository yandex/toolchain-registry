{% extends '//die/rust/cargo.sh' %}

{% block pkg_name %}
pyrefly
{% endblock %}

{% block version %}
1.1.1
{% endblock %}

{% block cargo_url %}
https://github.com/facebook/pyrefly/archive/refs/tags/{{self.version().strip()}}.tar.gz
{% endblock %}

{% block cargo_sha %}
b0de811112349a53422532ab393bdd52c7a3a97720c5bc498c1a314e6b2406bd
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
{% endblock %}

{% block cargo_flags %}
{{super()}}
--locked
{% endblock %}

{% block cargo_tool %}
bld/rust/88
{% endblock %}
