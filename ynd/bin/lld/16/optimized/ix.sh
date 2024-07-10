{% extends '//die/hub.sh' %}

{% block run_deps %}
{% if mingw32 %}
bin/lld/16/ix.sh
{% else %}
bin/lld/16/pgo/ext-profile
{% endif %}
{% endblock %}
