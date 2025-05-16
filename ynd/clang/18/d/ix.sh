{% extends '//clang/18/template.sh' %}

{% block llvm_projects %}
{{super()}}
clang-tools-extra
{% endblock %}

{% block llvm_targets %}
clangd
clang-resource-headers
{% endblock %}

{% block clangd_patches %}
67228-include-cleaner-associated-header.patch
87208-unused-angled-includes.patch
113796-113612-118174-std-symbol-mapping.patch
{% endblock %}

{% block patch %}
{{super()}}
{% for p in self.clangd_patches().strip().split() %}
(base64 -d | patch -p1) << EOF
{{ix.load_file('//clang/18/d/patches/' + p) | b64e}}
EOF
{% endfor %}
{% endblock %}

{% block postinstall %}
:
{% endblock %}

{% block clang_fix_includes %}
{% endblock %}
