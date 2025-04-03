{% extends '//die/std/ix.sh' %}

{% block go_version %}
1.23.8
{% endblock %}

{# curl 'https://go.dev/dl/?mode=json' | jq '.[] | select(.version=="go1.23.8") | .files[] | select((.kind=="archive") and (.arch|IN("amd64","arm64")) and (.os|IN("linux", "windows", "darwin")))' #}
{% block archive_hash %}
{% if linux and x86_64 %}
    45b87381172a58d62c977f27c4683c8681ef36580abecd14fd124d24ca306d3f
{% elif linux and aarch64 %}
    9d6d938422724a954832d6f806d397cf85ccfde8c581c201673e50e634fdc992
{% elif darwin and x86_64 %}
    4a0f0a5eb539013c1f4d989e0864aed45973c0a9d4b655ff9fd56013e74c1303
{% elif darwin and arm64 %}
    d4f53dcaecd67d9d2926eab7c3d674030111c2491e68025848f6839e04a4d3d1
{% elif mingw32 %}
    e0ad643f94875403830e84198dc9df6149647c924bfa91521f6eb29f4c013dc7
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
