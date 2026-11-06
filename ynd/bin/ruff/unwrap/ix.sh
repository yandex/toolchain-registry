{% extends '//bin/ruff/unwrap/ix.sh' %}

{% block version %}
0.17.4
{% endblock %}

{% block cargo_sha %}
a466d837b406c888f4f76e4be808658f36846288aa87f81ac89afcf078ad386a
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
bld/rust/97
{% endblock %}
