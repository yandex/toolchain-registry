{% extends '//clang/20/template.sh' %}

{% block cmakefiles %}
CMakeLists.txt:CMakeLists.txt
InstrumentationCMakeLists.txt:instrumentation/CMakeLists.txt
SrcCMakeLists.txt:src/CMakeLists.txt
{% endblock %}

{% block patches %}
aflcc.patch
instrumentation.patch
utils.patch
{% endblock %}

{% block src %}
v{{self.version().strip()}}c.tar.gz
{% endblock %}

{% block version %}
4.40
{% endblock %}

{% block llvm_targets %}
afl-cc
afl-fuzz
afl-showmap
afl-tmin
afl-analyze
afl-gotcpu
clang
clang-resource-headers
lld
llvm-ar
llvm-objcopy
{% endblock %}

{% block fetch %}
{{super()}}
{% include '//bin/aflplusplus/ver.sh' %}
{% endblock %}

{% block patch %}
{{super()}}

mkdir -p ${tmp}/aflplusplus && cd ${tmp}/aflplusplus
tar xf ${src}/{{self.src().strip()}} --strip-components=1

{% set ver = self.version().strip() %}
{% for p in self.patches().strip().split() %}
(base64 -d | patch -p1) << EOF
{{ix.load_file('//bin/aflplusplus/' + ver + '/patches/' + p) | b64e}}
EOF
{% endfor %}

{% for mapping in self.cmakefiles().strip().splitlines() %}
{% set s, d = mapping.strip().split(':') %}
base64 -d << EOF > {{d}}
{{ix.load_file('//bin/aflplusplus/' + ver + '/cmake/' + s) | b64e}}
EOF
{% endfor %}

{% endblock %}

{% block cmake_flags %}
{{super()}}
LLVM_EXTERNAL_PROJECTS="AFLplusplus"
LLVM_EXTERNAL_AFLPLUSPLUS_SOURCE_DIR=${tmp}/aflplusplus
LLVM_AFLCLANGLTO_LINK_INTO_TOOLS=ON
LLVM_AFLCLANGFAST_LINK_INTO_TOOLS=ON
{% endblock %}

{% block install %}
{{super()}}

cd ${out}/bin
ln -sf afl-cc afl-clang-lto
ln -sf afl-cc afl-clang-lto++
ln -sf afl-cc afl-clang-fast
ln -sf afl-cc afl-clang-fast++

# glibc 2.38+: _GNU_SOURCE triggers _ISOC2X_SOURCE in features.h,
# causing strtol to be redirected to __isoc23_strtol via __asm__.
for obj in ${out}/lib/afl/afl-compiler-rt.o ${out}/lib/afl/aflpp_driver.o; do
    ${out}/bin/llvm-objcopy \
        --redefine-sym __isoc23_strtol=strtol \
        --redefine-sym __isoc23_strtoll=strtoll \
        --redefine-sym __isoc23_strtoul=strtoul \
        --redefine-sym __isoc23_strtoull=strtoull \
        ${obj}
done

${out}/bin/llvm-ar rc ${out}/lib/afl/libAFLDriver.a ${out}/lib/afl/aflpp_driver.o
rm ${out}/lib/afl/aflpp_driver.o

{% endblock %}

{% block postinstall %}
:
{% endblock %}
