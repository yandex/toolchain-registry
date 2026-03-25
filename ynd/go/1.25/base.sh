{% extends '//die/std/ix.sh' %}

{% block go_version %}
1.25.8
{% endblock %}

{% set build_tool %}{% block build_tool %}{% endblock %}{% endset %}

{#
curl 'https://go.dev/dl/?mode=json&include=all' | jq -r '.[] | select(.version=="go1.25.8") | .files[] | select((.kind=="archive") and (.arch|IN("amd64","arm64")) and (.os|IN("linux", "windows", "darwin"))) | "", .filename, .sha256'
#}
{% block archive_hash %}
{% if linux and x86_64 or build_tool %}
    ceb5e041bbc3893846bd1614d76cb4681c91dadee579426cf21a63f2d7e03be6
{% elif linux and aarch64 %}
    7d137f59f66bb93f40a6b2b11e713adc2a9d0c8d9ae581718e3fad19e5295dc7
{% elif darwin and x86_64 %}
    a0b8136598baf192af400051cee2481ffb407f4c113a81ff400896e26cbce9e4
{% elif darwin and arm64 %}
    c6547959f5dbe8440bf3da972bd65ba900168de5e7ab01464fbdc7ac8375c21c
{% elif mingw32 %}
    8d4ed9a270b33df7a6d3ff3a5316e103e0042fcc4f0c9a80e40378700bab6794
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

{% block step_patch %}
(base64 -d | patch -p1) << EOF
{{ix.load_file('//go/1.25/old_coverage.diff') | b64e}}
EOF
{% endblock %}

{% block fetch %}
https://go.dev/dl/go{{self.go_version().strip()}}.{{self.archive_name().strip()}}
sha:{{self.archive_hash().strip()}}
{% endblock %}

{% block step_build %}
sed -i 's/GOTOOLCHAIN=auto/GOTOOLCHAIN=local/g' go.env
rm -r "test/fixedbugs/issue27836.dir"
{% endblock %}
