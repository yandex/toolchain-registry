{% extends '//ynd/go/1.26/base.sh' %}

{% block step_build %}
{{super()}}
rm -rf bin
rm -rf pkg
{% endblock %}

{% block install %}
mv ${tmp}/src/* ${out}
{% endblock %}
