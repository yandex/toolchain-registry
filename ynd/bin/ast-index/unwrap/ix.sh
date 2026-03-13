{% extends '//die/rust/cargo.sh' %}

{% block pkg_name %}
ast-index
{% endblock %}

{% block version %}
3.29.0
{% endblock %}

{% block cargo_url %}
https://github.com/defendend/Claude-ast-index-search/archive/refs/tags/v{{self.version().strip()}}.tar.gz
{% endblock %}

{% block cargo_sha %}
1617f41b302b964e49f6e64dfda005e9e9b606101cc83fb940da0ffa7f438231
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
