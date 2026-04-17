{% extends '//ynd/go/1.26/base.sh' %}

{% block build_tool %}true{% endblock %}

{% block goos %}
{% if linux %}
    linux
{% elif darwin %}
    darwin
{% elif mingw32 %}
    windows
{% endif %}
{% endblock %}

{% block goarch %}
{% if (linux and x86_64) or (darwin and x86_64) or mingw32 %}
    amd64
{% elif (linux and aarch64) or (darwin and arm64) %}
    arm64
{% endif %}
{% endblock %}

{% block tool_folder_name %}
{{self.goos().strip()}}_{{self.goarch().strip()}}
{% endblock %}

{% block step_build %}
{{super()}}

export GOOS={{self.goos().strip()}}
export GOARCH={{self.goarch().strip()}}

bin/go build -o bin ./src/cmd/pack
bin/go build -o bin ./src/cmd/covdata
{% endblock %}

{% block install %}
mkdir -p ${out}/pkg/tool/{{self.tool_folder_name().strip()}}
cp -r ${tmp}/src/bin/pack{{target.exe_suffix}} ${out}/pkg/tool/{{self.tool_folder_name().strip()}}
cp -r ${tmp}/src/bin/covdata{{target.exe_suffix}} ${out}/pkg/tool/{{self.tool_folder_name().strip()}}
{% endblock %}
