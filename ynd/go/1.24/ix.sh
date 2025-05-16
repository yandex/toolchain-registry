{% extends '//die/std/ix.sh' %}

{% block go_version %}
1.24.3
{% endblock %}

{# curl 'https://go.dev/dl/?mode=json' | jq '.[] | select(.version=="go1.24.3") | .files[] | select((.kind=="archive") and (.arch|IN("amd64","arm64")) and (.os|IN("linux", "windows", "darwin")))' #}
{% block archive_hash %}
{% if linux and x86_64 %}
    3333f6ea53afa971e9078895eaa4ac7204a8c6b5c68c10e6bc9a33e8e391bdd8
{% elif linux and aarch64 %}
    a463cb59382bd7ae7d8f4c68846e73c4d589f223c589ac76871b66811ded7836
{% elif darwin and x86_64 %}
    13e6fe3fcf65689d77d40e633de1e31c6febbdbcb846eb05fc2434ed2213e92b
{% elif darwin and arm64 %}
    64a3fa22142f627e78fac3018ce3d4aeace68b743eff0afda8aae0411df5e4fb
{% elif mingw32 %}
    be9787cb08998b1860fe3513e48a5fe5b96302d358a321b58e651184fa9638b3
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
