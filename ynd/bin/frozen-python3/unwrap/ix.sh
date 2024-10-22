{% extends '//bin/python/frozen/unwrap/ix.sh' %}

{% block build_flags %}
:
{% endblock %}


{% block build %}
{% if linux %}
export _PYTHON_SYSCONFIGDATA_NAME='_sysconfigdata__linux_'
{% elif darwin %}
export _PYTHON_SYSCONFIGDATA_NAME='_sysconfigdata__darwin_'
{% elif mingw32 %}
export _PYTHON_SYSCONFIGDATA_NAME='_sysconfigdata__win32_'
{% endif %}
{{super()}}
{% endblock %}

{% block extra_modules %}
sre_parse
sre_constants
{% endblock %}

{% block step_unpack %}
cat << EOF > python3
{% include 'py.py' %}
EOF
{% endblock %}

{% block install %}
{{super()}}
mkdir -p ${out}/fix
cat << EOF > ${out}/fix/remove_share.sh
rm -rf share
EOF
{% endblock %}

