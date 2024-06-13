{% extends '//die/std/ix.sh' %}

{% block bld_tool %}
bin/wget
{% endblock %}

{% block use_network %}true{% endblock %}

{% block fname %}
__file_name_not_provided__
{% endblock %}
{% block sandbox_id %}
__sandbox_id_not_provide__
{% endblock %}

{% block step_unpack %}
mkdir src
{% endblock %}

{% block predict_outputs %}
[{"path": "share/{{self.fname().strip()}}", "sum": "{{self.sandbox_id().strip()}}"}]
{% endblock %}

{% block step_build %}
wget https://devtools-registry.s3.yandex.net/{{self.sandbox_id().strip()}}
mv {{self.sandbox_id().strip()}} {{self.fname().strip()}}
{% endblock %}

{% block install %}
mkdir ${out}/share
mv ${tmp}/{{self.fname().strip()}} ${out}/share
ls -la ${out}/share
{% endblock %}

