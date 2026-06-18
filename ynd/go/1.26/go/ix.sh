{% extends '//ynd/go/1.26/base.sh' %}

{% block install %}
mv ${tmp}/src/* ${out}
{% endblock %}
