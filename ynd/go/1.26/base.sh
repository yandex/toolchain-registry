{% extends '//die/std/ix.sh' %}

{% block go_version %}
1.26.5
{% endblock %}

{% set build_tool %}{% block build_tool %}{% endblock %}{% endset %}

{#
curl 'https://go.dev/dl/?mode=json&include=all' | jq -r '.[] | select(.version=="go1.26.5") | .files[] | select((.kind=="archive") and (.arch|IN("amd64","arm64")) and (.os|IN("linux", "windows", "darwin"))) | "", .filename, .sha256'
#}
{% block archive_hash %}
{% if linux and x86_64 or build_tool %}
    5c2c3b16caefa1d968a94c1daca04a7ca301a496d9b086e17ad77bb81393f053
{% elif linux and aarch64 %}
    fe4789e92b1f33358680864bbe8704289e7bb5fc207d80623c308935bd696d49
{% elif darwin and x86_64 %}
    6231d8d3b8f5552ec6cbf6d685bdd5482e1e703214b120e89b3bf0d7bf1ef725
{% elif darwin and arm64 %}
    efb87ff28af9a188d0536ef5d42e63dd52ba8263cd7344a993cc48dd11dedb6a
{% elif mingw32 %}
    97e6b2a833b6d89f9ff17d25419ac0a7e3b482a044e9ab18cdef834bd834fd38
{% endif %}
{% endblock %}

{% block archive_name %}
{% if linux and x86_64 or build_tool %}
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
