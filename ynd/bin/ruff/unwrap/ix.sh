{% extends '//bin/ruff/unwrap/ix.sh' %}

{% block version %}
0.14.0
{% endblock %}

{% block cargo_sha %}
9fab62477631d1fbcbc15da037590e36c112a0a571f210eb5735997a9808aa96
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
