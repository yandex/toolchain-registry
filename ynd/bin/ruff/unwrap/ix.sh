{% extends '//bin/ruff/unwrap/ix.sh' %}

{% block version %}
0.17.4
{% endblock %}

{% block cargo_sha %}
014011f1f2cf60a3d609932fab6e5d06e6717666b203470e4174616a544a9639
{% endblock %}

{% block cargo_fetch_sha %}
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

{% block setup_target_tc %}
{% if mingw32 %}
export CPPFLAGS=
{% endif %}
{{super()}}
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
