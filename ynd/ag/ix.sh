{% extends '//bin/ag/ix.sh' %}

{% block make_target %}
ag{{target.exe_suffix}}
{% endblock %}

{% block patches %}
arcignore.patch
getpagesize.patch
{% endblock %}

{% block patch %}
{{super()}}

{% for p in self.patches().strip().split() %}
(base64 -d | patch -p1) << EOF
{{ix.load_file('//ynd/ag/patches/' + p) | b64e}}
EOF
{% endfor %}
{% endblock %}
