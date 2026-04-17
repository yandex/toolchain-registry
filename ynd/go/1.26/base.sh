{% extends '//die/std/ix.sh' %}

{% block go_version %}
1.26.2
{% endblock %}

{% set build_tool %}{% block build_tool %}{% endblock %}{% endset %}

{#
curl 'https://go.dev/dl/?mode=json&include=all' | jq -r '.[] | select(.version=="go1.26.2") | .files[] | select((.kind=="archive") and (.arch|IN("amd64","arm64")) and (.os|IN("linux", "windows", "darwin"))) | "", .filename, .sha256'
#}
{% block archive_hash %}
{% if linux and x86_64 or build_tool %}
    990e6b4bbba816dc3ee129eaeaf4b42f17c2800b88a2166c265ac1a200262282
{% elif linux and aarch64 %}
    c958a1fe1b361391db163a485e21f5f228142d6f8b584f6bef89b26f66dc5b23
{% elif darwin and x86_64 %}
    bc3f1500d9968c36d705442d90ba91addf9271665033748b82532682e90a7966
{% elif darwin and arm64 %}
    32af1522bf3e3ff3975864780a429cc0b41d190ec7bf90faa661d6d64566e7af
{% elif mingw32 %}
    094d05caaf6ba235e2bd570b625d064ceb65943866252722a8f3fdba232139c6
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
