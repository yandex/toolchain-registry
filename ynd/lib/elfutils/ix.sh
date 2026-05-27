{% extends '//lib/elfutils/t/ix.sh' %}

{% block bld_libs %}
{{super()}}
lib/curl
lib/json/c
lib/kernel
{% endblock %}

{% block configure_flags %}
{{super()}}
--enable-libdebuginfod
--enable-debuginfod
{% endblock %}
