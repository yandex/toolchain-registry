{% extends '//ynd/go/1.25/base.sh' %}

{% block bld_tool %}
bld/bash
{% endblock %}

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

export GOROOT_BOOTSTRAP=$(pwd)
cd src
bash ./bootstrap.bash
cd ..

bin/go build ./src/cmd/pack
{% endblock %}

{% block install %}
mv ${tmp}/go-{{self.goos().strip()}}-{{self.goarch().strip()}}-bootstrap/pkg ${out}
mv ${tmp}/go-{{self.goos().strip()}}-{{self.goarch().strip()}}-bootstrap/bin ${out}

cp ${tmp}/src/pack{{target.exe_suffix}} ${out}/pkg/tool/{{self.tool_folder_name().strip()}}/
{% endblock %}
