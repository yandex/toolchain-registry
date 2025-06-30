{% extends '//die/std/ix.sh' %}

{% block bld_tool %}
bin/wget
bin/unzip
{% endblock %}

{% block py_version %}3.12.6{% endblock %}
{% block src_url %}https://www.python.org/ftp/python/{{self.py_version().strip()}}/python-{{self.py_version().strip()}}-embed-win32.zip{% endblock %}

{% block fetch %}
{{self.src_url().strip()}}
sha:8c1b9cd9f63001a58f986f8570a5ab79276e8c1a048f7bfe3efe631a878e189c
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
