{% extends '//lib/xml/2/t/ix.sh' %}

{% block patch %}
{% if mingw32 %}
base64 -d << EOF | patch -p1
{% include '0.diff/base64' %}
EOF
{% endif %}
{% endblock %}

