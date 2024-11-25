{% extends '//die/std/ix.sh' %}

{% block bld_tool %}
bin/wget
{% endblock %}

{% block use_network %}true{% endblock %}

{% block yq_version %}4.35.2{% endblock %}

{% block yq_name %}
{% if linux and x86_64 %}
yq_linux_amd64
{% elif linux and aarch64 %}
yq_linux_arm64
{% elif darwin and x86_64 %}
yq_darwin_amd64
{% elif darwin and arm64 %}
yq_darwin_arm64
{% elif mingw32 %}
yq_windows_amd64.exe 
{% endif %}
{% endblock %}

{% block yq_url %}https://github.com/mikefarah/yq/releases/download/v{{self.yq_version().strip()}}/{{self.yq_name().strip()}}{% endblock %}

{% block step_unpack %}
mkdir src
{% endblock %}

{% block predict_outputs %}
[{"path": "", "sum": ""}]
{% endblock %}

{% block step_build %}
wget {{self.yq_url().strip()}} -O yq 
chmod +x yq
{% endblock %}

{% block install %}
mv ${tmp}/yq ${out}
{% endblock %}
