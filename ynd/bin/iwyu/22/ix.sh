{% extends '//clang/18/template.sh' %}

{% block bld_deps %}
{{super()}}
//clang/18
{% endblock %}

{% block fetch %}
{{super()}}
https://github.com/include-what-you-use/include-what-you-use/archive/clang_18.tar.gz
sha:b3af37c6d61522597485541e310202ef7bb96bb78fd020ae0c9f8df5e7178d2e
{% endblock %}

{% block llvm_targets %}
clang-resource-headers
{% endblock %}

{% block cmake_flags %}
{{super()}}
LLVM_ENABLE_PROJECTS="llvm;clang"
LLVM_EXTERNAL_PROJECTS="iwyu"
LLVM_EXTERNAL_IWYU_SOURCE_DIR=${tmp}/iwyu_src
{% endblock %}

{% block patch %}
{{super()}}

mkdir -p ${tmp}/iwyu_src
cd ${tmp}/iwyu_src
tar --strip-components=1 -xzf ${src}/*clang_18.tar.gz
{% endblock %}

{% block ninja_install_targets %}
{{super()}}
tools/iwyu/install
{% endblock %}

{% block postinstall %}
:
{% endblock %}
