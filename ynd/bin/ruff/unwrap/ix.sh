{% extends '//bin/ruff/unwrap/ix.sh' %}

{% block version %}
0.14.1
{% endblock %}

{% block cargo_sha %}
d20308dac22c63bca153ac2bf71b7f38afd56786901537622dd4139df8a9c43c
{% endblock %}

{% block bld_libs %}
{{super()}}
{% if mingw32 %}
lib/shim/fake(lib_name=windows.0.53.0)
{% endif %}
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

{% block cargo_tool %}
bld/rust/88
{% endblock %}
