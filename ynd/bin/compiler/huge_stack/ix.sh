{% extends '//bin/clang/21/ix.sh' %}

{% block bld_tool %}
{{super()}}
bin/muslstack
{% endblock %}

{% block install %}
{{super()}}
muslstack -s 16777216 ${out}/bin/lld
{% endblock %}
