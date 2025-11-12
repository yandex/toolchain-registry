{% extends '//ynd/go/1.25/base.sh' %}

{% block install %}
mv ${tmp}/src/* ${out}
{% endblock %}