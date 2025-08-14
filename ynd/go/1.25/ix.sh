{% extends '//die/std/ix.sh' %}

{% block go_version %}
1.25.0
{% endblock %}

{#
curl 'https://go.dev/dl/?mode=json&include=all' | jq -r '.[] | select(.version=="go1.25.0") | .files[] | select((.kind=="archive") and (.arch|IN("amd64","arm64")) and (.os|IN("linux", "windows", "darwin"))) | "", .filename, .sha256'
#}
{% block archive_hash %}
{% if linux and x86_64 %}
    2852af0cb20a13139b3448992e69b868e50ed0f8a1e5940ee1de9e19a123b613
{% elif linux and aarch64 %}
    05de75d6994a2783699815ee553bd5a9327d8b79991de36e38b66862782f54ae
{% elif darwin and x86_64 %}
    5bd60e823037062c2307c71e8111809865116714d6f6b410597cf5075dfd80ef
{% elif darwin and arm64 %}
    544932844156d8172f7a28f77f2ac9c15a23046698b6243f633b0a0b00c0749c
{% elif mingw32 %}
    89efb4f9b30812eee083cc1770fdd2913c14d301064f6454851428f9707d190b
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
