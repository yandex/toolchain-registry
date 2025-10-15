{% extends '//bin/ruff/unwrap/ix.sh' %}

{% block version %}
0.13.0
{% endblock %}

{% block cargo_sha %}
3238859a723e88d79c8f57a07d68584edfe3abce452922b7631676cb547b6637
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
bld/rust/87
{% endblock %}
