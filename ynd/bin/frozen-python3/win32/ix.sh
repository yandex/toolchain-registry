{% extends '//die/std/ix.sh' %}

{% block bld_tool %}
bin/wget
bin/unzip
{% endblock %}

{% block use_network %}true{% endblock %}

{% block py_version %}3.12.6{% endblock %}
{% block src_url %}https://www.python.org/ftp/python/{{self.py_version().strip()}}/python-{{self.py_version().strip()}}-embed-win32.zip{% endblock %}

{% block step_unpack %}
mkdir src
wget {{self.src_url().strip()}} -O src.zip
{% endblock %}

{% block predict_outputs %}
[{"path": "python3-mingw", "sum": "b6eecbdfd865e4a3ae8bed93b9f2cd95"}]
{% endblock %}

{% block step_build %}
set -xue
unzip src.zip -d bin/ && rm src.zip
cd bin && unzip python312.zip && rm python312.zip
mv python.exe python3
{% endblock %}

{% block install %}
mv ${tmp}/* ${out}
{% endblock %}

{% block strip_bin %}:{% endblock %}
