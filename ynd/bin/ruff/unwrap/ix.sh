{% extends '//bin/ruff/unwrap/ix.sh' %}

{% block version %}
0.14.0
{% endblock %}

{% block cargo_sha %}
9701d24322802e2ce66fd9d9929faf04c5b34d7b7a99f8cf1e4157bcce4fd07b
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
