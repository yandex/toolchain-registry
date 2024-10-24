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

# Collect all modules
export PYTHONPLATLIBDIR=${TARGET_PYTHONHOME}/lib/python3.12
cur=$(pwd)
cd $PYTHONPLATLIBDIR
find . -type f -name '*.py' | cut -b3- | sed -E 's|\.py$||g' | sed -E 's|/|\.|g' | grep -v '\-' >> $cur/ext_modules
cd $cur
cat modules >> ext_modules
cat ext_modules | sort | uniq > modules

{{super()}}
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

