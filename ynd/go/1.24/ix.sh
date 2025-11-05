{% extends '//die/std/ix.sh' %}

{% block go_version %}
1.24.8
{% endblock %}

{#
curl 'https://go.dev/dl/?mode=json&include=all' | jq -r '.[] | select(.version=="go1.24.8") | .files[] | select((.kind=="archive") and (.arch|IN("amd64","arm64")) and (.os|IN("linux", "windows", "darwin"))) | "", .filename, .sha256'
#}
{% block archive_hash %}
{% if linux and x86_64 %}
    6842c516ca66c89d648a7f1dbe28e28c47b61b59f8f06633eb2ceb1188e9251d
{% elif linux and aarch64 %}
    38ac33b4cfa41e8a32132de7a87c6db49277ab5c0de1412512484db1ed77637e
{% elif darwin and x86_64 %}
    ecb3cecb1e0bcfb24e50039701f9505b09744cc4730a8b9fc512b0a3b47cf232
{% elif darwin and arm64 %}
    0db27ff8c3e35fd93ccf9d31dd88a0f9c6454e8d9b30c28bd88a70b930cc4240
{% elif mingw32 %}
    0de7b65422d9377404a22f3903f17b20e4065f6c43e136ae2de16494c8c6b057
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
