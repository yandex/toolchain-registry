{% extends '//ynd/go/1.27/base.sh' %}

{% block install %}
mv ${tmp}/src/* ${out}
{% endblock %}
