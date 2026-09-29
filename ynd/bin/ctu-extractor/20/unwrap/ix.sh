{% extends '//clang/20/template.sh' %}

# Rebuild against the same Clang 20 source/toolchain package as the compiler,
# exactly as the clang-tidy package does.  NATIVE_CLANG_DIR is exported by
# //clang/20 and supplies the build-host compiler for cross builds.
{% block bld_deps %}
{{super()}}
//clang/20
{% endblock %}

{% block bld_libs %}
{{super()}}
lib/protobuf
lib/zstd
{% endblock %}

{% block bld_tool %}
{{super()}}
ynd/lib/protobuf/protoc
{% endblock %}

{% block llvm_targets %}
ctu-extractor
clang-resource-headers
{% endblock %}

{% block cmake_flags %}
{{super()}}
LLVM_ENABLE_PROJECTS="llvm;clang"
LLVM_EXTERNAL_PROJECTS="ctu_extractor"
LLVM_EXTERNAL_CTU_EXTRACTOR_SOURCE_DIR=${tmp}/ctu-extractor
CLANG_PLUGIN_SUPPORT=FALSE
NATIVE_CLANG_DIR=$NATIVE_CLANG_DIR
{% endblock %}

{% block patch %}
{{super()}}

mkdir -p ${tmp}/ctu-extractor/factspb

{% for source in [
    'CMakeLists.txt',
    'driver.cpp',
    'entity_id.h',
    'fact_visitor.h',
    'fact_visitor_control_flow.cpp',
    'fact_visitor_decls.cpp',
    'fact_visitor_exprs.cpp',
    'fact_visitor_pointers.cpp',
    'source_utils.cpp',
    'source_utils.h',
] %}
base64 -d << 'EOF' > ${tmp}/ctu-extractor/{{source}}
{{ix.load_file('//ynd/bin/ctu-extractor/20/src/' + source) | b64e}}
EOF
{% endfor %}

base64 -d << 'EOF' > ${tmp}/ctu-extractor/factspb/facts.proto
{{ix.load_file('//ynd/bin/ctu-extractor/20/src/facts.proto') | b64e}}
EOF
{% endblock %}

{% block install %}
{{super()}}

# Keep the compiler resource headers installed by clang-resource-headers, but
# discard unrelated LLVM executables from the inherited Clang build.
mkdir -p ${out}/fix
cat << 'EOF' > ${out}/fix/remove_unused.sh
mkdir bin1
mv bin/ctu-extractor* bin1/.
rm -rf bin
mv bin1 bin
EOF
{% endblock %}
