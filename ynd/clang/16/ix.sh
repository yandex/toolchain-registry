{% extends '//bin/clang/16/ix.sh' %}

{% block bld_tool %}
lib/llvm/16/tblgen
{{super()}}
{% endblock %}

{% block cmake_flags %}
{{super()}}
{% if linux and x86_64 %}
LLVM_ENABLE_PROJECTS="clang;clang-tools-extra;lld;polly"
{% endif %}
LLVM_ENABLE_LIBXML2="yes"
{% endblock %}

{% block llvm_targets %}
{{super()}}
clang-format
clang-rename
{% if linux and x86_64 %}
clang-tidy
{% endif %}
llvm-cov
llvm-profdata
llvm-rc
sancov
{% endblock %}

{% block clang_patches %}
cindex.patch
clang-format-patches.patch
asan_static.patch
D92001.patch
D21113-case-insesitive-include-paths.patch
D142421.patch
fix-build.patch
dwarf-emit-namespaces.patch
{% endblock %}

{% block bld_libs %}
lib/xml/2
{{super()}}
{% endblock %}

{% block common_patches %}
D149723-optimize-renamer-clang-tidy-check.patch
{% endblock %}

{% block llvm_patches %}
spgo-unit32-overflow-fix.patch
vfs-case-insensitive.patch
{% endblock %}

{% block patch %}
{{super()}}
{% for p in self.common_patches().strip().split() %}
(base64 -d | patch -p1) << EOF
{{ix.load_file('patches/clang/' + p) | b64e}}
EOF
{% endfor %}
cd llvm
{% for p in self.llvm_patches().strip().split() %}
(base64 -d | patch -p1) << EOF
{{ix.load_file('patches/llvm/' + p) | b64e}}
EOF
{% endfor %}
cd ..
cd clang
{% for p in self.clang_patches().strip().split() %}
(base64 -d | patch -p1) << EOF
{{ix.load_file('patches/clang/' + p) | b64e}}
EOF
{% endfor %}
{% endblock %}

{% block postinstall %}
:
cd ${out}/bin

{% if linux and x86_64 %}
ln -s llvm-cov llvm-gcov
{% endif %}

{% if mingw32 %}
ln -s llvm-ar.exe llvm-lib.exe
{% else %}
ln -s llvm-ar llvm-lib
{% endif %}

{% endblock %}

{% block clang_fix_includes %}
:
{% endblock %}
