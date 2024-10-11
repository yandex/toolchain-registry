{% extends '//bin/python/frozen/unwrap/ix.sh' %}

{% block build_flags %}
:
{% endblock %}

{% block step_unpack %}
cat << EOF > python3
{% include 'py.py' %}
EOF
{% endblock %}

