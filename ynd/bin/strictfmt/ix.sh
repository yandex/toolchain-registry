{% extends '//die/c/cmake.sh' %}

{% block version %}
0.1.13
{% endblock %}

{% block fetch %}
https://github.com/elizarov/strictfmt/archive/502587a018646c4153e7c2dc7b9634700cab3b15.tar.gz
sha:7ad93f60a36cbc57f819f0c3d32bae50d35f2c42baca2c75f09faf4734c62e9a
{% endblock %}

{% block bld_tool %}
{{super()}}
bld/compiler
{% endblock %}

{% block bld_libs %}
{{super()}}
lib/c++
{% endblock %}

{% block cmake_flags %}
{{super()}}
STRICTFMT_VERSION={{self.version().strip()}}
{% endblock %}

{% block patch %}
{{super()}}
cat >> CMakeLists.txt <<'CMAKE'
target_include_directories(strictfmt_tree_sitter_runtime PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/tree-sitter/tree-sitter/lib/src")
{% if mingw32 %}
target_link_options(strictfmt PRIVATE "LINKER:--stack,16777216")
{% endif %}
CMAKE
{% if mingw32 %}
sed -i 's/std::jthread thread_/std::thread thread_/' src/tools/tools_parallel.cpp
{% endif %}
{% endblock %}

{% block ninja_build_targets %}
strictfmt
{% endblock %}

{% block install %}
{% set executable = 'strictfmt.exe' if mingw32 else 'strictfmt' %}
mkdir -p "${out}/bin"
cp "{{ninja_build_dir}}/{{executable}}" "${out}/bin/"
for license in LICENSE THIRD_PARTY_NOTICES.md vendor/unicode/LICENSE \
    vendor/tree-sitter/tree-sitter/LICENSE vendor/tree-sitter/tree-sitter-c/LICENSE \
    vendor/tree-sitter/tree-sitter/lib/src/unicode/LICENSE \
    vendor/tree-sitter/tree-sitter-cpp/LICENSE; do
    dest="${out}/share/licenses/strictfmt/${license}"
    mkdir -p "$(dirname "${dest}")"
    cp "${license}" "${dest}"
done
{% endblock %}
