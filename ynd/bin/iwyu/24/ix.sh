{% extends '//clang/20/template.sh' %}

{% block bld_deps %}
{{super()}}
//clang/20
{% endblock %}

{% block fetch %}
{{super()}}
https://github.com/include-what-you-use/include-what-you-use/archive/clang_20.tar.gz
sha:fb9dcffdb34d70e045d7023f2f9c5b1916b1bc2072d6a85a58cb10417be0e98d
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
tar --strip-components=1 -xzf ${src}/*clang_20.tar.gz
{% endblock %}

{% block ninja_install_targets %}
{{super()}}
tools/iwyu/install
{% endblock %}

{% block postinstall %}
mv ${out}/bin/include-what-you-use ${out}/bin/include-what-you-use.bin

# Create wrapper script with correct header search order
cat > ${out}/bin/include-what-you-use << 'EOF'
#!/bin/bash
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CLANG_INCLUDE_DIR="${SCRIPT_DIR}/../lib/clang/20/include"
LIBCXX_INCLUDE_DIR="${SCRIPT_DIR}/../include/c++/v1"

# C++ stdlib first, then system includes
CMD=("${SCRIPT_DIR}/include-what-you-use.bin")
[[ -d "${LIBCXX_INCLUDE_DIR}" ]] && CMD+=("-I${LIBCXX_INCLUDE_DIR}")
[[ -d "${CLANG_INCLUDE_DIR}" ]] && CMD+=("-isystem${CLANG_INCLUDE_DIR}")
CMD+=("$@")

exec "${CMD[@]}"
EOF
chmod +x ${out}/bin/include-what-you-use
{% endblock %}
