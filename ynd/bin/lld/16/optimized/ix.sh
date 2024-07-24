{% extends '//die/hub.sh' %}

{% block run_deps %}
{% if linux and x86_64 %}
bin/lld/16/bolt/ext-profile
{% elif mingw32 %}
bin/lld/16/ix.sh
{% else %}
bin/lld/16/pgo/ext-profile
{% endif %}
{% endblock %}
