{% extends '//ynd/go/1.27/base.sh' %}

{% block install %}
# The patched cover tool is installed by go_pack.
rm -f ${tmp}/src/pkg/tool/*/cover ${tmp}/src/pkg/tool/*/cover.exe
mv ${tmp}/src/* ${out}
{% endblock %}
