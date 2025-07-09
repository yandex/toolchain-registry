{% extends '//die/std/ix.sh' %}

{% block go_version %}
1.24.5
{% endblock %}

{#
curl 'https://go.dev/dl/?mode=json&include=all' | jq -r '.[] | select(.version=="go1.24.5") | .files[] | select((.kind=="archive") and (.arch|IN("amd64","arm64")) and (.os|IN("linux", "windows", "darwin"))) | "", .filename, .sha256'
#}
{% block archive_hash %}
{% if linux and x86_64 %}
    10ad9e86233e74c0f6590fe5426895de6bf388964210eac34a6d83f38918ecdc
{% elif linux and aarch64 %}
    0df02e6aeb3d3c06c95ff201d575907c736d6c62cfa4b6934c11203f1d600ffa
{% elif darwin and x86_64 %}
    2fe5f3866b8fbcd20625d531f81019e574376b8a840b0a096d8a2180308b1672
{% elif darwin and arm64 %}
    92d30a678f306c327c544758f2d2fa5515aa60abe9dba4ca35fbf9b8bfc53212
{% elif mingw32 %}
    658f432689106d4e0a401a2ebb522b1213f497bc8357142fe8def18d79f02957
{% endif %}
{% endblock %}

{% block archive_name %}
{% if linux and x86_64 %}
    linux-amd64.tar.gz
{% elif linux and aarch64 %}
    linux-arm64.tar.gz
{% elif darwin and x86_64 %}
    darwin-amd64.tar.gz
{% elif darwin and arm64 %}
    darwin-arm64.tar.gz
{% elif mingw32 %}
    windows-amd64.zip
{% endif %}
{% endblock %}

{% block fetch %}
https://go.dev/dl/go{{self.go_version().strip()}}.{{self.archive_name().strip()}}
sha:{{self.archive_hash().strip()}}
{% endblock %}

{% block step_build %}
sed -i 's/GOTOOLCHAIN=auto/GOTOOLCHAIN=local/g' go.env
rm -r "test/fixedbugs/issue27836.dir"
{% endblock %}

{% block install %}
mv ${tmp}/src/* ${out}
{% endblock %}
