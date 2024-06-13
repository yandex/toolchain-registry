{% extends '//clang/16/profiles/t.sh' %}

{% block fname %}bolt.fdata{% endblock %}
{% block sandbox_id %}6483862193{% endblock %}

{% block env %}
export BOLT_EXTERNAL_PROFILE=${out}/share/{{self.fname().strip()}}
{% endblock %}
