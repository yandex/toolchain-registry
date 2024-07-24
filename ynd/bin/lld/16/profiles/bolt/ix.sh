{% extends '//clang/16/profiles/t.sh' %}

{% block fname %}bolt.prof{% endblock %}
{% block sandbox_id %}6735790589{% endblock %}

{% block step_build %}
wget https://devtools-registry.s3.yandex.net/{{self.sandbox_id().strip()}}
tar zxvf {{self.sandbox_id().strip()}}
{% endblock %}

{% block env %}
export LLD_BOLT_EXTERNAL_PROFILE=${out}/share/{{self.fname().strip()}}
{% endblock %}
