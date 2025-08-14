{% extends '//die/std/ix.sh' %}

{% block go_version %}
1.24.6
{% endblock %}

{#
curl 'https://go.dev/dl/?mode=json&include=all' | jq -r '.[] | select(.version=="go1.24.6") | .files[] | select((.kind=="archive") and (.arch|IN("amd64","arm64")) and (.os|IN("linux", "windows", "darwin"))) | "", .filename, .sha256'
#}
{% block archive_hash %}
{% if linux and x86_64 %}
    bbca37cc395c974ffa4893ee35819ad23ebb27426df87af92e93a9ec66ef8712
{% elif linux and aarch64 %}
    124ea6033a8bf98aa9fbab53e58d134905262d45a022af3a90b73320f3c3afd5
{% elif darwin and x86_64 %}
    4a8d7a32052f223e71faab424a69430455b27b3fff5f4e651f9d97c3e51a8746
{% elif darwin and arm64 %}
    4e29202c49573b953be7cc3500e1f8d9e66ddd12faa8cf0939a4951411e09a2a
{% elif mingw32 %}
    4fbc8af2cfca9e5059019b5150a426eb78e1e57718bf08f0e52b1c942a2782bf
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
