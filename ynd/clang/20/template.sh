{% extends '//bin/clang/20/ix.sh' %}

{% block cmake_flags %}
{{super()}}
{% if linux and x86_64 %}
LLVM_ENABLE_PROJECTS="clang;clang-tools-extra;lld;polly"
{% endif %}
LLVM_ENABLE_LIBXML2="FORCE_ON"
{% endblock %}

{% block llvm_targets %}
{{super()}}
clang-format
clang-refactor
{% if linux and x86_64 %}
clang-tidy
{% endif %}
llvm-cov
llvm-profdata
llvm-rc
sancov
{% endblock %}

{% block bld_libs %}
ynd/lib/xml2
{{super()}}
{% endblock %}


{% block clang_patches %}
clang-format-patches.patch
asan_static.patch
D21113-case-insesitive-include-paths.patch
{% endblock %}

{% block common_patches %}
{% endblock %}

{% block llvm_patches %}
vfs-case-insensitive.patch
dont-remove-dbg-info.patch
{% endblock %}

{% block patch %}
{{super()}}
{% for p in self.common_patches().strip().split() %}
(base64 -d | patch -p1) << EOF
{{ix.load_file('//clang/20/patches/clang/' + p) | b64e}}
EOF
{% endfor %}
cd llvm
{% for p in self.llvm_patches().strip().split() %}
(base64 -d | patch -p1) << EOF
{{ix.load_file('//clang/20/patches/llvm/' + p) | b64e}}
EOF
{% endfor %}
cd ..
cd clang
echo $(pwd)
{% for p in self.clang_patches().strip().split() %}
(base64 -d | patch -p1) << EOF
{{ix.load_file('//clang/20/patches/clang/' + p) | b64e}}
EOF
{% endfor %}
cd ..
{% endblock %}

{% block postinstall %}
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
