{% extends '//die/std/ix.sh' %}

{% block bld_tool %}
bin/wget
bin/unzip
{% endblock %}

{% block py_version %}3.13.12{% endblock %}
{% block src_url %}https://www.python.org/ftp/python/{{self.py_version().strip()}}/python-{{self.py_version().strip()}}-embed-win32.zip{% endblock %}

{% block fetch %}
{{self.src_url().strip()}}
sha:51ec4c741212f9488d678033bd2c8a67fc27cf7d36abffee7cb9f4481c9fb52b
{% endblock %}


{% block step_build %}
set -xue
cp ${src}/python-{{self.py_version().strip()}}-embed-win32.zip src.zip
mkdir bin
unzip src.zip -d bin/ && rm src.zip

cd bin && unzip python312.zip && rm python312.zip
mv python.exe python3
{% endblock %}

{% block install %}
mv ${tmp}/src/* ${out}

mkdir -p ${out}/fix
cat << EOF > ${out}/fix/remove_tmp.sh
rm -rf tmp
EOF
{% endblock %}

{% block strip_bin %}:{% endblock %}
