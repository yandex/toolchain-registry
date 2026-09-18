{% extends '//bin/ag/ix.sh' %}

{% block patches %}
arcignore.patch
{% endblock %}

{% block patch %}
{{super()}}

{% for p in self.patches().strip().split() %}
(base64 -d | patch -p1) << EOF
{{ix.load_file('//ynd/ag/patches/' + p) | b64e}}
EOF
{% endfor %}
{% endblock %}
