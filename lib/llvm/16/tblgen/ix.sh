{% extends '//bin/clang/t/t/ix.sh' %}

{% block fetch %}
{% include '//lib/llvm/16/ver.sh' %}
{% endblock %}

{% block llvm_projects %}
{{super()}}
mlir
{% endblock %}

{% block llvm_targets %}
llvm-config
llvm-tblgen
clang-tblgen
mlir-tblgen
mlir-linalg-ods-yaml-gen
{% endblock %}

{% block env %}
export CMFLAGS="-DLLVM_CONFIG_PATH=${out}/bin/llvm-config -DCLANG_TABLEGEN=${out}/bin/clang-tblgen -DLLVM_TABLEGEN=${out}/bin/llvm-tblgen -DMLIR_TABLEGEN=${out}/bin/mlir-tblgen -DMLIR_LINALG_ODS_YAML_GEN=${out}/bin/mlir-linalg-ods-yaml-gen -DLLVM_USE_HOST_TOOLS=OFF \${CMFLAGS}"
{% endblock %}
