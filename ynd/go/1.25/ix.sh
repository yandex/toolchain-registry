{% extends '//die/std/ix.sh' %}

{% block go_version %}
1.25rc2
{% endblock %}

{#
curl 'https://go.dev/dl/?mode=json&include=all' | jq -r '.[] | select(.version=="go1.25rc2") | .files[] | select((.kind=="archive") and (.arch|IN("amd64","arm64")) and (.os|IN("linux", "windows", "darwin"))) | "", .filename, .sha256'
#}
{% block archive_hash %}
{% if linux and x86_64 %}
    efcd3a151b174ffebde86b9d391ad59084300a4c5e9ea8c1d5dff90bbac38820
{% elif linux and aarch64 %}
    e9a077cef12d5c4a82df6d85a76f5bb7a4abd69c7d0fbab89d072591ef219ed3
{% elif darwin and x86_64 %}
    a09e19acff22863dfad464aee1b8b83689b75b233d65406b1d9e63d1dea21296
{% elif darwin and arm64 %}
    995979864ed7a80a81fc5c20381da9973f8528f3776ddcebf542d888e72991d2
{% elif mingw32 %}
    f0dadd0cdebbf56ad4ae7a010a5e980b64d6e4ddd13d6d294c0dad9e42285d09
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
