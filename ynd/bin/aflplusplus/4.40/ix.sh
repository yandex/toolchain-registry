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

${out}/bin/clang \
    -O3 -fPIC -std=gnu17 \
    -Wno-unused-result -Wno-deprecated \
    -I${tmp}/aflplusplus/include \
    -I${tmp}/aflplusplus/instrumentation \
    -c ${tmp}/aflplusplus/instrumentation/afl-compiler-rt.o.c \
    -o ${out}/lib/afl/afl-compiler-rt.o

${out}/bin/clang \
    -O0 -flto=full -fPIC -std=gnu17 \
    -Wno-unused-result \
    -I${tmp}/aflplusplus/include \
    -I${tmp}/aflplusplus/instrumentation \
    -c ${tmp}/aflplusplus/instrumentation/afl-llvm-rt-lto.o.c \
    -o ${out}/lib/afl/afl-llvm-rt-lto.o

${out}/bin/clang \
    -O3 -funroll-loops -g -fPIC -fno-lto -std=gnu17 \
    -I${tmp}/aflplusplus/include \
    -I${tmp}/aflplusplus/utils/aflpp_driver \
    -c ${tmp}/aflplusplus/utils/aflpp_driver/aflpp_driver.c \
    -o ${tmp}/aflpp_driver.o

# glibc 2.38+: _GNU_SOURCE triggers _ISOC2X_SOURCE in features.h,
# causing strtol to be redirected to __isoc23_strtol via __asm__.
for obj in ${out}/lib/afl/afl-compiler-rt.o ${tmp}/aflpp_driver.o; do
    ${out}/bin/llvm-objcopy \
        --redefine-sym __isoc23_strtol=strtol \
        --redefine-sym __isoc23_strtoll=strtoll \
        --redefine-sym __isoc23_strtoul=strtoul \
        --redefine-sym __isoc23_strtoull=strtoull \
        ${obj}
done

${out}/bin/llvm-ar rc ${out}/lib/afl/libAFLDriver.a ${tmp}/aflpp_driver.o

${out}/bin/clang \
    -fuse-ld=lld -B${out}/bin \
    -O3 -funroll-loops -std=gnu17 \
    -Wall -Wno-pointer-sign -Wno-variadic-macros -Wno-deprecated \
    -I${tmp}/aflplusplus/include \
    -I${tmp}/aflplusplus/instrumentation \
    '-DAFL_PATH="${out}/lib/afl"' \
    '-DBIN_PATH="${out}/bin"' \
    '-DDOC_PATH="${out}/share/doc/afl"' \
    -o ${out}/bin/afl-fuzz \
    ${tmp}/aflplusplus/src/afl-fuzz.c \
    ${tmp}/aflplusplus/src/afl-fuzz-bitmap.c \
    ${tmp}/aflplusplus/src/afl-fuzz-cmplog.c \
    ${tmp}/aflplusplus/src/afl-fuzz-extras.c \
    ${tmp}/aflplusplus/src/afl-fuzz-frameshift.c \
    ${tmp}/aflplusplus/src/afl-fuzz-ijon.c \
    ${tmp}/aflplusplus/src/afl-fuzz-init.c \
    ${tmp}/aflplusplus/src/afl-fuzz-mutators.c \
    ${tmp}/aflplusplus/src/afl-fuzz-one.c \
    ${tmp}/aflplusplus/src/afl-fuzz-python.c \
    ${tmp}/aflplusplus/src/afl-fuzz-queue.c \
    ${tmp}/aflplusplus/src/afl-fuzz-redqueen.c \
    ${tmp}/aflplusplus/src/afl-fuzz-run.c \
    ${tmp}/aflplusplus/src/afl-fuzz-sanfuzz.c \
    ${tmp}/aflplusplus/src/afl-fuzz-skipdet.c \
    ${tmp}/aflplusplus/src/afl-fuzz-state.c \
    ${tmp}/aflplusplus/src/afl-fuzz-stats.c \
    ${tmp}/aflplusplus/src/afl-fuzz-statsd.c \
    ${tmp}/aflplusplus/src/afl-common.c \
    ${tmp}/aflplusplus/src/afl-sharedmem.c \
    ${tmp}/aflplusplus/src/afl-forkserver.c \
    ${tmp}/aflplusplus/src/afl-performance.c \
    -lm -ldl -lrt

{% endblock %}

{% block postinstall %}
:
{% endblock %}
