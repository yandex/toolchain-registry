{% extends '//clang/16/profiles/t.sh' %}

{% block fname %}pgo-merged.prof{% endblock %}
{% block sandbox_id %}6484986666{% endblock %}

{% block env %}
export PGO_EXTERNAL_PROFILE=${out}/share/{{self.fname().strip()}}
{% endblock %}
