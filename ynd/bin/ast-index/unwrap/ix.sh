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
9d1c26f44cee4b2abda18366b9fdf2cecca403404e8f2995e35495ef97e97c4a
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
