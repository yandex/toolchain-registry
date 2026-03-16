{% extends '//die/rust/cargo.sh' %}

{% block pkg_name %}
ast-index
{% endblock %}

{% block version %}
3.29.1
{% endblock %}

{% block cargo_url %}
https://github.com/defendend/Claude-ast-index-search/archive/refs/tags/v{{self.version().strip()}}.tar.gz
{% endblock %}

{% block cargo_sha %}
0ec5f5b60b16692039c32971924ec0a453aead3e7597cc35426bdc360fc51933
{% endblock %}

{% block cargo_bins %}
ast-index
{% endblock %}

{% block bld_tool %}
{{super()}}
bld/compiler
{% endblock %}

{% block bld_libs %}
lib/c
{{super()}}
{% endblock %}

{% block host_libs %}
lib/c
{{super()}}
{% endblock %}

{% block cargo_tool %}
bld/rust/88
{% endblock %}
