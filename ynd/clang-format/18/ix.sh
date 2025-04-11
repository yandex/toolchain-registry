{% extends '//bin/clang/18/ix.sh' %}

{% block bld_tool %}
lib/llvm/18/tblgen
{{super()}}
{% endblock %}

{% block cmake_flags %}
{{super()}}
LLVM_ENABLE_PROJECTS="llvm;clang;clang-tools-extra"
{% endblock %}

{% block llvm_targets %}
clang-format
{% endblock %}

{% block clang_patches %}
clang-format-patches.patch
asan_static.patch
D21113-case-insesitive-include-paths.patch
clang-format-issue-99758.patch
yt-specific-options.patch
clang-format-better-brakes-in-ctor-init-inheritance-lists.patch
{% endblock %}

{% block common_patches %}
{% endblock %}

{% block llvm_patches %}
vfs-case-insensitive.patch
{% endblock %}

{% block patch %}
{{super()}}
{% for p in self.common_patches().strip().split() %}
(base64 -d | patch -p1) << EOF
{{ix.load_file('//clang/18/patches/clang/' + p) | b64e}}
EOF
{% endfor %}
cd llvm
{% for p in self.llvm_patches().strip().split() %}
(base64 -d | patch -p1) << EOF
{{ix.load_file('//clang/18/patches/llvm/' + p) | b64e}}
EOF
{% endfor %}
cd ..
cd clang
{% for p in self.clang_patches().strip().split() %}
(base64 -d | patch -p1) << EOF
{{ix.load_file('//clang/18/patches/clang/' + p) | b64e}}
EOF
{% endfor %}
{% endblock %}


{% block install %}
{{super()}}
mkdir -p ${out}/fix
cat << EOF > ${out}/fix/remove_unused.sh
rm -rf share
mv bin/clang-format clang-format
rm -rf bin
EOF
{% endblock %}

{% block postinstall %}
:
{% endblock %}

{% block clang_fix_includes %}
:
{% endblock %}
